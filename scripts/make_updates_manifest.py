#!/usr/bin/env python3
"""Build web-flasher/updates.json, the dashboard's update manifest.

The dashboard (web/index.html, served by the device over plain HTTP) checks
for updates from the browser and downloads the image itself, then POSTs it
to the device's /update. Release-asset downloads (github.com ->
release-assets.githubusercontent.com) send no Access-Control-Allow-Origin,
so the browser cannot read them; GitHub Pages does (`*`). The pages workflow
therefore stages every listed *-ota.bin next to this manifest.

Input: the GitHub releases JSON (`gh api repos/OWNER/REPO/releases`) and a
directory holding the downloaded images as <dir>/<tag>/<asset name>.
Output: JSON on stdout. Per release: tag, semver version, prerelease flag,
release-notes URL and one entry per variant (from the asset name
nimble_ble_proxy-<variant>-ota.bin) with path (relative to the manifest),
size, sha256, and the image's own app descriptor version and ELF SHA-256
prefix, which is what GET /appinfo reports once the image runs.

Selection: the newest stable release, plus every prerelease newer than it
(the dashboard shows those only with ?debug-releases=1). Drafts are never
listed: their assets are not public and must not land on Pages.

Every image is checked before it is listed; any failure exits non-zero so
the workflow does not publish a manifest that disagrees with its files:
the GitHub asset digest (when the API reports one) and size match the file,
the ESP image and app-descriptor magics are present, and the project name
is nimble_ble_proxy.

Signed images only (scripts/nbp_sign.py check_release_image): an image
built to accept only signed updates but published unsigned would lock every
device that installs it out of OTA until a USB flash. An image that is not
signed with the pinned release key (RELEASE_KEY_SHA256), or is missing (the
workflow deletes refused files first), is left out of the manifest and
reported on stderr; classic ESP32 images are unsigned by design and pass.
With no release key pinned no signing-capable image is ever listed.
"""
import argparse
import hashlib
import json
import re
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nbp_sign  # noqa: E402

ASSET_RE = re.compile(r"^nimble_ble_proxy-(?P<variant>[a-z0-9]+-[a-z0-9-]+)-ota\.bin$")
SEMVER_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$")
APP_DESC_OFFSET = 32  # esp_image_header_t (24) + first segment header (8)
APP_DESC_MAGIC = 0xABCD5432


def semver_key(tag):
    m = SEMVER_RE.match(tag)
    if not m:
        return None
    major, minor, patch, pre = m.groups()
    # A release sorts after any of its prereleases (semver section 11).
    pre_key = (1,) if pre is None else (0,) + tuple(
        (0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split("."))
    return (int(major), int(minor), int(patch), pre_key)


def app_desc(data):
    if len(data) < APP_DESC_OFFSET + 176 or data[0] != 0xE9:
        raise ValueError("not an ESP app image (magic 0xE9 missing)")
    d = data[APP_DESC_OFFSET:APP_DESC_OFFSET + 176]
    if struct.unpack_from("<I", d, 0)[0] != APP_DESC_MAGIC:
        raise ValueError("app descriptor magic missing")
    cstr = lambda b: b.split(b"\0", 1)[0].decode("ascii", "replace")
    return {
        "version": cstr(d[16:48]),
        "project": cstr(d[48:80]),
        "elf_sha256": d[144:152].hex(),  # the 8 bytes /appinfo prints
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("releases_json")
    ap.add_argument("firmware_dir", help="dir with <tag>/<asset> files")
    ap.add_argument("--repo", required=True)
    ap.add_argument("--list-only", action="store_true",
                    help="print 'tag asset' lines to download, then exit")
    args = ap.parse_args()

    releases = json.loads(Path(args.releases_json).read_text())
    candidates = [r for r in releases
                  if not r.get("draft") and semver_key(r["tag_name"])]
    candidates.sort(key=lambda r: semver_key(r["tag_name"]), reverse=True)
    stable = next((r for r in candidates if not r.get("prerelease")), None)
    chosen = []
    for r in candidates:
        if r is stable:
            chosen.append(r)
            break
        if r.get("prerelease"):
            chosen.append(r)
    if stable is None:
        chosen = [r for r in candidates if r.get("prerelease")][:1]

    if args.list_only:
        for r in chosen:
            for a in r["assets"]:
                if ASSET_RE.match(a["name"]):
                    print(r["tag_name"], a["name"])
        return 0

    out = []
    fw = Path(args.firmware_dir)
    for r in chosen:
        tag = r["tag_name"]
        images = {}
        for a in r["assets"]:
            m = ASSET_RE.match(a["name"])
            if not m:
                continue
            path = fw / tag / a["name"]
            if not path.exists():
                print(f"{tag}/{a['name']}: not staged (refused), not listed", file=sys.stderr)
                continue
            data = path.read_bytes()
            why = nbp_sign.check_release_image(data, nbp_sign.RELEASE_KEY_SHA256, tag)
            if why is not None:
                print(f"{tag}/{a['name']}: {why}; not listed", file=sys.stderr)
                continue
            sha = hashlib.sha256(data).hexdigest()
            if len(data) != a["size"]:
                sys.exit(f"{tag}/{a['name']}: {len(data)} B on disk, "
                         f"release says {a['size']}")
            digest = a.get("digest") or ""
            if digest.startswith("sha256:") and digest[7:] != sha:
                sys.exit(f"{tag}/{a['name']}: sha256 {sha} != release digest {digest}")
            desc = app_desc(data)
            if desc["project"] != "nimble_ble_proxy":
                sys.exit(f"{tag}/{a['name']}: project {desc['project']!r}")
            images[m["variant"]] = {
                "path": f"firmware/{tag}/{a['name']}",
                "size": len(data),
                "sha256": sha,
                "app_version": desc["version"],
                "elf_sha256": desc["elf_sha256"],
            }
        if not images:
            continue
        out.append({
            "tag": tag,
            "version": tag.lstrip("v"),
            "name": r.get("name") or tag,
            "prerelease": bool(r.get("prerelease")),
            "published_at": r.get("published_at"),
            "notes_url": r.get("html_url")
                or f"https://github.com/{args.repo}/releases/tag/{tag}",
            "images": images,
        })

    json.dump({
        "schema": 1,
        "repo": args.repo,
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "releases": out,
    }, sys.stdout, indent=1, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

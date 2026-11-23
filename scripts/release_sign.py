#!/usr/bin/env python3
"""Sign a GitHub release's firmware with the release key and replace the
unsigned CI assets.

CI (.github/workflows/build.yml) builds and attaches UNSIGNED images to the
release for a `v*` tag. This script, run by Fab on his Mac:

  1. downloads the release assets (or takes them from --from-dir),
  2. signs every *-ota.bin of a verifying target (S3/C3/C6) with the release
     key held in the macOS Keychain (never on disk, never in CI),
  3. rebuilds each *-flash.bin from the signed app and the CI's own
     bootloader/partition table/otadata/flash_args, after checking that the
     same merge of the UNSIGNED app reproduces the CI's flash.bin byte for byte,
  4. verifies every signature with espsecure against keys/release.pub.pem,
     whose fingerprint must equal RELEASE_KEY_SHA256 pinned in nbp_sign.py,
  5. prints the plan and, only with --upload and after you type the tag,
     replaces the assets: gh release upload TAG ... --clobber.

    scripts/release_sign.py keygen                 # once: create the release key
    scripts/release_sign.py export-key --out PATH  # offline backup of it
    scripts/release_sign.py sign v0.2.9            # dry run: download, sign, verify
    scripts/release_sign.py sign v0.2.9 --upload   # ... and replace the assets

Run inside the ESP-IDF Python env (cryptography, espsecure, esptool) with an
authenticated `gh`. See docs/signing.md.
"""
import argparse
import base64
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nbp_sign as ns  # noqa: E402

GH_REPO = 'fl4p/nimble-ble-proxy-esphome'
KEYCHAIN_SERVICE = 'nbp-release-signing-key'
KEYCHAIN_ACCOUNT = 'nimble-ble-proxy'
# Default: the login keychain. NBP_RELEASE_KEYCHAIN=<path> selects another
# keychain file (used to test keygen without touching the login keychain).
KEYCHAIN_FILE = os.environ.get('NBP_RELEASE_KEYCHAIN')


def _kc():
    return [KEYCHAIN_FILE] if KEYCHAIN_FILE else []
# Targets whose firmware checks signatures on OTA (sdkconfig.defaults). The
# classic ESP32 does not (sdkconfig.defaults.esp32): its assets are left as
# CI built them and the script says so.
VERIFYING_TARGETS = {'esp32s3', 'esp32c3', 'esp32c6'}
ASSET_RE = re.compile(r'^nimble_ble_proxy-(?P<tv>(?P<target>esp32[a-z0-9]*)-(?P<variant>[a-z0-9-]+))-ota\.bin$')


def die(msg):
    ns.die(msg)


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        die(f'{" ".join(cmd)} failed:\n{r.stdout}{r.stderr}')
    return r.stdout


# --- release key -------------------------------------------------------------

def load_release_key(args):
    """The private key, in memory only. Keychain by default; NBP_RELEASE_KEY_FILE
    (a PEM outside the repo) as the alternative; --test-key for dry runs."""
    if args.test_key:
        if args.upload:
            die('--test-key is for dry runs only')
        return ns.load_private_key(open(os.path.expanduser(args.test_key), 'rb').read())
    path = os.environ.get('NBP_RELEASE_KEY_FILE')
    if path:
        ns.assert_outside_repo(path, 'NBP_RELEASE_KEY_FILE')
        return ns.load_private_key(open(os.path.expanduser(path), 'rb').read())
    r = subprocess.run(['security', 'find-generic-password', '-s', KEYCHAIN_SERVICE,
                        '-a', KEYCHAIN_ACCOUNT, '-w'] + _kc(), capture_output=True, text=True)
    if r.returncode != 0:
        die(f'release key not in the Keychain (service {KEYCHAIN_SERVICE}); run '
            f'`scripts/release_sign.py keygen` or set NBP_RELEASE_KEY_FILE')
    return ns.load_private_key(base64.b64decode(r.stdout.strip()))


def expected_pub(args):
    """(public PEM path, fingerprint) the signatures must verify against."""
    if args.test_key:
        priv = ns.load_private_key(open(os.path.expanduser(args.test_key), 'rb').read())
        td = tempfile.mkdtemp(prefix='nbp-testpub-')
        p = os.path.join(td, 'test.pub.pem')
        open(p, 'wb').write(ns.public_pem(priv.public_key()))
        return p, ns.fingerprint(priv.public_key())
    if not ns.RELEASE_KEY_SHA256:
        die('RELEASE_KEY_SHA256 is not pinned in scripts/nbp_sign.py yet (run keygen first)')
    if not os.path.exists(ns.RELEASE_PUBKEY_PEM):
        die(f'{ns.RELEASE_PUBKEY_PEM} missing')
    fp = ns.fingerprint(ns.load_public_key_file(ns.RELEASE_PUBKEY_PEM))
    if fp != ns.RELEASE_KEY_SHA256:
        die(f'{ns.RELEASE_PUBKEY_PEM} has fingerprint {fp}, pinned is {ns.RELEASE_KEY_SHA256}')
    return ns.RELEASE_PUBKEY_PEM, fp


def cmd_keygen(args):
    _, serialization, _, rsa, _ = ns._crypto()
    if os.path.exists(ns.RELEASE_PUBKEY_PEM) and not args.force:
        die(f'{ns.RELEASE_PUBKEY_PEM} exists; a new key means USB-reflashing every '
            f'release device (docs/signing.md, "Rotate"). Use --force if you mean it.')
    r = subprocess.run(['security', 'find-generic-password', '-s', KEYCHAIN_SERVICE,
                        '-a', KEYCHAIN_ACCOUNT] + _kc(), capture_output=True)
    if r.returncode == 0 and not args.force:
        die(f'a Keychain item {KEYCHAIN_SERVICE} already exists')
    key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
    der = key.private_bytes(serialization.Encoding.DER,
                            serialization.PrivateFormat.PKCS8,
                            serialization.NoEncryption())
    b64 = base64.b64encode(der).decode()
    # `security -i` reads the command from stdin, so the key is never in argv.
    cmd = (f'add-generic-password -U -s {KEYCHAIN_SERVICE} -a {KEYCHAIN_ACCOUNT} '
           f'-l "nimble-ble-proxy release signing key (RSA-3072)" -w {b64}'
           + (f' "{KEYCHAIN_FILE}"' if KEYCHAIN_FILE else '') + '\n')
    r = subprocess.run(['security', '-i'], input=cmd, capture_output=True, text=True)
    if r.returncode != 0 or 'error' in (r.stdout + r.stderr).lower():
        die(f'storing the key in the Keychain failed: {r.stdout}{r.stderr}')
    back = load_release_key(argparse.Namespace(test_key=None, upload=False))
    fp = ns.fingerprint(key.public_key())
    if ns.fingerprint(back.public_key()) != fp:
        die('Keychain read-back does not match the generated key')
    os.makedirs(os.path.dirname(ns.RELEASE_PUBKEY_PEM), exist_ok=True)
    open(ns.RELEASE_PUBKEY_PEM, 'wb').write(ns.public_pem(key.public_key()))
    print(f'Release key stored in the Keychain (service {KEYCHAIN_SERVICE}).')
    print(f'Public key written to {ns.RELEASE_PUBKEY_PEM}  (commit it)')
    print(f'Fingerprint: {fp}')
    print('Pin it: in scripts/nbp_sign.py set')
    print(f"    RELEASE_KEY_SHA256 = '{fp}'")
    print('Then back the key up offline: scripts/release_sign.py export-key --out /Volumes/<USB>/nbp-release-key.pem')


def cmd_export_key(args):
    _, serialization, _, _, _ = ns._crypto()
    ns.assert_outside_repo(args.out, 'backup file')
    if os.path.exists(args.out):
        die(f'{args.out} exists')
    key = load_release_key(argparse.Namespace(test_key=None, upload=False))
    pem = key.private_bytes(serialization.Encoding.PEM,
                            serialization.PrivateFormat.PKCS8,
                            serialization.NoEncryption())
    fd = os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(pem)
    print(f'wrote {args.out} (fingerprint {ns.fingerprint(key.public_key())}). '
          f'Keep it offline; it can sign firmware every release device accepts.')


# --- signing a release -------------------------------------------------------

def parse_flash_args(path):
    lines = [l.strip() for l in open(path) if l.strip()]
    opts = lines[0].split()
    entries = []
    for l in lines[1:]:
        off, f = l.split()
        entries.append((off, f))
    return opts, entries


def merge(target, opts, entries, files, out):
    """esptool merge-bin with the CI's flash_args, files mapped to assets."""
    args = [sys.executable, '-m', 'esptool', '--chip', target, 'merge-bin', '-o', out]
    args += opts
    for off, f in entries:
        base = os.path.basename(f)
        if base not in files:
            die(f'flash_args names {f}, no matching release asset')
        args += [off, files[base]]
    run(args)


def app_offset(entries):
    for off, f in entries:
        if os.path.basename(f) == 'nimble_ble_proxy.bin':
            return int(off, 16)
    die('flash_args has no nimble_ble_proxy.bin entry')


def cmd_sign(args):
    pub_pem, fp = expected_pub(args)
    priv = load_release_key(args)
    if ns.fingerprint(priv.public_key()) != fp:
        die(f'private key fingerprint {ns.fingerprint(priv.public_key())} != expected {fp}')

    work = tempfile.mkdtemp(prefix=f'nbp-release-{args.tag}-')
    src = os.path.join(work, 'ci')
    out = os.path.abspath(args.out or os.path.join(work, 'signed'))
    os.makedirs(out, exist_ok=True)
    if args.from_dir:
        shutil.copytree(args.from_dir, src)
    else:
        os.makedirs(src)
        run(['gh', 'release', 'download', args.tag, '--repo', args.repo, '--dir', src])

    names = sorted(os.listdir(src))
    plan = []
    for n in names:
        m = ASSET_RE.match(n)
        if not m:
            continue
        tv, target = m['tv'], m['target']
        ota = os.path.join(src, n)
        if target not in VERIFYING_TARGETS:
            plan.append((tv, 'skip', f'{target} does not verify signatures; left unsigned'))
            continue
        files = {
            'bootloader.bin': os.path.join(src, f'bootloader-{tv}.bin'),
            'partition-table.bin': os.path.join(src, f'partition-table-{tv}.bin'),
            'ota_data_initial.bin': os.path.join(src, f'ota_data_initial-{tv}.bin'),
        }
        fa = os.path.join(src, f'flash_args-{tv}')
        ci_flash = os.path.join(src, f'nimble_ble_proxy-{tv}-flash.bin')
        for p in list(files.values()) + [fa, ci_flash]:
            if not os.path.exists(p):
                die(f'{tv}: missing asset {os.path.basename(p)}')
        data = open(ota, 'rb').read()
        content, b0 = ns.split_signed(data)
        opts, entries = parse_flash_args(fa)
        if b0 is not None:
            if b0['key_sha256'] != fp:
                die(f'{n} is already signed by key {b0["key_sha256"]}; refusing')
            ns.verify(ota, pub_pem, fp)
            off = app_offset(entries)
            if open(ci_flash, 'rb').read()[off:off + len(data)] != data:
                die(f'{tv}: ota.bin is signed but {os.path.basename(ci_flash)} does not '
                    f'contain it (half-finished earlier upload?). Fix the release by hand.')
            plan.append((tv, 'skip', 'already signed with this key (ota + flash)'))
            continue
        # 1. the CI merged image must be reproducible from its parts
        files['nimble_ble_proxy.bin'] = ota
        check = os.path.join(work, f'{tv}-remerge-unsigned.bin')
        merge(target, opts, entries, files, check)
        if open(check, 'rb').read() != open(ci_flash, 'rb').read():
            die(f'{tv}: merging the CI parts does not reproduce the CI flash.bin; refusing')
        # 2. sign the app, verify it independently
        signed = ns.sign(content, priv)
        signed_path = os.path.join(out, n)
        open(signed_path, 'wb').write(signed)
        ns.verify(signed_path, pub_pem, fp)
        # 3. merged image from the signed app; its app region must be the signed app
        files['nimble_ble_proxy.bin'] = signed_path
        flash_out = os.path.join(out, os.path.basename(ci_flash))
        merge(target, opts, entries, files, flash_out)
        off = app_offset(entries)
        merged = open(flash_out, 'rb').read()
        if merged[off:off + len(signed)] != signed:
            die(f'{tv}: signed app not found at 0x{off:x} in the merged image')
        if merged[:off] != open(ci_flash, 'rb').read()[:off]:
            die(f'{tv}: bootloader/partition region changed by the re-merge')
        plan.append((tv, 'sign', (ota, signed_path, ci_flash, flash_out)))

    if not plan:
        die('no *-ota.bin assets found')
    label = 'TEST KEY (dry run only)' if args.test_key else ns.key_label(fp)
    print(f'\nRelease {args.tag} ({args.repo}), key sha256 {fp} [{label}]\n')
    uploads = []
    for tv, action, info in plan:
        if action == 'skip':
            print(f'  {tv:22s} SKIP  {info}')
            continue
        ota, signed_path, ci_flash, flash_out = info
        print(f'  {tv:22s} SIGN  {os.path.basename(ota)}')
        print(f'  {"":22s}       unsigned {sha(ota)[:16]} -> signed {sha(signed_path)[:16]}')
        print(f'  {"":22s}       {os.path.basename(ci_flash)}: {sha(ci_flash)[:16]} -> {sha(flash_out)[:16]}')
        uploads += [signed_path, flash_out]
    print(f'\nSigned files: {out}')
    if not args.upload:
        print('Dry run: nothing uploaded. Re-run with --upload to replace the release assets.')
        return
    if not uploads:
        print('Nothing to upload.')
        return
    print(f'\nThis replaces {len(uploads)} assets of release {args.tag} on {args.repo} (--clobber).')
    ans = input(f'Type the tag ({args.tag}) to upload: ').strip()
    if ans != args.tag:
        die('not confirmed; nothing uploaded')
    run(['gh', 'release', 'upload', args.tag, '--repo', args.repo, '--clobber'] + uploads)
    print('Uploaded. Publishing to Pages (web flasher + dashboard updates.json): '
          'gh workflow run pages.yml')
    # pages.yml no longer runs after a tag build (it would stage the unsigned CI
    # images); this is what publishes a release. Its gate re-checks every image.
    run(['gh', 'workflow', 'run', 'pages.yml', '--repo', args.repo])


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('keygen', help='create the release key in the Keychain')
    s.add_argument('--force', action='store_true')
    s.set_defaults(fn=cmd_keygen)
    s = sub.add_parser('export-key', help='write the release key to an offline backup file')
    s.add_argument('--out', required=True)
    s.set_defaults(fn=cmd_export_key)
    s = sub.add_parser('sign', help='sign a release (dry run unless --upload)')
    s.add_argument('tag')
    s.add_argument('--repo', default=GH_REPO)
    s.add_argument('--from-dir', help='use local CI-style assets instead of downloading')
    s.add_argument('--out', help='directory for the signed files (default: a temp dir)')
    s.add_argument('--upload', action='store_true', help='replace the release assets')
    s.add_argument('--test-key', help='sign with this PEM instead of the release key (dry run only)')
    s.set_defaults(fn=cmd_sign)
    a = p.parse_args()
    a.fn(a)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Fetch and symbolicate a nimble-ble-proxy panic report.

The firmware keeps the last panics in RTC memory (GET /crash, JSON) and, when
it has a coredump region, a full ELF core dump (GET /coredump, raw bytes).
See components/crash_capture.

    scripts/crash_decode.py --host 192.168.1.231          # fetch + decode
    scripts/crash_decode.py crash.json [coredump.bin]     # decode saved files

ELFs come from the archive (etc/idf-devtools/elf_archive.py, $ELF_ARCHIVE_DIR,
default ./elf-archive): the /crash records carry the first 8 bytes of the
crashed image's app_elf_sha256, the core dump carries all 32. --elf overrides;
app_elf_sha256 is sha256 of the ELF file, so a given ELF is hashed and must
match the record/core, never trusted blindly.

The core dump is only shown if it belongs to the newest record: the firmware
writes a tag (seq, uptime, nonce) into every core and the same nonce into the
record. A core from an older panic, from a panic that recorded nothing, or one
that cannot be tied to a record is refused (exit 2/3) unless --allow-stale /
--allow-unverified.

Needs the IDF tools on PATH (xtensa-esp32s3-elf-addr2line /
riscv32-esp-elf-addr2line, esp-coredump) and zstd.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'etc', 'idf-devtools'))
os.environ.setdefault('ELF_ARCHIVE_DIR', os.path.join(ROOT, 'elf-archive'))
import elf_archive  # noqa: E402

EXIT_MISMATCH = 2    # wrong ELF, or the core is not the newest panic's
EXIT_UNVERIFIED = 3  # the core cannot be tied to any record
TAG_MAGIC = b'NBPCTAG1'


class Fail(Exception):
    def __init__(self, msg, code=EXIT_MISMATCH):
        super().__init__(msg)
        self.code = code


def addr2line_for(elf):
    with open(elf, 'rb') as f:
        hdr = f.read(20)
    machine = int.from_bytes(hdr[18:20], 'little')
    tool = 'xtensa-esp32s3-elf-addr2line' if machine == 94 else 'riscv32-esp-elf-addr2line'
    if machine == 94:
        # any xtensa addr2line reads any ESP xtensa ELF
        for t in ('xtensa-esp32s3-elf-addr2line', 'xtensa-esp32-elf-addr2line',
                  'xtensa-esp-elf-addr2line'):
            if shutil.which(t):
                return t
    if shutil.which(tool):
        return tool
    sys.exit(f'missing tool: {tool} (export the ESP-IDF environment first)')


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


class Elfs:
    """Resolve and verify ELFs; extracted archive copies are removed at close()."""

    def __init__(self, override):
        self.override = override
        self.override_sha = file_sha256(override) if override else None
        self.tmp = {}

    def for_prefix(self, prefix, what):
        prefix = (prefix or '').lower()
        if not re.fullmatch(r'[0-9a-f]{16,64}', prefix) or set(prefix) == {'0'}:
            raise Fail(f'{what} carries no usable ELF sha ({prefix!r}); cannot pick an ELF')
        if self.override:
            if not self.override_sha.startswith(prefix):
                raise Fail(f'--elf {self.override} has sha {self.override_sha[:16]}, '
                           f'but {what} was built from {prefix[:16]}: wrong ELF')
            return self.override
        if prefix[:16] in self.tmp:
            return self.tmp[prefix[:16]]
        try:
            path = elf_archive.extract(prefix)
        except elf_archive.ShaMismatch as e:
            raise Fail(f'{what}: {e}')
        except FileNotFoundError as e:
            raise Fail(f'{what}: {e} (pass --elf)')
        self.tmp[prefix[:16]] = path
        return path

    def close(self):
        for p in self.tmp.values():
            if os.path.exists(p):
                os.unlink(p)


def decode_records(report, elfs):
    recs = report.get('records', [])
    print(f"running image elf {report.get('running_elf')}, "
          f"{report.get('count', 0)} panic(s) since the store was initialised"
          + (f", last reset {report['last_reset_reason']}" if 'last_reset_reason' in report
             else ''))
    if report.get('store_was_valid') is False and report.get('store_state') not in (
            None, 'uninitialised'):
        print(f"  WARNING: the RTC crash store was {report.get('store_state')} at boot; "
              'older records were lost')
    if report.get('crash_without_record'):
        print('  WARNING: the last reset was a crash that left no RTC record')
    for r in recs:
        print(f"\npanic #{r['seq']} at uptime {r['uptime_ms'] / 1000:.3f} s, core {r['core']}, "
              f"task \"{r['task']}\": {r['reason']} ({r['exception']})"
              + ('  [NESTED: a second panic hit during capture/handling]'
                 if r.get('nested') else ''))
        if r.get('details'):
            print(f"  {r['details']}")
        print(f"  pc {r['pc']} addr {r['addr']} cause {r['cause']} elf {r['elf']}"
              + ('  [stack scan, not a call chain]' if r.get('stack_scan') else '')
              + ('  [CORRUPTED]' if r.get('corrupted') else '')
              + ('  [CONTINUES]' if r.get('truncated') else ''))
        pcs = [e.split(':')[0] for e in r['backtrace'].split()]
        if not pcs:
            continue
        elf = elfs.for_prefix(r['elf'], f"panic #{r['seq']}")
        out = subprocess.run([addr2line_for(elf), '-pfiaC', '-e', elf, *pcs],
                             capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            print('  ' + line)


def core_tag(data):
    """(seq, uptime_ms, nonce) from the firmware's CoreTag, or None (older
    firmware). Several copies must agree."""
    tags = set()
    i = data.find(TAG_MAGIC)
    while i >= 0 and i + 20 <= len(data):
        tags.add(struct.unpack_from('<III', data, i + 8))
        i = data.find(TAG_MAGIC, i + 1)
    if len(tags) > 1:
        raise Fail(f'core holds {len(tags)} different crash tags: {sorted(tags)}')
    return tags.pop() if tags else None


def core_sha(data):
    """The 64-hex app_elf_sha256 the ELF-format core stores, or None."""
    m = re.search(rb'(?<![0-9a-f])[0-9a-f]{64}(?![0-9a-f])', data)
    return m.group(0).decode() if m else None


def run_coredump(core, elf):
    p = subprocess.run(['esp-coredump', 'info_corefile', '--core-format', 'raw',
                        '-c', core, elf], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def crashed_task_pc(text):
    name = re.search(r"Crashed task handle: \S+, name: '([^']*)'", text)
    pc = re.search(r'^pc\s+(0x[0-9a-f]+)', text, re.M)
    return (name.group(1) if name else None), (int(pc.group(1), 16) if pc else None)


def check_core(core, report, elfs, args):
    """Return (elf, coredump output) for a core that belongs to the newest
    record; raise Fail otherwise (unless allowed)."""
    data = open(core, 'rb').read()
    sha = elf_archive.sha_from_core(core) or core_sha(data)
    if sha is None:
        raise Fail('the core carries no ELF sha (not an ELF-format core?)')
    elf = elfs.for_prefix(sha, 'the core dump')
    rc, text = run_coredump(core, elf)
    if rc != 0:
        raise Fail(f'esp-coredump failed (exit {rc}):\n{text}')

    recs = report.get('records', []) if report else []
    newest = recs[0] if recs else None
    tag = core_tag(data)
    verdict, code = None, 0
    if newest is None:
        verdict, code = ('no RTC record to tie this core to (store empty, reset or lost); '
                         'it may be from any earlier panic'), EXIT_UNVERIFIED
    elif tag is not None and 'nonce' in newest:
        seq, up, nonce = tag
        if nonce == 0:
            verdict, code = ('the core is from a panic that wrote no RTC record (tag empty); '
                             f"the newest record #{newest['seq']} is a different panic"), \
                EXIT_MISMATCH
        elif f'{nonce:08x}' != newest['nonce'].lower():
            older = [r for r in recs[1:] if r.get('nonce', '').lower() == f'{nonce:08x}']
            verdict = (f'STALE: the core belongs to panic #{older[0]["seq"]}, not the newest '
                       f"record #{newest['seq']}" if older else
                       f'the core (tag seq {seq}, uptime {up} ms, nonce {nonce:08x}) matches no '
                       f"record; the newest is #{newest['seq']} (nonce {newest['nonce']})")
            code = EXIT_MISMATCH
        else:
            print(f"core matches the newest record #{newest['seq']} (nonce {nonce:08x})",
                  file=sys.stderr)
    else:
        # firmware without the tag: compare the crashed task and PC
        task, pc = crashed_task_pc(text)
        rpc = int(newest['pc'], 16)
        if task is None or pc is None:
            verdict, code = 'could not read the crashed task/PC from esp-coredump', \
                EXIT_UNVERIFIED
        elif task[:15] != newest['task'][:15] or pc != rpc:
            verdict, code = (f"the core's crashed task/PC ({task!r}, 0x{pc:08x}) differ from "
                             f"the newest record #{newest['seq']} ({newest['task']!r}, "
                             f"0x{rpc:08x}): STALE or unrelated core"), EXIT_MISMATCH
        else:
            print(f"core matches the newest record #{newest['seq']} by task and PC only "
                  '(older firmware has no core tag; the same crash site twice would also match)',
                  file=sys.stderr)
    if newest is not None and newest.get('nested') and code == 0:
        print('NOTE: the newest record is flagged nested: this core shows the SECOND fault '
              '(inside the capture/panic handler), the record shows the first.', file=sys.stderr)
    if code:
        allowed = args.allow_stale if code == EXIT_MISMATCH else args.allow_unverified
        msg = ('*' * 72 + f'\nCORE DUMP REJECTED: {verdict}\n' + '*' * 72)
        if not allowed:
            raise Fail(msg + '\n(--allow-stale / --allow-unverified to print it anyway)', code)
        print(msg, file=sys.stderr)
    return code, text


def fetch(host, out):
    os.makedirs(out, exist_ok=True)
    jpath = os.path.join(out, 'crash.json')
    with urllib.request.urlopen(f'http://{host}/crash', timeout=20) as r:
        body = r.read()
    report = json.loads(body)  # a truncated reply fails here, before it is saved
    with open(jpath, 'wb') as f:
        f.write(body)
    core = None
    try:
        with urllib.request.urlopen(f'http://{host}/coredump', timeout=60) as r:
            data = r.read()
        core = os.path.join(out, 'coredump.bin')
        with open(core, 'wb') as f:
            f.write(data)
        print(f'saved {jpath}, {core} ({len(data)} B)', file=sys.stderr)
    except urllib.error.HTTPError as e:
        state = (report.get('coredump') or {}).get('state')
        print(f'no valid core dump on the device ({e.code}, state {state})', file=sys.stderr)
        if state in ('invalid', 'bad_checksum'):
            with urllib.request.urlopen(f'http://{host}/coredump?raw=1', timeout=60) as r:
                data = r.read()
            raw = os.path.join(out, 'coredump-raw.bin')
            with open(raw, 'wb') as f:
                f.write(data)
            print(f'saved the unreadable region as {raw} ({len(data)} B) for inspection',
                  file=sys.stderr)
    return jpath, core


def main():
    sys.stdout.reconfigure(line_buffering=True)  # keep order with subprocesses
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('json', nargs='?', help='saved GET /crash output')
    ap.add_argument('core', nargs='?', help='saved GET /coredump output')
    ap.add_argument('--host', help='fetch /crash and /coredump from this device')
    ap.add_argument('--out', default='.', help='where --host saves crash.json/coredump.bin')
    ap.add_argument('--elf', help='use this ELF (hashed and checked against record/core)')
    ap.add_argument('--allow-stale', action='store_true',
                    help='print a core that belongs to another panic than the newest record')
    ap.add_argument('--allow-unverified', action='store_true',
                    help='print a core that cannot be tied to any record')
    args = ap.parse_args()

    if args.host:
        args.json, args.core = fetch(args.host, args.out)
    if not args.json and not args.core:
        ap.error('give --host or a saved crash.json')

    elfs = Elfs(args.elf)
    try:
        report = None
        if args.json:
            with open(args.json) as f:
                report = json.load(f)
            decode_records(report, elfs)
        if args.core:
            print('\n---- core dump ----')
            code, text = check_core(args.core, report, elfs, args)
            print(text, end='')
            return code
        return 0
    except Fail as e:
        print(f'\nERROR: {e}', file=sys.stderr)
        return e.code
    finally:
        elfs.close()


if __name__ == '__main__':
    sys.exit(main())

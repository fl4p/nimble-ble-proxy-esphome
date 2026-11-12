#!/usr/bin/env python3
"""Fetch and symbolicate a nimble-ble-proxy panic report.

The firmware keeps the last panics in RTC memory (GET /crash, JSON) and, when
it has a coredump region, a full ELF core dump (GET /coredump, raw bytes).
See components/crash_capture.

    scripts/crash_decode.py --host 192.168.1.231          # fetch + decode
    scripts/crash_decode.py crash.json [coredump.bin]     # decode saved files

ELFs come from the archive (etc/idf-devtools/elf_archive.py, $ELF_ARCHIVE_DIR,
default ./elf-archive): the /crash records carry the first 8 bytes of the
crashed image's app_elf_sha256, the core dump carries all 32. --elf overrides.
Needs the IDF tools on PATH (xtensa-esp32s3-elf-addr2line /
riscv32-esp-elf-addr2line, esp-coredump) and zstd.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'etc', 'idf-devtools'))
os.environ.setdefault('ELF_ARCHIVE_DIR', os.path.join(ROOT, 'elf-archive'))
import elf_archive  # noqa: E402


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


def elf_for(sha_prefix, override):
    if override:
        return override, False
    try:
        return elf_archive.extract(sha_prefix), True
    except FileNotFoundError as e:
        print(f'  ({e}; pass --elf)', file=sys.stderr)
        return None, False


def decode_records(report, elf_override):
    recs = report.get('records', [])
    print(f"running image elf {report.get('running_elf')}, "
          f"{report.get('count', 0)} panic(s) since power-on")
    for r in recs:
        print(f"\npanic #{r['seq']} at uptime {r['uptime_ms'] / 1000:.3f} s, core {r['core']}, "
              f"task \"{r['task']}\": {r['reason']} ({r['exception']})")
        if r.get('details'):
            print(f"  {r['details']}")
        print(f"  pc {r['pc']} addr {r['addr']} cause {r['cause']} elf {r['elf']}"
              + ('  [stack scan, not a call chain]' if r.get('stack_scan') else '')
              + ('  [CORRUPTED]' if r.get('corrupted') else '')
              + ('  [CONTINUES]' if r.get('truncated') else ''))
        pcs = [e.split(':')[0] for e in r['backtrace'].split()]
        if not pcs:
            continue
        elf, tmp = elf_for(r['elf'], elf_override)
        if not elf:
            print('  Backtrace: ' + r['backtrace'])
            continue
        try:
            out = subprocess.run([addr2line_for(elf), '-pfiaC', '-e', elf, *pcs],
                                 capture_output=True, text=True, check=True).stdout
            for line in out.splitlines():
                print('  ' + line)
        finally:
            if tmp:
                os.unlink(elf)


def decode_core(core, elf_override):
    if not shutil.which('esp-coredump'):
        sys.exit('missing tool: esp-coredump (export the ESP-IDF environment first)')
    if elf_override:
        return subprocess.run(['esp-coredump', 'info_corefile', '--core-format', 'raw',
                               '-c', core, elf_override]).returncode
    return subprocess.run([sys.executable, os.path.join(ROOT, 'etc', 'idf-devtools',
                                                        'elf_archive.py'),
                           'decode', core]).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('json', nargs='?', help='saved GET /crash output')
    ap.add_argument('core', nargs='?', help='saved GET /coredump output')
    ap.add_argument('--host', help='fetch /crash and /coredump from this device')
    ap.add_argument('--out', default='.', help='where --host saves crash.json/coredump.bin')
    ap.add_argument('--elf', help='use this ELF instead of the archive')
    args = ap.parse_args()

    if args.host:
        os.makedirs(args.out, exist_ok=True)
        args.json = os.path.join(args.out, 'crash.json')
        with urllib.request.urlopen(f'http://{args.host}/crash', timeout=20) as r, \
                open(args.json, 'wb') as f:
            f.write(r.read())
        try:
            with urllib.request.urlopen(f'http://{args.host}/coredump', timeout=60) as r:
                data = r.read()
            args.core = os.path.join(args.out, 'coredump.bin')
            with open(args.core, 'wb') as f:
                f.write(data)
            print(f'saved {args.json}, {args.core} ({len(data)} B)', file=sys.stderr)
        except urllib.error.HTTPError as e:
            print(f'no core dump on the device ({e.code})', file=sys.stderr)
    if not args.json and not args.core:
        ap.error('give --host or a saved crash.json')

    if args.json:
        with open(args.json) as f:
            decode_records(json.load(f), args.elf)
    if args.core:
        print('\n---- core dump ----')
        return decode_core(args.core, args.elf)
    return 0


if __name__ == '__main__':
    sys.exit(main())

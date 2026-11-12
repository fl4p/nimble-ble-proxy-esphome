#!/usr/bin/env python3
"""Fail the build unless the esp_panic_handler --wrap is in effect.

Reads the Cross Reference Table that IDF's link writes into the map (--cref):

  __wrap_esp_panic_handler   .../libcrash_capture.a(crash_capture.cpp.obj)   <- defined
                             .../libesp_system.a(panic_handler.c.obj)        <- referenced
  esp_panic_handler          .../libesp_system.a(panic.c.obj)                <- defined
                             .../libcrash_capture.a(crash_capture.cpp.obj)   <- only caller

On failure the ELF is deleted so a rebuild cannot skip the check.
usage: check_panic_wrap.py <map> <elf>
"""
import os
import re
import sys


def cref(path):
    """{symbol: [defining file, referencing files...]} from the map's cref table."""
    table, sym, active = {}, None, False
    with open(path, errors='replace') as f:
        for line in f:
            if not active:
                active = line.startswith('Cross Reference Table')
                continue
            if not line.strip() or line.startswith('Symbol'):
                continue
            m = re.match(r'(\S+)\s+(\S.*)$', line.rstrip('\n'))
            if m and not line[0].isspace():
                sym = m.group(1)
                table[sym] = [m.group(2).strip()]
            elif sym and line[0].isspace():
                table[sym].append(line.strip())
    return table if active else None


def main():
    map_path, elf = sys.argv[1], sys.argv[2]
    problems = []
    t = cref(map_path)
    if t is None:
        problems.append(f'{map_path} has no Cross Reference Table (link without --cref?)')
    else:
        wrap = t.get('__wrap_esp_panic_handler', [])
        if not wrap or 'crash_capture.cpp.obj' not in wrap[0]:
            problems.append('__wrap_esp_panic_handler is not defined by crash_capture.cpp.obj')
        if not any('panic_handler.c.obj' in r for r in wrap[1:]):
            problems.append('panic_handler.c.obj does not reference __wrap_esp_panic_handler: '
                            'the wrap is not in effect and panics would not be captured')
        real = t.get('esp_panic_handler', [])
        bypass = [r for r in real[1:] if 'crash_capture.cpp.obj' not in r]
        if bypass:
            problems.append('esp_panic_handler is called without the wrapper from: '
                            + ', '.join(bypass))
    if problems:
        for p in problems:
            print(f'crash_capture wrap check FAILED: {p}', file=sys.stderr)
        if os.path.exists(elf):
            os.unlink(elf)
        return 1
    print('crash_capture: esp_panic_handler wrap in effect')
    return 0


if __name__ == '__main__':
    sys.exit(main())

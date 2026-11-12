#!/usr/bin/env python3
"""Fail the build unless oom_guard.cpp defines the global operator new family.

Reads the Cross Reference Table in the link map (IDF links with --cref). The
first file listed for a symbol is the one that defines it:

  _ZnwjRKSt9nothrow_t   .../liboom_guard.a(oom_guard.cpp.obj)   <- must be ours

If libstdc++'s definition wins instead, `new (std::nothrow)` calls the
throwing new and abort()s on OOM (no exceptions in this build). The ELF is
deleted on failure so a rebuild cannot skip the check.

size_t is `unsigned int` (j) on both Xtensa and RISC-V ESP32 targets. The
Xtensa link writes mangled names into the table, the RISC-V link demangled
ones ("operator new(unsigned int, std::nothrow_t const&)"), so both spellings
are looked up, and a name that fills the column pushes the file to the next
line.
usage: check_new_override.py <map> <elf>
"""
import os
import sys

# mangled -> demangled
SYMBOLS = {
    '_Znwj': 'operator new(unsigned int)',
    '_Znaj': 'operator new[](unsigned int)',
    '_ZnwjRKSt9nothrow_t': 'operator new(unsigned int, std::nothrow_t const&)',
    '_ZnajRKSt9nothrow_t': 'operator new[](unsigned int, std::nothrow_t const&)',
}


def definers(path, names):
    """{name: defining file} for each name found in the map's cref table,
    or None if the map has no cref table."""
    found, active, pending = {}, False, None
    with open(path, errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if not active:
                active = line.startswith('Cross Reference Table')
                continue
            if pending is not None:
                if line.strip():
                    found[pending] = line.strip()
                pending = None
                continue
            if not line or line[0].isspace():
                continue
            for n in names:
                if line == n:
                    pending = n          # file on the next line
                elif line.startswith(n + ' '):
                    found[n] = line[len(n):].strip()
    return found if active else None


def main():
    map_path, elf = sys.argv[1], sys.argv[2]
    problems = []
    names = list(SYMBOLS) + list(SYMBOLS.values())
    d = definers(map_path, names)
    if d is None:
        problems.append(f'{map_path} has no Cross Reference Table (link without --cref?)')
    else:
        for mangled, demangled in SYMBOLS.items():
            where = d.get(mangled) or d.get(demangled)
            if where is None:
                # Neither spelling in the table: nothing references or
                # defines it. Only plain operator new must be there.
                if mangled == '_Znwj':
                    problems.append(f'{demangled} ({mangled}) not in the map at all')
                continue
            if 'oom_guard.cpp.obj' not in where:
                problems.append(f'{demangled} ({mangled}) is defined by {where}, '
                                'not oom_guard.cpp.obj')
    if problems:
        for p in problems:
            print(f'oom_guard new-override check FAILED: {p}', file=sys.stderr)
        if os.path.exists(elf):
            os.unlink(elf)
        return 1
    print('oom_guard: global operator new family is ours')
    return 0


if __name__ == '__main__':
    sys.exit(main())

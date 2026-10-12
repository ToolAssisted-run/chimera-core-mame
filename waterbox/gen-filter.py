#!/usr/bin/env python3
"""Writes the list of MAME driver files this core builds.

MAME's source says what kind of machine each entry is by the macro that
declares it: GAME and GAMEL are arcade machines, CONS consoles, COMP
computers, SYST everything else. This prints every file under src/mame that
declares at least one arcade machine, in the format MAME's own build takes as
a SOURCEFILTER (paths relative to src/mame, one a line). MAME's build then
works out which processors, chips and devices those files need.

Usage: gen-filter.py <mame tree> [--only a/b.cpp,c/d.cpp]   > arcade.flt
  --only   keep just these files (a small build for working on the core)
"""
import os
import re
import sys

# GAME_CUSTOM is what a few fruit machine files wrap GAME in
ARCADE = re.compile(r'^\s*GAME(L|_CUSTOM)?\s*\(', re.M)
# any machine at all: arcade, console, computer or other
MACHINE = re.compile(r'^\s*(GAMEL?|GAME_CUSTOM|CONS|COMP|SYST)\s*\(', re.M)


def main():
    tree = sys.argv[1]
    only = None
    if len(sys.argv) > 3 and sys.argv[2] == '--only':
        only = set(sys.argv[3].split(','))
    base = os.path.join(tree, 'src', 'mame')
    out = []
    for folder, _, files in os.walk(base):
        for name in files:
            if not name.endswith('.cpp'):
                continue
            path = os.path.join(folder, name)
            rel = os.path.relpath(path, base).replace(os.sep, '/')
            if only is not None and rel not in only:
                continue
            with open(path, errors='replace') as f:
                if ARCADE.search(f.read()):
                    out.append(rel)
    if only is not None and len(out) != len(only):
        sys.exit('not arcade driver files: ' + ', '.join(sorted(only - set(out))))
    # MAME's build finds a driver's other source files by their names: x.cpp
    # brings x_v.cpp, x_m.cpp and x_a.cpp, and any file whose header is
    # included. A file that is none of those is not found, and the link then
    # fails (cave/cv1k_v_blit0.cpp and its eight siblings, which have no
    # header of their own). So: every x_*.cpp beside a driver x.cpp that is
    # not one of the three, has no header of its own name and declares no
    # machine is listed too.
    extra = []
    for rel in out:
        folder, name = os.path.split(os.path.join(base, rel))
        stem = name[:-4]
        for other in os.listdir(folder):
            if not (other.startswith(stem + '_') and other.endswith('.cpp')):
                continue
            if other[len(stem):-4] in ('_v', '_m', '_a') or os.path.exists(os.path.join(folder, other[:-4] + '.h')):
                continue
            with open(os.path.join(folder, other), errors='replace') as f:
                if not MACHINE.search(f.read()):
                    extra.append(os.path.relpath(os.path.join(folder, other), base).replace(os.sep, '/'))
    out = sorted(set(out) | set(extra))
    sys.stdout.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()

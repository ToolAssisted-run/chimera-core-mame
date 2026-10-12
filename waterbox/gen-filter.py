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
    out.sort()
    sys.stdout.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()

# Building

This core is built on Linux. The result, `core.wbx`, is one file that runs in
Chimera on Linux and on Windows.

## What is needed

- GCC 13 or newer, GNU make, Python 3, git.
- A checkout of Chimera with miniBox built, for the sandbox toolchain
  (Chimera's `AGENTS.md`, "Build").
- About 3 GB of disk for the small build described below. The full build's
  needs are measured in `docs/PLAN.md` when it has been done.

## MAME's source

MAME is a git submodule at `extern/mame`, pinned to one commit.

```sh
waterbox/checkout-mame.sh
```

checks out the part of MAME this core builds (`waterbox/mame-sparse.txt`):
everything except the software lists of the home systems, the desktop
renderer, the artwork and the manual. `git submodule update --init
extern/mame` gets the whole tree and works as well.

MAME is never edited in place. A change to it is a numbered patch in
`patches/`, applied by `waterbox/apply-patches.sh`, which the build runs.

## Two sets of machines

MAME's arcade machines are about 36,000 in 1,766 driver files, and building
them takes hours. So there are two builds:

- `-s dev`: the 24 driver files listed in `waterbox/dev-drivers.txt` (about
  1,500 machines). About 15 minutes on 12 processors. This is the build for
  working on the core. Its outputs go to `build/native-dev` and
  `build/guest-dev`.
- `-s all` (the default): every arcade driver file. This is the build that
  is packaged. Its sandbox output goes to `waterbox/bin`.

`waterbox/gen-filter.py` writes the list of driver files: every file under
`src/mame` that declares at least one arcade machine. MAME's own build takes
that list and works out which processors, chips and devices are needed.

## The two flavors

Each set is built twice:

- `native`: for the host. `run-native` is the reference the tests compare
  the sandbox with.
- `guest`: for miniBox's sandbox. This is `core.wbx`, and `run-wbx` runs it.

```sh
waterbox/build-core.sh native -s dev      # build/native-dev/run-native
waterbox/build-core.sh guest -s dev       # build/guest-dev/core.wbx, run-wbx
```

`build-core.sh` builds MAME's libraries first when they are missing or when
what they were built from has changed (`build-mame.sh`), then compiles this
core's own files with exactly the flags MAME compiled its own with
(`mame-flags.sh`), and links.

Only MAME's libraries are built. Its frontend (menus, Lua, plugins), its
SDL layer and its executable are not: `waterbox/mame-driver.cpp` is this
core's frontend.

## Running a game by hand

Put rom sets in `tests/roms-local` (the folder is ignored by git; nothing in
it is ever committed). Then:

```sh
tests/try.sh "ssf2t qsound_hle" --suggest            # what MAME's database says of the game and the files
tests/try.sh "ssf2t qsound_hle" --frames 600 --list-panel
tests/try.sh -g "ssf2t qsound_hle" --frames 600      # the same in the sandbox
tests/try.sh "pong" --frames 600                     # Pong needs no files
```

The first set names the game. The run options are in
`waterbox/gate-harness.h`.

`run-native <work folder> --scan` walks every machine of the build without
running any, and prints which controls are used and anything the control
panel cannot hold.

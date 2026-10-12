# Building

This core is built on Linux. The result, `core.wbx`, is one file that runs in
Chimera on Linux and on Windows.

## What is needed

- GCC 13 or newer, GNU make, Python 3, git.
- A checkout of Chimera with miniBox built, for the sandbox toolchain
  (Chimera's `AGENTS.md`, "Build").
- About 3 GB of disk for the small build described below, 6 GB for the
  full one.

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

MAME's arcade machines are 36,721 in 1,766 driver files. Building them takes
about 20 minutes a flavor on 14 processors, and a public CI runner has 4. So
there are two builds:

- `-s dev`: the 24 driver files listed in `waterbox/dev-drivers.txt` (about
  1,500 machines). A quarter of the files to compile, and a `core.wbx` of
  86 MB against 318 MB, so a change to the core links and packages in
  seconds. This is the build for working on the core, and the one a CI
  runner can afford. Its outputs go to `build/native-dev` and
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
waterbox/build-core.sh native             # build/native/run-native, every machine
waterbox/build-package.sh -r <chimera>    # waterbox/bin/core.wbx and the package
```

`-m <miniBox folder>` (or `MINIBOX_DIR`) says where miniBox is when it is not
in `~/chimera/extern/chimera-common-minibox`.

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

## The tests

```sh
CHIMERA_ROOT=<chimera> waterbox/run-gate.sh           # the full build
CHIMERA_ROOT=<chimera> waterbox/run-gate.sh -s dev    # the small build
```

builds both flavors and the package if needed and runs every test: Pong
(which needs no files), the database's answers, and each game whose sets are
in `tests/roms-local`. A game whose sets are absent is reported as skipped.

## The control panel

The package declares the controls at least one machine has, by MAME's own
names. The list is generated, not written:

```sh
build/native/run-native <a work folder of Pong> --scan > scan.txt
waterbox/gen-config.py scan.txt
```

writes `waterbox/panel.txt` (which the core compiles in),
`waterbox/waterbox.config` and `waterbox/default_keybinds.json`. Do this
again when the MAME commit moves; the test script fails if the three files
are not what the walk gives.

# AGENTS.md - MAME arcade core for Chimera

This repository builds the arcade machines of MAME as a sandboxed guest for
Chimera (https://github.com/ToolAssisted-run/chimera), a frontend for
tool-assisted speedruns. MAME is not forked: its own build system builds its
libraries for the arcade driver files, and `waterbox/` replaces its frontend
and SDL layer. The result is one file, `mame.chimeraCore`: the guest binary
`core.wbx` plus the declarations Chimera reads. `docs/BUILDING.md` has the
detail behind every command here, and `docs/PLAN.md` what was decided, what
was measured and what is not done.

## Layout

- `extern/mame/` - upstream MAME at a pinned commit (submodule). Never commit
  in it.
- `patches/` - numbered patches against `extern/mame`.
- `waterbox/checkout-mame.sh`, `mame-sparse.txt` - the part of MAME that is
  checked out.
- `waterbox/apply-patches.sh` - applies the patches to the MAME tree.
- `waterbox/gen-filter.py` - lists the driver files MAME is asked to build.
  `dev-drivers.txt` is the short list of the small build.
- `waterbox/build-mame.sh` - MAME's libraries, `native` or `guest`,
  `-s all` or `-s dev`.
- `waterbox/build-core.sh` - this repository's code linked against them.
  `mame-flags.sh` reads the flags MAME compiled a driver with.
- `waterbox/build-package.sh` - builds the guest and writes the package.
- `waterbox/mame-driver.cpp`, `mame-driver.h` - the frontend: the machine,
  the picture, the sound, the panel, the database, save data, memory.
- `waterbox/wbx-entry.cpp` - the exports Chimera calls.
- `waterbox/run-native.c`, `run-wbx.c`, `gate-harness.h` - the native
  reference, the sandbox runner, and the options both take.
- `waterbox/gen-config.py` - GENERATES `panel.txt`, `waterbox.config` and
  `default_keybinds.json` (all three committed) from a walk over every
  machine.
- `waterbox/run-gate.sh` - the test script. `tests/run-frontend.sh` - the
  window test. `tests/try.sh` - one game by hand. `tests/make-project.py` -
  writes a Chimera project for a test.
- `waterbox/libco/` - ares's libco. `guest-shim/`, `native-shim/` - headers.
- `waterbox/bin/`, `build/` - build output (ignored by git).
- `tests/roms-local/` - where the tests look for rom sets (ignored by git).

## Build and test

```sh
export MINIBOX_DIR=<chimera>/extern/chimera-common-minibox   # always the same one
waterbox/checkout-mame.sh
waterbox/build-core.sh native -s dev && waterbox/build-core.sh guest -s dev
CHIMERA_ROOT=<chimera> waterbox/run-gate.sh -s dev           # the small build
CHIMERA_ROOT=<chimera> waterbox/run-gate.sh                  # the full build
waterbox/build-package.sh -r <chimera> && bash tests/run-frontend.sh --chimera-root <chimera>
```

Run the test script before every commit that touches what it covers, and say
in the commit what ran, with the numbers. Chimera's own package checks need
`dotnet` on the PATH.

## Rules

- **Never commit inside `extern/mame`.** A change to MAME is a numbered patch
  in `patches/` (paths relative to `extern/mame`, the form `git apply`
  takes), with its code under the `CHIMERA_CORE` define and a comment that
  says why. Two patches may touch one file: write the later one as a diff
  against the tree with the earlier ones applied. `waterbox/apply-patches.sh`
  must then print `already applied: all <n> patches`.
- **Keep `MINIBOX_DIR` the same for every build.** The path of miniBox's
  toolchain is part of what decides whether MAME's sandbox build is still
  valid; another path rebuilds it from nothing.
- **The panel is generated.** After a change to the MAME commit or to how
  controls are found, build the full native core, run `run-native <a work
  folder of Pong> --scan` and give its output to `gen-config.py`. Do not
  edit `panel.txt`, `waterbox.config` or `default_keybinds.json` by hand; the
  test script compares them with what the walk gives.
- **Nothing depends on the host.** No clock, no thread, no file written, no
  input device. A machine must run the same in the native reference, in the
  sandbox, and after a savestate is loaded; the test script checks all
  three for every game it has.
- **No ROM, BIOS or disc image is committed**, uploaded to CI or attached to
  an issue. Rom sets for the tests go in `tests/roms-local`, copied, never
  linked.
- **A test is seen to fail once.** A new test is broken on purpose and seen
  to fail before it is trusted, and a "the same" comparison has a run beside
  it that must come out different.
- **Write in plain language**, in commits, documents and comments: say what
  changed in ordinary words, and explain a term the first time it is used.
  Prose files are ASCII.
- **Commits**: `type(scope): what changed (chimera#164)`, a short body with
  what changed, why and how it was tested. Stage paths by name.
- **Publishing, and anything that changes what Chimera is, is the owner's to
  decide.**

## Traps already met

- `ioport_field::type_class()` and `name()` need a running machine. On a
  machine that was only described (the database walk, `mame_describe`), use
  `specific_name()` and the running machine's `ioport().type_group()`.
- MAME catches what is thrown while it starts a machine (`ui_initialize`,
  `before_load_settings`) and carries on. A reason not to start is put in
  `s_refused`, and `mame_init` answers with it.
- MAME reads every port once a frame for itself. Patch 0005 leaves those
  reads out, or no frame would ever count as a lag frame.
- MAME's dependency finder knows `x_v.cpp`, `x_m.cpp` and `x_a.cpp` beside a
  driver `x.cpp`, and files whose header is included. `gen-filter.py` lists
  the others (today nine Cave files); a link error naming one class is the
  sign of a new one.
- Chimera mounts a project's files in the sandbox's root (`/ssf2t.zip`); the
  test harnesses run in a work folder. The rom path is both.
- A movie carries only the controls the game has. `tests/make-project.py`
  must be given the game's live panel (`--list-panel`).
- A board may take no coin for a long time after power-on (Crypt Killer:
  3,500 frames). A test that puts a coin in too early proves nothing.

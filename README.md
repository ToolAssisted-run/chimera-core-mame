# chimera-core-mame

The arcade machines of [MAME](https://github.com/mamedev/mame) as a
[Chimera](https://github.com/ToolAssisted-run/chimera) core, running in
miniBox's sandbox (chimera issue 164).

MAME is not forked: `extern/mame` is upstream at a pinned commit, built with
MAME's own build system for every driver file that declares an arcade
machine (36,721 machines), with a few small patches in `patches/`. Of that
build only the libraries are used. MAME's own frontend and its SDL layer are
replaced by `waterbox/`: no menus, no Lua, no threads, no host clock, no
files written. A Chimera frame is one turn of MAME's own machine loop, from
one vertical blank of the screen to the next.

## What a project needs

- **Rom sets** (`.zip` or `.7z`). The game's set comes first, with the file
  name MAME uses: `ssf2t.zip` is the machine `ssf2t`. Then the other sets the
  game reads from: its parent's if it is a clone, its board's BIOS set
  (`neogeo.zip`), the sets of the devices it uses (`qsound_hle.zip`).
- **Disc images** (`.chd`) for a game with a hard disk, a CD or a laserdisc.
- **Save data** (optional): the files Export Save Data wrote.

The package carries no ROM and no disc image.

**The core says what is missing.** It carries MAME's own database of every
machine's files. When you have chosen the files, the settings page of the
New Project window shows the game's full name, MAME's notes on how well it
is emulated, the sets the game reads from, and every file that is missing or
wrong with the set it belongs in:

> Super Street Fighter II Turbo (World 940223) (1994, Capcom). It reads 25
> files from ssf2t.zip, qsound_hle.zip (NOT GIVEN). 1 file is missing or
> wrong: dl-1425.bin (in qsound_hle.zip) - missing.

A project with a required file missing does not start, and says the same.
Rom sets change between MAME versions; this core wants the sets of the MAME
version it is built from (`docs/PLAN.md` names it).

## Controls

One panel, `Arcade Panel`: every control at least one arcade machine has, by
MAME's own names - `P1 Up`, `P1 Button 1`, `1 Player Start`, `Coin 1`,
`Service`, `P1 Mahjong A`, and so on. A game shows the controls its board
has; the rest leave the input roll. `Other 1`, `Other 2`... are a game's
controls that have no type of their own (a keypad, a keyboard, a board's odd
switches), in the order MAME lists them.

A control is called what MAME calls its type, not what the game calls it:
Street Fighter's six attacks are `P1 Button 1` to `P1 Button 6`.

An analog control (a paddle, a pedal, a light gun, an analog stick) is an
axis: its value is added to the control's rest position and reaches the game
as it is, with no sensitivity setting and no smoothing. A dial, a trackball
or a mouse takes the value as its movement this frame.

Lag frames are counted: a frame in which the game read none of its controls.

## Settings

The game's own, shown when the game is chosen:

- each **dip switch** of its board (`dip.<name>`) and each **configuration
  switch** of the machine (`config.<name>`), at MAME's defaults;
- the **BIOS** (`bios`), for a machine with more than one.

They are part of the machine: a movie needs the same values.

## Memory

- Each processor's address space is a bus (`maincpu` first): memory at the
  addresses the game itself uses. RAM Search covers its RAM.
- Each named block of the machine's memory is a memory domain under MAME's
  name (`nvram`, `paletteram`, `sprites`...).

## Save data

Every device that keeps something with the power off (an EEPROM, a
battery-backed RAM) exports as one file named after the device
(`eeprom.nv`), in the form MAME writes to its own nvram folder. The Save
data slot puts them back before the first frame. Without them the machine
starts as it left the factory. What a game writes to its hard disk is kept
in memory and is part of a savestate; the disc image is never changed.

## Not there yet

- The recompilers of the MIPS, PowerPC and SH processors run on MAME's
  portable back end, so the 3D boards of the late 1990s are slow.
- A machine with several screens or a cabinet layout is drawn in MAME's
  first view; there is no setting to choose another.
- Only arcade machines: MAME's consoles and computers are not built.

## Using it in Chimera

Chimera ships no cores and downloads nothing. Put the `.chimeraCore` package
in the `Cores` folder beside `Chimera.exe`; File > Core Manager lists it. The
same file works on Linux and on Windows.

## Building

[docs/BUILDING.md](docs/BUILDING.md) has the instructions,
[docs/PLAN.md](docs/PLAN.md) what was decided and measured, and
[AGENTS.md](AGENTS.md) is the guide for an AI coding agent. In short:

```sh
waterbox/checkout-mame.sh                       # MAME at the pinned commit
waterbox/build-core.sh native                   # the reference, every machine
waterbox/build-package.sh -r <chimera>          # core.wbx and the package
CHIMERA_ROOT=<chimera> waterbox/run-gate.sh     # the tests
```

## Licence

The integration in this repository is MIT. MAME as a whole is GPL-2.0 or
later, and the package is distributed under it; `waterbox/package-licenses.json`
lists every component and its terms.

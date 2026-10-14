# The plan

This core is MAME's arcade machines for Chimera (chimera issue 164). It is a
large piece of work, done in steps; this page says what was found before any
code, what was decided, and what each step has to prove. It is updated as the
steps are done.

## What was asked

Issue 164: the newest MAME's arcade machines in Chimera, with RAM Search and
RAM Watch, and without a limit on a game's size (BizHawk's MAME is an older
version and cannot take the largest games). The owner added (2026-10-12):
BizHawk's MAME core is the precedent to read, and the core must use MAME's own
game database to say, before a game starts, which ROM or firmware file a
chosen game is missing.

## What was read (2026-10-12)

- **This project's X68000 core** (`chimera-core-x68k`): MAME built for one
  machine, with MAME's frontend and its OSD layer replaced by a small one of
  our own. Its build scripts, its three patches and its frontend are the
  starting point here.
- **BizHawk's MAME core** (TASEmulators/BizHawk at ab81ea2,
  `src/BizHawk.Emulation.Cores/Arcades/MAME` and `waterbox/mame-arcade`). It
  builds a fork of MAME 0.252 with `SUBTARGET=arcade`, starts it with a
  command line, and asks it everything through MAME's Lua engine. What it
  teaches:
  - a game is known by the name of its zip (`ssf2t.zip` is the machine
    `ssf2t`); BizHawk keeps a text list of every machine name to tell a MAME
    zip from any other zip;
  - a game's files are its own zip, its parent's, its BIOS set's and the sets
    of devices it uses, plus CHD disc images in a folder named after the game;
  - the controls are read from the running machine: every input field that is
    not a switch, named by the driver;
  - the dip switches, the BIOS choice and the screen layout ("view") are
    per-game settings read from the running machine;
  - an analog control is set by writing the field's value directly;
  - a missing file is only learned by starting the machine and reading
    "NOT FOUND" lines from MAME's log.
- **This project's FBNeo core**: how Chimera already handles an arcade
  emulator - a Rom set slot that takes the game's zip and its parent's, a
  panel declared once with the controls a game lacks hidden, and per-game
  settings for dip switches (`SuggestSettings`, `GetGameSettings`).
- **MAME** at the commit pinned here: `SUBTARGET=arcade` no longer exists
  (this is the "change of structure" the issue mentions). MAME now builds
  either everything or a set of driver files given as `SOURCES` or as a
  `SOURCEFILTER` file. Its source declares about 36,000 arcade machines in 1,766
  driver files (of 3,391), by the macro each machine is declared with.

## The eight questions (docs/new-core.md in chimera)

1. **A new core?** Yes. FBNeo covers five boards, Flycast three, DuckStation
   two; MAME covers thousands of boards none of them has.
2. **Licence.** MAME is GPL-2.0-or-later as a whole (BSD-3-Clause and other
   compatible licences file by file). Binary redistribution with source is
   allowed; this repository carries the patches and pins the commit.
3. **One thread on one clock?** Yes: MAME's machine is one scheduler. The
   X68000 core already removed the host from it (no throttle, the sound
   stage not on a thread, a fixed clock date, no files written).
4. **The picture.** MAME's own software renderer, drawing the machine's
   layout into memory. No GPU.
5. **A recompiler?** MAME recompiles MIPS, PowerPC, SH and a few other
   processors. First step: its portable back end (`FORCE_DRC_C_BACKEND`) in
   both the reference and the sandbox build. Whether the x86-64 back end can
   run in the sandbox is a later step, measured; it decides how fast the 3D
   boards of the late 1990s run.
6. **What the user brings.** The game's ROM set, its parent's, the BIOS set
   and device sets it names, and CHD images. Nothing is committed here.
7. **What a public runner can test.** Pong: MAME emulates its circuit and it
   has no ROM at all. The missing-file check is tested with zips the test
   makes itself (wrong or empty files). Games are tested where they are.
8. **Size.** To be measured by the full build: time, the size of
   `core.wbx`, memory. A public runner has 4 processors and a 6 hour limit,
   so CI will probably build the two flavors in separate jobs.

## The pin

MAME 0cacb1a7 (2026-09-28, on its main branch after release 0.289), the
commit the X68000 core builds, so its patches and its sandbox build are
already proven on it. The reporters of issue 164 asked to wait for MAME 0.290
for a Mortal Kombat 4 fix: the first fix (mamedev/mame pull request 16225,
merged 2026-09-23) is in this commit; the second (16325) is still open
upstream. The pin moves to 0.290 when it is released.

## The design

### Which game

The file in the Rom set slot names the machine: `sfiii3.zip` is `sfiii3`. A name MAME does not know is a load error that offers the closest
names (MAME's own approximate match).

### The files, and what is missing

The core carries MAME's driver table: every machine, its ROM files with
sizes and checksums, its parent, its BIOS set, the devices it uses and their
ROM sets, and its disc images. So the core answers from MAME's own data, in
two places:

- **Before the machine exists** (`SuggestSettings`, which Chimera's New
  Project window calls with the chosen files): the game's full name, year
  and maker, MAME's status for it (working, not working, imperfect sound...),
  the sets it reads from (for example `fatfury3.zip` and `neogeo.zip`), and
  for each file that is missing or wrong: its name, which set it belongs in,
  and whether that set was given at all. This is MAME's own audit
  (`media_auditor`), run on the mounted files.
- **At load** (`Init`): the same check; a machine with a required file
  missing does not start, and the load error is the same list.

Files are found by checksum first, by name second (MAME's rule), in a zip or
a 7z. A CHD is found by its name in the Disc slot.

### The controls

MAME names every control by a type and a player (`P1 Button 1`, `P2 Up`,
`Coin 1`, `P1 Mahjong A`, `P1 Paddle`...); a driver only says which of them
its machine has and what the game calls them. Chimera declares a core's
controls once, in the package, and asks the running core which of them this
machine has (`IsButtonActive`, `IsAxisActive`). So:

- the package declares every control type and player that at least one
  arcade machine of this build uses. `gen-config.py` writes that list from
  the database itself (a probe that builds every machine's description
  without running it), so it is exact for the pin and is regenerated when the
  pin moves;
- a running game shows only its own controls;
- two fields of one machine with the same type and player are one control,
  as they are one key in MAME;
- controls with no type (a driver's own named switches, keypads, keyboards)
  take numbered places ("Other 1"...), in the order MAME lists them;
- an analog control is an axis whose value is added to the control's resting
  value and written to the port as it is (no sensitivity, no smoothing): 0 is
  at rest, and the range is the control's own. A dial or a trackball takes
  the axis as its movement this frame.

Where MAME's driver has the game's own name for a control, Chimera shows it
beside the control's name (see "Decided by the owner" below).

### Settings

- Per game, read from the machine's description: each dip switch
  (`dip.<name>`), each machine configuration switch (`config.<name>`), the
  BIOS (`bios`, for a machine with several), the layout view (`view`, for a
  machine with more than one screen or cabinet layout).
- For the package: how large a vector game is drawn, and whether a vertical
  game is turned upright.

### Savestates, save data, memory

- A savestate is the sandbox's memory, so every machine has one, including
  those MAME itself cannot save.
- NVRAM (settings, high scores) is exported and imported as save data, one
  file per NVRAM device.
- RAM Search and RAM Watch: each memory share of the machine is a memory
  domain under MAME's own name, and the main processor's address space is the
  system bus.
- A hard disk a game writes to: MAME keeps the changes in a "diff" file
  beside the image. In the sandbox that file must live in memory. Later step.

## Steps

| Step | What | Proof | State |
|---|---|---|---|
| 1 | A small build (24 driver files, `-s dev`) and the frontend: a game boots, picture, sound, controls | Pong with no files; games from the local folder; a coin gives a credit | done 2026-10-12 |
| 2 | The same in the sandbox | The reference and the sandbox give the same digest, frame by frame | done 2026-10-12 |
| 3 | The missing-file check, per-game settings, BIOS and parent sets | A set with a file taken out names that file before the machine starts | done 2026-10-12 |
| 4 | The full build, both flavors | Time, size, memory measured; the walk over every machine | done 2026-10-12 |
| 5 | The package: controls from the database, Chimera's package checks, a project run by Chimera's engine and opened in its window | The test script and the window test | done 2026-10-12 |
| 6 | CHD games, hard disks, NVRAM as save data | Crypt Killer (a CHD is in the local folder) | done 2026-10-12 |
| 7 | Speed: the recompiler's x86-64 back end in the sandbox | Frames per second measured on a 3D board | not started |
| 8 | Windows, CI, documents, publishing | The owner's word for publishing | Windows and the documents done; the CI workflow is written and has never run; nothing is published |

## Measured (2026-10-12)

The full build:

- 1,775 source files given to MAME (1,766 driver files and nine that MAME's
  dependency finder misses), 5,086 files compiled, 36,721 machines. On 14 of
  this machine's 20 processors MAME's libraries take 21 minutes natively and
  19 minutes for the sandbox. The build folders are 1.8 GB each.
- `core.wbx` is 318 MB; the package is 77 MB.
- The walk over all 36,721 machines takes under 4 seconds and none fails.
  Every control has a place on the panel: 267 of MAME's 513 button types and
  58 of its 180 axis types are used, and the machine with the most controls
  of no type of their own (a roulette table) has 213. So the package
  declares 491 controls and 58 axes. MAME marks 23,064 of the machines as not
  working and 15,928 as having mechanical parts (fruit machines, pinball).
- Seven machines need no files at all (Pong, Pong Doubles, Breakout, Rebound
  and three others): what a public CI runner can run.

The test script (`waterbox/run-gate.sh`), on the full build with the local
sets: 73 passed, 0 failed, 0 skipped. For Super Street Fighter II Turbo
(CPS-2), Fatal Fury 3 (Neo Geo), Shinobi (System 16A), Rampart, Crypt Killer
(Konami GQ, a hard disk image) and Pong (no files):

- the reference and the sandbox give the same digests every 50 frames, with
  controls pressed at random (coins and Start among them);
- a run that saves and loads its state around every frame gives the same
  digests, and so does a state reopened in a new sandbox. States are 5 MB
  (Rampart) to 49 MB (Fatal Fury 3);
- lag frames are counted: 37 of 900 on CPS-2, 282 on the Neo Geo;
- a coin is taken; a missing device set, a missing disc image and an empty
  set are named file by file before and at load; a BIOS setting and a dip
  switch set in the project are the machine's; the EEPROM goes out as save
  data and back in unchanged;
- through Chimera's engine, a project of a rom set and its firmware plays
  a movie whose coin, Start and punch change the picture; Chimera's package
  checks pass.

Also run by hand in the full native build, 1,200 frames with controls
pressed at random: 1944, Daioh, Forgotten Worlds, Hippodrome, Jurassic Park
(System 32), Mach Breakers, Magic Sword, Numan Athletics, Phantasm, Varth,
Kabuki Klash, Samurai Shodown IV and the Konami GV BIOS. Vertical games come
out upright (Varth 224x384).

The window test (`tests/run-frontend.sh`, Chimera.exe under Mono on a
private display), 5 passed. Super Street Fighter II Turbo opens with
`qsound_hle.zip` taken from Chimera's firmware store and runs as the
headless runner runs it; with no such firmware known the project is not
opened and the set is named. And: Pong runs 300 frames in the window with no
files; the keys the package ships are Chimera's; Shinobi's work RAM after
300 frames in the window is what the headless runner leaves.

Windows (the same package, Chimera's headless runner built for Windows, run
on this PC): the picture at the last frame is byte for byte Linux's for
Shinobi (1,200 frames), Super Street Fighter II Turbo (a project of two
sets with a movie, 2,400 frames) and Crypt Killer (a set and a disc image
with a movie, 4,600 frames), and so is Shinobi's exported save data.

Not measured: the speed of a board whose processor MAME recompiles. No
complete set of such a game is in the local folder.

Crypt Killer takes no coin for its first 3,500 frames (it is testing
itself), and it is two coins a credit.

## The patches

| Patch | What |
|---|---|
| 0001 | A file asked for inside a folder (`cryptklr/420uaa04.chd`) is also found beside it: the sandbox has files, no folders. From the X68000 core. |
| 0002 | No SDL and no host clipboard. From the X68000 core. |
| 0003 | A frame's sound is handed over at the frame's end. From the X68000 core. |
| 0004 | What a machine writes to its hard disk is kept in memory, as MAME's own differencing image, and not in a file beside the image. |
| 0005 | MAME tells the frontend of every port read, leaving out its own once-a-frame look at every port: this is how lag frames are counted. |
| 0006 | An analog control given a value takes it as it is. MAME clamps it to the control's range, which goes wrong for a range written as signed. |

## Decided by the owner (2026-10-12)

- **Names of controls.** A control keeps MAME's type name as its identity
  ("P1 Button 1"); the game's own name for it, where MAME's driver has one,
  is a label Chimera shows beside it (hover text of the TAStudio column, the
  key configuration). Chimera learned this for the purpose: the core's
  `GetButtonLabel` and `GetAxisLabel` exports. MAME names controls in some
  drivers only: the Neo Geo's buttons are A to D, Hippodrome's are Attack and
  Jump, and the CPS-2 driver names none.
- **Missing sets are firmware.** The project's file is the game's rom set.
  The other sets it reads from are firmware: the core lists them for the game
  in hand (`"gameFirmware": true`, and a `"firmware"` array in its answer to
  the New Project window), the Firmware page asks for them, and Chimera's
  firmware store remembers them. A set is listed when it holds a file the
  game's own set does not; when that depends on the BIOS (a Neo Geo set that
  carries the default BIOS), the entry carries the BIOS choices it is needed
  with. Disc images stay files of the project: firmware is read into memory.
- **The system name** is `ARCADE`, "Arcade".
- **Nothing is published yet.**

## An axis is fitted to the game (2026-10-13, chimera issue 263)

Reported with a light gun game: the aim only reached the corners of the
screen.

An axis is declared once for all 36,721 machines, so its declared range is
the widest any of them has: "P1 Lightgun X" is -32768 to 32767. Chimera
moves a control over the declared range - the mouse across the picture is
the gun axis from one end to the other. The core added that number to the
machine's rest and clamped it to the machine's own range, which is nearly
always far narrower: Crypt Killer's gun counts 125 to 444 across and 0 to
239 down, so anything past a hundredth of the picture from the middle was
already at the edge. A paddle or an analog stick had the same fault: full
lock for anything past a twitch.

The number is now scaled (`from_wire` in waterbox/mame-driver.cpp). A light
gun is a place on the screen: the declared range is the screen from edge to
edge, and so is the machine's. Any other lever rests where the machine rests
it and reaches the machine's end when the declared range is at its end. A
dial, a trackball and a mouse are movement in the machine's own counts and
are not scaled. The declared range of each axis is compiled in from
waterbox/panel.txt, which gen-config.py now writes with it.

A movie made before this holds numbers that meant something else; the core
was a day old.

**Tests.** `--list-axes` (the core's own runners) prints, for each axis of
the game, the declared range, the machine's, and what the machine is given
at the ends, at rest and half way. Two checks read it: Pong's paddle, with
no files needed, and Crypt Killer's guns where the set is present. Fed a
line as the old code would have printed it (half way giving the same as the
end), the check fails. The Pong check that the paddle moves now sends 20000
of 32767 where it sent 60 of the machine's 255.

**Measured**: the whole test script, 74 passed, 1 failed, 0 skipped. The one
that failed is Chimera's package checks, which could not start on this
computer from inside the script (it found the wrong .NET); run by hand they
pass, 18 of 18. Crypt Killer's guns: 125, 205, 285, 364, 444 across for the
declared range's low end, a quarter, the middle, three quarters and the high
end; 0, 60, 120, 179, 239 down.

**Not proved**: the game in the report (Time Crisis 2 is the reporter's
example for another request; the gun game they tried is not named), and the
aim seen with a mouse in Chimera's window. Each axis keeps the declared
range the walk over all machines gave it, so two players' guns have
different declared ranges (-32768 to 32767 for the first, -1168 to 1169 for
the second); each is fitted to the screen all the same.

## 2026-10-14: the default bindings name keys Chimera sends

Start, Coin and Service were on D1, D2, D5, D6 and D9; they are on Number1 and
so on. Chimera hands a key to the bindings under one name: the name its
DistinctKeyNameOverrides gives it ("Number1", "Enter", "Backtick",
"KeypadAdd") or else its DistinctKey name ("Up"); and unless told otherwise it
reads the left and right Shift, Ctrl and Alt as one key each, "Shift", "Ctrl"
and "Alt" (Input.cs, Config.MergeLAndRModifierKeys). A default binding to
another name never fired, so those controls shipped unbound on the keyboard.
Checked on Xvfb: with 1, Return, left Shift and Up held, Chimera reports
Number1, Enter, Shift and Up. Left and right names stay beside the merged one,
for a setup with the merging off.

#!/usr/bin/env python3
"""Writes the package's control panel from MAME's own database: panel.txt
(the list the core compiles in), waterbox.config (what Chimera is told) and
default_keybinds.json.

MAME names every control by a type and a player. Which of them the arcade
machines of this build use is not written down anywhere, so it is counted:
`run-native <work folder> --scan` walks every machine and prints a line for
each control at least one machine has, and the widest range each analog
control is given. This script turns that list into the three files, so the
panel is exact for the MAME commit the core is pinned to, and is regenerated
when the pin moves. run-gate.sh checks the core's panel against what this
wrote.

usage: waterbox/gen-config.py <scan output> [OUTDIR]
       (default OUTDIR: beside this script)
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PANEL = "Arcade Panel"

# ---- letters and column headers -------------------------------------------
# A control's letter is the one character it writes into a movie; its header
# is what heads its column. Keyed by MAME's token without the player.
PLAYER_LETTERS = {
    "JOYSTICK_UP": "U", "JOYSTICK_DOWN": "D", "JOYSTICK_LEFT": "L", "JOYSTICK_RIGHT": "R",
    "JOYSTICKLEFT_UP": "^", "JOYSTICKLEFT_DOWN": "v", "JOYSTICKLEFT_LEFT": "<", "JOYSTICKLEFT_RIGHT": ">",
    "JOYSTICKRIGHT_UP": "i", "JOYSTICKRIGHT_DOWN": "k", "JOYSTICKRIGHT_LEFT": "j", "JOYSTICKRIGHT_RIGHT": "o",
    "BUTTON1": "1", "BUTTON2": "2", "BUTTON3": "3", "BUTTON4": "4", "BUTTON5": "5", "BUTTON6": "6",
    "BUTTON7": "7", "BUTTON8": "8", "BUTTON9": "9", "BUTTON10": "0", "BUTTON11": "a", "BUTTON12": "b",
    "BUTTON13": "c", "BUTTON14": "d", "BUTTON15": "e", "BUTTON16": "f",
    "START": "S", "SELECT": "s",
}
PLAYER_HEADERS = {
    "JOYSTICKLEFT_UP": "LS-U", "JOYSTICKLEFT_DOWN": "LS-D", "JOYSTICKLEFT_LEFT": "LS-L", "JOYSTICKLEFT_RIGHT": "LS-R",
    "JOYSTICKRIGHT_UP": "RS-U", "JOYSTICKRIGHT_DOWN": "RS-D", "JOYSTICKRIGHT_LEFT": "RS-L", "JOYSTICKRIGHT_RIGHT": "RS-R",
    "START": "Start", "SELECT": "Select",
}
# the words of a token, shortened for a header
SHORT = {
    "MAHJONG": "Mj", "HANAFUDA": "Hf", "GAMBLE": "", "POKER": "Pk", "SLOT": "Sl", "BUTTON": "B", "KEYPAD": "Kp",
    "CANCEL": "Cncl", "PAYOUT": "Pay", "SERVICE": "Svc", "VOLUME": "Vol", "DOWN": "-", "UP": "+", "STAND": "Stnd",
    "DOUBLE": "Dbl", "FLIP": "Flp", "FLOP": "Flp", "CHANCE": "Chnc", "SCORE": "Scr", "REACH": "Rch", "KEYIN": "KeyIn",
    "KEYOUT": "KeyOut", "DEAL": "Deal", "TAKE": "Take", "HALF": "Half", "HIGH": "Hi", "LOW": "Lo", "BOOK": "Book",
    "DOOR": "Door", "STOP": "Stop", "HOLD": "Hold", "BET": "Bet", "LAST": "Last", "SMALL": "Sml", "BIG": "Big",
    "COIN": "Coin", "START": "St", "BILL": "Bill", "TILT": "Tilt", "INTERLOCK": "Lock", "MEMORY": "Mem", "RESET": "Rst",
    "POWER": "Pwr", "ALL": "All", "LEFT": "L", "RIGHT": "R", "ON": "On", "OFF": "Off", "YES": "Yes", "NO": "No",
}
AXIS_SHORT = {
    "PEDAL": "Pd1", "PEDAL2": "Pd2", "PEDAL3": "Pd3", "PADDLE": "Pdl", "PADDLE_V": "PdV", "POSITIONAL": "Pos",
    "POSITIONAL_V": "PoV", "DIAL": "Dl", "DIAL_V": "DlV", "TRACKBALL_X": "TbX", "TRACKBALL_Y": "TbY",
    "AD_STICK_X": "AX", "AD_STICK_Y": "AY", "AD_STICK_Z": "AZ", "LIGHTGUN_X": "GnX", "LIGHTGUN_Y": "GnY",
    "MOUSE_X": "MsX", "MOUSE_Y": "MsY",
}
# what the first two players' common controls are bound to out of the box
# the digit keys by the names Chimera hands to bindings (Number1, not D1)
KEYS = {
    "P1 Up": "Up, J1 POV1U, X1 DpadUp, X1 LStickUp", "P1 Down": "Down, J1 POV1D, X1 DpadDown, X1 LStickDown",
    "P1 Left": "Left, J1 POV1L, X1 DpadLeft, X1 LStickLeft", "P1 Right": "Right, J1 POV1R, X1 DpadRight, X1 LStickRight",
    "P1 Button 1": "Z, J1 B1, X1 X", "P1 Button 2": "X, J1 B2, X1 A", "P1 Button 3": "C, J1 B3, X1 B",
    "P1 Button 4": "A, J1 B4, X1 Y", "P1 Button 5": "S, J1 B5, X1 LeftShoulder", "P1 Button 6": "D, J1 B6, X1 RightShoulder",
    "1 Player Start": "Number1, J1 B10, X1 Start", "Coin 1": "Number5, J1 B9, X1 Back",
    "P2 Up": "J2 POV1U, X2 DpadUp, X2 LStickUp", "P2 Down": "J2 POV1D, X2 DpadDown, X2 LStickDown",
    "P2 Left": "J2 POV1L, X2 DpadLeft, X2 LStickLeft", "P2 Right": "J2 POV1R, X2 DpadRight, X2 LStickRight",
    "P2 Button 1": "J2 B1, X2 X", "P2 Button 2": "J2 B2, X2 A", "P2 Button 3": "J2 B3, X2 B",
    "P2 Button 4": "J2 B4, X2 Y", "P2 Button 5": "J2 B5, X2 LeftShoulder", "P2 Button 6": "J2 B6, X2 RightShoulder",
    "2 Players Start": "Number2, J2 B10, X2 Start", "Coin 2": "Number6, J2 B9, X2 Back",
    "Service": "F2", "Service 1": "Number9",
}
SPARE = "ABCEFGHIJKMNOPQTVWXYZghlmnpqrtuwxyz!#$%&*+-=?@~;:,/"


def bare(token):
    """P3_BUTTON2 -> (3, BUTTON2); COIN1 -> (0, COIN1)."""
    m = re.match(r"P(\d+)_(.+)$", token)
    return (int(m.group(1)), m.group(2)) if m else (0, token)


def short_header(rest):
    words = re.findall(r"[A-Z]+|\d+", rest)
    out = "".join(SHORT.get(w, w.capitalize() if w.isalpha() else w) for w in words)
    return out[:8] or rest[:8]


def read_scan(path):
    buttons, axes, summary = [], [], {}
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("used "):
            token, name, count, player = line[5:].split("\t")
            buttons.append({"token": token, "mame": name, "machines": int(count)})
        elif line.startswith("usedaxis "):
            token, name, count, player, lo, hi = line[9:].split("\t")
            axes.append({"token": token, "mame": name, "machines": int(count), "lo": int(lo), "hi": int(hi)})
        elif line.startswith("summary "):
            words = line.split()
            summary = {words[i]: words[i + 1] for i in range(1, len(words) - 1, 2) if not words[i + 1].startswith("(")}
            summary["mostother"] = int(re.search(r"mostother (\d+)", line).group(1))
            summary["machines"] = int(re.search(r"machines (\d+)", line).group(1))
            summary["unplaced"] = int(re.search(r"unplaced (\d+)", line).group(1))
    if not summary:
        sys.exit("the scan has no summary line: is it the output of run-native --scan?")
    if summary["unplaced"]:
        sys.exit("the scan found %d controls the panel has no place for" % summary["unplaced"])
    return buttons, axes, summary


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    buttons, axes, summary = read_scan(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else HERE
    # room for the machine with the most controls of no type of their own
    others = max(16, (summary["mostother"] + 15) // 16 * 16)

    names, letters, headers = [], {}, {}
    taken = {}      # player -> letters in use
    heads = {}      # player -> headers in use
    for b in buttons:
        player, rest = bare(b["token"])
        name = b["mame"].replace("/", " ")
        b["name"] = name
        names.append(name)
        used = taken.setdefault(player, set())
        letter = PLAYER_LETTERS.get(rest) if player else None
        if letter is None or letter in used:
            letter = next((ch for ch in SPARE if ch not in used), "?")
        used.add(letter)
        letters[name] = letter
        if not (player and rest in PLAYER_LETTERS and rest not in PLAYER_HEADERS):
            head = PLAYER_HEADERS.get(rest) if player else None
            head = head or short_header(rest)
            seen = heads.setdefault(player, set())
            base, n = head, 2
            while head in seen:
                tail = str(n)
                head = base[:8 - len(tail)] + tail
                n += 1
            seen.add(head)
            headers[name] = head
    for i in range(others):
        name = "Other %d" % (i + 1)
        names.append(name)
        letters[name] = SPARE[i % len(SPARE)]
        headers[name] = "O%d" % (i + 1)

    axis_decl = []
    for a in axes:
        player, rest = bare(a["token"])
        # MAME: "P1 Pedal 2" (the first player's second pedal), but "Paddle"
        # and "Paddle 2" (the first and the second player's paddle)
        if re.match(r"P\d+ ", a["mame"]):
            name = a["mame"]
        else:
            base = a["mame"][:-len(" %d" % player)] if player > 1 and a["mame"].endswith(" %d" % player) else a["mame"]
            name = "P%d %s" % (player, base)
        a["name"] = name
        axis_decl.append({"name": name, "min": a["lo"], "max": a["hi"], "neutral": 0,
                          "header": ("%d%s" % (player, AXIS_SHORT.get(rest, rest[:4].capitalize())))[:5]})
    assert len(set(names)) == len(names), "two controls share a name"
    assert len({a["name"] for a in axis_decl}) == len(axis_decl), "two axes share a name"

    with open(os.path.join(out, "panel.txt"), "w") as f:
        f.write("# The controls the package declares: every control at least one of the\n"
                "# %d machines of the build has. Written by gen-config.py from\n"
                "# run-native --scan; the core compiles this list in. Do not edit.\n" % summary["machines"])
        for b in buttons:
            f.write("button %s\n" % b["token"])
        f.write("others %d\n" % others)
        # an axis with the range the package declares for it: the core turns a
        # value in that range into the running machine's own (mame-driver.cpp)
        for a in axes:
            f.write("axis %s %d %d\n" % (a["token"], a["lo"], a["hi"]))

    config = {
        "coreName": "MAME",
        "systemId": "ARCADE",
        "systemNames": {"ARCADE": "Arcade"},
        "author": "The MAME team; chimera port by Sergio Martin",
        "url": "https://github.com/ToolAssisted-run/chimera-core-mame",
        "romFile": "romset",
        "deterministic": True,
        "suggestSettings": True,
        "gameFirmware": True,
        "memoryLayoutMiB": [2048, 8, 8, 64, 2048],
        "_memoryLayoutMiB_note": "sbrk, sealed, invisible, plain, mmap. This is room, not use: memory is taken as a machine touches it. MAME holds a game's ROMs in memory whole, and some boards' are hundreds of megabytes; a small board's savestate is 5 to 50 MB.",
        "video": {
            "_comment": "The BUFFER CAPACITY. Every machine has its own picture size, so the live size comes from GetVideoWidth/Height every frame. A machine with one screen is handed out at that screen's own pixels and shown in the shape its monitor had (4:3, or 3:4 for a vertical game, which is handed out upright).",
            "width": 2048, "height": 2048, "virtualWidth": 640, "virtualHeight": 480,
            "vsyncNumerator": 60000, "vsyncDenominator": 1000,
            "_vsync_note": "Every machine has its own refresh rate; GetVsyncNumerator reports the screen's, in thousandths of a hertz.",
            "getBgra": "GetVideoBgra",
        },
        "audio": {
            "_comment": "MAME's mix of the machine's sound, 48 kHz stereo, all of a frame's at its end.",
            "rate": 48000, "samplesPerFrame": 4096, "channels": 2, "get": "GetAudio",
        },
        "lag": {"inputWasRead": "InputWasRead"},
        "extensions": {},
        "input": {
            "name": PANEL,
            "_comment": "Every control at least one arcade machine of this build has, by MAME's own names: a type and a player. A game shows the ones its board has; the rest leave the input roll. 'Other 1' and up are a game's controls that have no type of their own (a keypad, a keyboard, a board's odd switches), in the order MAME lists them. An analog control's value is added to its rest position and written to the game as it is; a dial or a trackball takes it as the movement this frame.",
            "buttons": names,
            "mnemonics": letters,
            "headers": headers,
            "axes": axis_decl,
        },
        "settings": [],
        "firmware": [],
    }
    with open(os.path.join(out, "waterbox.config"), "w") as f:
        json.dump(config, f, indent=1)
        f.write("\n")

    binds = {
        "_comment": ["Player 1 on the arrows and Z X C A S D as well as the first pad, player 2 on the second pad; 1 and 2 start, 5 and 6 are the coins, F2 is the service switch. Everything else ships unbound: bind what the game you play has."],
        "AllTrollers": {PANEL: {n: KEYS.get(n, "") for n in names}},
        "AllTrollersAutoFire": {PANEL: {}},
        "AllTrollersAnalog": {PANEL: {}},
    }
    with open(os.path.join(out, "default_keybinds.json"), "w") as f:
        json.dump(binds, f, indent=1)
        f.write("\n")
    print("%d buttons (%d of them 'Other'), %d axes, from %d machines" % (len(names), others, len(axis_decl), summary["machines"]))


if __name__ == "__main__":
    main()

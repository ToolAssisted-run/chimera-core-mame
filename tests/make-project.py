#!/usr/bin/env python3
"""Writes a .chimeraProject for a game, the way the New Project window
would: the files in their slots, the settings, and an input log in the
panel's own control names.

usage: make-project.py <package> <out.chimeraProject> <frames>
                       [<setting>=<value> ...] [press=<control>:<first>:<count> ...]
                       -- <slot=file> [<slot=file> ...]

The first file of the romset slot names the game. A setting is one of the
game's own ("dip.Lives=5", "bios=...").

A movie carries only the controls the game has, so the script must be told
which they are: panel=<file> is the output of the test harness's
--list-panel for the same game (tests/try.sh "<sets>" --frames 1 --list-panel).
"""
import hashlib
import json
import os
import sys
import zipfile


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 24), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def holds(cond, settings):
    """the condition language of docs/project.md, the setting tests only"""
    if cond is None:
        return True
    if "all" in cond:
        return all(holds(c, settings) for c in cond["all"])
    if "any" in cond:
        return any(holds(c, settings) for c in cond["any"])
    if "not" in cond:
        return not holds(cond["not"], settings)
    if "setting" in cond:
        have = settings.get(cond["setting"])
        if "is" in cond:
            return have == cond["is"]
        return have in cond.get("in", [])
    return False


def player_of(name):
    return int(name[1]) if len(name) > 2 and name[0] == "P" and name[1].isdigit() else 0


def main():
    package, out, frames = sys.argv[1], sys.argv[2], int(sys.argv[3])
    rest = sys.argv[4:]
    sep = rest.index("--")
    options, slotargs = rest[:sep], rest[sep + 1:]

    z = zipfile.ZipFile(package)
    cfg = json.loads(z.read("waterbox.config"))
    buttons = cfg["input"]["buttons"]
    axes = cfg["input"].get("axes", [])

    presses = {}
    overrides = []
    for o in options:
        k, v = o.split("=", 1)
        if k == "panel":
            live, live_axes = set(), set()
            for line in open(v):
                word = line.split()
                if len(word) >= 3 and word[0] == "panel" and word[-1] == "active":
                    live.add(line[line.index("'") + 1:line.rindex("'")])
                elif len(word) == 3 and word[0] == "axis" and word[2] == "active":
                    live_axes.add(int(word[1]))
            buttons = [b for b in buttons if b in live]
            axes = [a for i, a in enumerate(axes) if i in live_axes]
        elif k == "press":
            control, first, count = v.rsplit(":", 2)
            presses[control] = (int(first), int(count))
        else:
            overrides.append((k, v))

    groups = max([player_of(a["name"]) for a in axes] + [player_of(b) for b in buttons] + [0]) + 1
    key = ""
    for g in range(groups):
        key += "#"
        for a in axes:
            if player_of(a["name"]) == g:
                key += a["name"] + "|"
        for b in buttons:
            if player_of(b) == g:
                key += b + "|"
    rows = []
    for f in range(frames):
        row = ""
        for g in range(groups):
            row += "|"
            for a in axes:
                if player_of(a["name"]) == g:
                    row += "%5d," % a.get("neutral", 0)
            for b in buttons:
                if player_of(b) == g:
                    first, count = presses.get(b, (0, 0))
                    row += "X" if first <= f < first + count else "."
        rows.append(row + "|")
    log = "[Input]\nLogKey:" + key + "\n" + "\n".join(rows) + "\n[/Input]\n"

    files = []
    for arg in slotargs:
        slot, path = arg.split("=", 1)
        files.append({"name": os.path.basename(path), "sha1": sha1(path), "slot": slot})

    settings = {}
    for d in cfg.get("settings", []):
        if d["name"] not in settings and d.get("default") is not None and holds(d.get("exposedWhen"), settings):
            settings[d["name"]] = d["default"]
    for k, v in overrides:
        settings[k] = v

    firmware = [{"id": d["id"], "sha1": d.get("sha1", "")}
                for d in cfg.get("firmware", []) if holds(d.get("requiredWhen"), settings)]

    project = {
        "id": "mame-test-%s" % os.path.splitext(files[0]["name"])[0],
        "title": "%s through Chimera" % files[0]["name"],
        "description": "written by tests/make-project.py",
        "core": {"name": cfg["coreName"], "version": cfg["version"], "sha1": sha1(package)},
        "rerecords": 0,
        "files": files,
        "settings": settings,
        "firmware": firmware,
        "coreCache": [],
        "input": log,
        "markers": [],
        "branches": [],
        "headers": {
            "MovieVersion": "Chimera Project File v1.1",
            "Platform": cfg["systemId"],
            "SHA1": files[0]["sha1"] if files else "",
            "LastInputFrame": str(frames - 1),
            "VsyncNumerator": str(cfg["video"]["vsyncNumerator"]),
            "VsyncDenominator": str(cfg["video"]["vsyncDenominator"]),
        },
    }
    with open(out, "w") as f:
        json.dump(project, f, indent="\t")
    return 0


if __name__ == "__main__":
    sys.exit(main())

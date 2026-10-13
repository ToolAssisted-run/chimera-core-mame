#!/bin/sh
# The MAME arcade core's test script.
#
# Written against chimera's docs/gates.md, and the ways a test script has
# already been watched to pass on a broken thing:
#
#   * a DEAD machine finishes at once and every digest agrees with itself, so
#     the machine tests first check that the machine got somewhere;
#   * a digest of the END misses a machine that ran differently and arrived
#     at the same place, so the reference and the sandbox are compared as a
#     STREAM of digests, one every 50 frames;
#   * a comparison that cannot fail proves nothing, so every "the same" has a
#     run beside it that must come out DIFFERENT through the same comparison;
#   * a sandbox build that failed was once packaged as the previous binary,
#     so the package test checks core.wbx is newer than its sources.
#
# Pong needs no files (MAME emulates its circuit), so the tests that use it
# run anywhere, CI included. The tests of real games need rom sets, which
# cannot be distributed: put them in tests/roms-local, or point MAME_ROMS at
# a folder holding them. Each game test says SKIP when its sets are absent.
#
# Usage: waterbox/run-gate.sh [-s all|dev] [-r <chimera root>] [-m <miniBox dir>]
#   -s dev   test the small build (build-core.sh)
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
chimera_root="${CHIMERA_ROOT:-$HOME/chimera}"
mb="${MINIBOX_DIR:-}"
roms="${MAME_ROMS:-$root/tests/roms-local}"
set_="all"
while getopts "r:m:s:" opt; do
	case "$opt" in
		r) chimera_root="$OPTARG" ;;
		m) mb="$OPTARG" ;;
		s) set_="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
[ -n "$mb" ] || mb="$chimera_root/extern/chimera-common-minibox"
case "$set_" in
	all) tail=""; bin="$here/bin" ;;
	dev) tail="-dev"; bin="$root/build/guest-dev" ;;
	*) echo "-s is all or dev" >&2; exit 2 ;;
esac

work="$root/build/gate$tail"
rm -rf "$work"; mkdir -p "$work"
native="$root/build/native$tail/run-native"
wbxrun="$bin/run-wbx"
core="$bin/core.wbx"
run="$chimera_root/build/meson-linux/chimera-run"
pkg="$root/build/package$tail/mame.chimeraCore"
config="$here/waterbox.config"

pass=0; fail=0; skip=0
report() {
	case "$1" in
		PASS) pass=$((pass+1)) ;;
		FAIL) fail=$((fail+1)) ;;
		SKIP) skip=$((skip+1)) ;;
	esac
	printf '%-5s %-58s %s\n' "$1" "$2" "${3:-}"
}
finish() {
	printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
	[ "$pass" -eq 0 ] && { echo "NOTHING RAN"; exit 1; }
	[ "$fail" -eq 0 ] || exit 1
	exit 0
}
# check <label> <detail> <condition...>: PASS or FAIL on a shell condition
check() {
	ck_label="$1"; ck_detail="$2"; shift 2
	if "$@"; then report PASS "$ck_label" "$ck_detail"; else report FAIL "$ck_label" "$ck_detail"; fi
}
stream() { awk '/^frame/' "$1"; }
same() { [ -s "$1" ] && [ -s "$2" ] && cmp -s "$1" "$2"; }
differ() { [ -s "$1" ] && [ -s "$2" ] && ! cmp -s "$1" "$2"; }
# field <report line> <name>: the value after a name in a report line
field() { echo "$1" | awk -v f="$2" '{for (i=1;i<=NF;i++) if ($i==f) {print $(i+1); exit}}'; }
# have <set>...: are all these files in the roms folder
have() {
	for hv in "$@"; do [ -f "$roms/$hv" ] || return 1; done
	return 0
}
# folder <name> <settings json> <file>...: a work folder as the sandbox mounts
# a project - the files under their names. The first is the game's rom set,
# the one file of the Rom set slot; the other sets are there as a project's
# firmware is: mounted under MAME's names for them
folder() {
	fd_dir="$work/$1"; fd_settings="$2"; shift 2
	mkdir -p "$fd_dir"
	fd_sets=""; fd_saves=""
	for fd_file in "$@"; do
		case "$fd_file" in
			*.nv) cp "$fd_file" "$fd_dir/"; fd_saves="$fd_saves${fd_saves:+,}\"$(basename "$fd_file")\"" ;;
			*) [ -f "$roms/$fd_file" ] && cp "$roms/$fd_file" "$fd_dir/"
				[ -n "$fd_sets" ] || fd_sets="\"$fd_file\"" ;;
		esac
	done
	printf '{"romset":[%s],"savedata":[%s]}' "$fd_sets" "$fd_saves" > "$fd_dir/slots"
	printf '%s' "$fd_settings" > "$fd_dir/settings"
}
# firmware_of <answer to the New Project window>: the firmware ids the core
# lists for the game, a "?" after one that is needed with some settings only
firmware_of() { python3 -c "
import json, sys
t = open(sys.argv[1]).read()
print(' '.join(f['id'] + ('?' if 'requiredWhen' in f else '') for f in json.loads(t[t.index('{'):]).get('firmware', [])))" "$1" 2> /dev/null; }
# index <panel listing> <control name>: a control's place on the panel
index() { awk -v n="'$2'" 'index($0, n) && $1=="panel" {print $2; exit}' "$1"; }

# ---------------------------------------------------------------- 1. build
if sh "$here/build-core.sh" native -s "$set_" -m "$mb" > "$work/native.log" 2>&1; then
	report PASS "the native reference builds"
else
	report FAIL "the native reference builds" "see $work/native.log"
fi
if sh "$here/build-package.sh" -s "$set_" -m "$mb" -o "$(dirname "$pkg")" > "$work/package.log" 2>&1; then
	report PASS "the package builds" "$(grep -o 'sha1 [0-9a-f]*' "$work/package.log") $(du -h "$pkg" | cut -f1)"
else
	report FAIL "the package builds" "see $work/package.log"
fi
stale=""
for src in "$here"/mame-driver.cpp "$here"/wbx-entry.cpp "$here"/emulator-info.cpp "$here"/panel.txt "$root"/patches/*.patch; do
	[ "$core" -nt "$src" ] || stale="$stale $(basename "$src")"
done
if [ -f "$core" ] && [ -z "$stale" ]; then
	report PASS "core.wbx is not older than its sources" "$(du -h "$core" | cut -f1)"
else
	report FAIL "core.wbx is not older than its sources" "older than:$stale"
fi

# MAME runs on a stack of its own, and to the sandbox that must be a stack:
# mapped with MAP_STACK, not taken from malloc. On Windows miniBox cannot
# deliver a fault on a page the stack pointer is in unless it was told the
# page is a stack, and a core built the other way dies on its first frame
# there. Linux runs both, so this is where it can be seen.
if nm "$root/build/guest$tail/libco.o" 2>/dev/null | grep -q ' U mmap$' \
	&& ! nm "$root/build/guest$tail/libco.o" 2>/dev/null | grep -q ' U malloc$'; then
	report PASS "MAME's stack is a MAP_STACK mapping" "what Windows needs to run it"
else
	report FAIL "MAME's stack is a MAP_STACK mapping" "libco.o takes it from malloc: the core dies on Windows"
fi

# The declarations hold together, and they are the core's: the panel the
# package declares is the panel the core in the sandbox reports.
folder pong '{}' pong.zip
"$wbxrun" "$core" "$work/pong" --frames 1 --list-panel > "$work/pong.panel" 2> "$work/pong.panel.err" || true
python3 - "$config" "$here/default_keybinds.json" "$here/file_slots.json" "$here/panel.txt" "$work/pong.panel" > "$work/decl.log" 2>&1 <<'PY' && ok=1 || ok=0
import json, re, sys
cfg = json.load(open(sys.argv[1])); kb = json.load(open(sys.argv[2])); slots = json.load(open(sys.argv[3]))
name = cfg["input"]["name"]
buttons = cfg["input"]["buttons"]; axes = [a["name"] for a in cfg["input"]["axes"]]
assert len(buttons) == len(set(buttons)), "a control is declared twice"
assert set(kb["AllTrollers"][name]) == set(buttons), "the key bindings are not the panel"
assert [s["id"] for s in slots["slots"]] == ["romset", "disc", "savedata"] and cfg["romFile"] == "romset"
listed = [l.split() for l in open(sys.argv[4]) if not l.startswith("#")]
assert len([l for l in listed if l[0] == "button"]) + int([l for l in listed if l[0] == "others"][0][1]) == len(buttons), "panel.txt is not the declared panel"
assert len([l for l in listed if l[0] == "axis"]) == len(axes), "panel.txt's axes are not the declared axes"
core, core_axes = [], 0
for line in open(sys.argv[5]):
    if line.startswith("panel "):
        core.append(line[line.index("'") + 1:line.rindex("'")].replace("/", " "))
    elif line.startswith("axis "):
        core_axes += 1
assert len(core) == len(buttons), f"the core's panel has {len(core)} controls, the package declares {len(buttons)}"
wrong = [(i, c, b) for i, (c, b) in enumerate(zip(core, buttons)) if c != b]
assert not wrong, f"control {wrong[0][0]} is {wrong[0][1]} in the core and {wrong[0][2]} in the package"
assert core_axes == len(axes), f"the core has {core_axes} axes, the package declares {len(axes)}"
print(f"{len(buttons)} controls and {len(axes)} axes, the same in the core and the package")
PY
if [ "$ok" = 1 ]; then
	report PASS "the declarations hold together" "$(cat "$work/decl.log")"
else
	report FAIL "the declarations hold together" "$(tail -1 "$work/decl.log")"
fi

# The panel is what the database says: a walk over every machine of the build
# gives the same three files gen-config.py wrote. (They are committed for one
# set of machines; the other set says so and skips.)
"$native" "$work/pong" --scan > "$work/scan.txt" 2> "$work/scan.err" || true
machines="$(sed -n 's/^summary machines \([0-9]*\).*/\1/p' "$work/scan.txt")"
if [ -z "$machines" ]; then
	report FAIL "the walk over every machine" "no summary: see $work/scan.err"
else
	check "the walk over every machine places every control" "$(grep '^summary' "$work/scan.txt" | cut -c9-)" \
		grep -q "^summary .* failed 0 .* unplaced 0 " "$work/scan.txt"
	if grep -q "^# $machines machines of the build" "$here/panel.txt"; then
		mkdir -p "$work/gen"
		python3 "$here/gen-config.py" "$work/scan.txt" "$work/gen" > "$work/gen.log" 2>&1 || true
		if same "$work/gen/panel.txt" "$here/panel.txt" && same "$work/gen/waterbox.config" "$config" \
			&& same "$work/gen/default_keybinds.json" "$here/default_keybinds.json"; then
			report PASS "the committed panel is what the database gives" "$(cat "$work/gen.log")"
		else
			report FAIL "the committed panel is what the database gives" "run gen-config.py on $work/scan.txt"
		fi
	else
		report SKIP "the committed panel is what the database gives" "it was written for another set of machines than these $machines"
	fi
fi

# ------------------------------------------------- 2. Pong: no files needed
# three runs of 900 frames with a coin and the first paddle moved: the
# reference, the sandbox, and the sandbox saving and loading around each frame
coin="$(index "$work/pong.panel" "Coin 1")"
if [ -z "$coin" ]; then
	report FAIL "pong: the panel has a coin" "see $work/pong.panel"
	finish
fi
paddle="$(awk '$1=="axis" && $3=="active" {print $2; exit}' "$work/pong.panel")"
# (the paddle's number is in the range the package declares, -32768 to 32767,
# which the core scales to the machine's own: 20000 is most of the way across)
pong="--frames 900 --report 50 --press $coin:100:5 --axis $paddle:20000:300:200"
"$native" "$work/pong" $pong > "$work/pong.n" 2> "$work/pong.n.err" || true
"$wbxrun" "$core" "$work/pong" $pong --vid-out "$work/pong.raw" > "$work/pong.w" 2> "$work/pong.w.err" || true
"$wbxrun" "$core" "$work/pong" $pong --rerecord > "$work/pong.r" 2> "$work/pong.r.err" || true
"$wbxrun" "$core" "$work/pong" $pong --session > "$work/pong.s" 2> "$work/pong.s.err" || true
"$wbxrun" "$core" "$work/pong" --frames 900 --report 50 > "$work/pong.idle" 2> /dev/null || true
"$wbxrun" "$core" "$work/pong" --frames 900 --report 50 --press "$coin:100:5" > "$work/pong.still" 2> /dev/null || true
check "pong: runs 900 frames with no files" "$(tail -1 "$work/pong.w" | cut -c1-70)" \
	[ "$(stream "$work/pong.w" | wc -l)" = 18 ]
check "pong: the reference and the sandbox agree" "18 digests" same "$work/pong.n" "$work/pong.w"
check "pong: saving and loading around every frame changes nothing" "" same "$work/pong.w" "$work/pong.r"
check "pong: a state reopened in a new sandbox goes on the same" "" same "$work/pong.w" "$work/pong.s"
check "pong: a coin starts a game (a run without one differs)" "" differ "$work/pong.idle" "$work/pong.still"
check "pong: the paddle moves (a run with it still differs)" "" differ "$work/pong.still" "$work/pong.w"
# An axis is declared once for every machine, with the widest range any of
# them has; the core turns the frontend's number into this machine's. Held
# for each of the game's axes that is a position (not a dial's movement): the
# declared range's ends give the machine's own ends, never more; its middle
# gives the machine's rest or, for a gun, the middle of the screen; and half
# way gives half way. Before this the number was added to the machine's
# rest, and a gun could only point at the edges of the screen (chimera issue
# 263): half of the declared range gave the same as all of it.
axes_right() { # <a file of --list-axes lines>
	python3 - "$1" <<'PY'
import sys
seen = 0
for line in open(sys.argv[1]):
    w = line.split()
    if len(w) < 4 or w[0] != "axis" or w[3] != "declared":
        continue
    lo, hi, mn, rest, mx = int(w[4]), int(w[5]), int(w[7]), int(w[8]), int(w[9])
    a, b, c, d, e = (int(x) for x in w[11:16])
    name = w[2]
    assert hi > lo, f"{name}: no declared range"
    assert mn <= a <= b <= c <= d <= e <= mx, f"{name}: {a} {b} {c} {d} {e} is not in order inside {mn}..{mx}"
    assert e == mx, f"{name}: the high end gives {e}, the machine's is {mx}"
    assert lo >= 0 or a == mn, f"{name}: the low end gives {a}, the machine's is {mn}"
    assert abs(2 * d - (c + e)) <= 2, f"{name}: half way up gives {d}, between {c} and {e}"
    assert "LIGHTGUN" in name or c == rest, f"{name}: at rest it gives {c}, the machine rests at {rest}"
    seen += 1
assert seen, "the machine has no axis that is a position"
print(f"{seen} axes")
PY
}
"$wbxrun" "$core" "$work/pong" --frames 1 --list-axes > "$work/pong.axes" 2> /dev/null || true
check "pong: the paddle's declared range is the machine's own, end to end" "$(grep -m1 ' declared ' "$work/pong.axes" | cut -d' ' -f3-)" \
	axes_right "$work/pong.axes"
# the picture is not one colour
if python3 -c "
import sys
d = open(sys.argv[1], 'rb').read(); px = d[d.index(b'\n') + 1:]
sys.exit(0 if len(set(px[i:i + 4] for i in range(0, len(px), 4))) > 1 else 1)" "$work/pong.raw" 2> /dev/null; then
	report PASS "pong: the picture is not blank"
else
	report FAIL "pong: the picture is not blank"
fi

# ----------------------------------- 3. the database, with no game at all
# what the New Project window is told, asked of the core in the sandbox
"$wbxrun" "$core" "$work/pong" --suggest > "$work/pong.suggest" 2> /dev/null || true
check "database: a machine that needs no files says so" "" grep -q "This machine needs no files" "$work/pong.suggest"
check "database: a game's switches are its settings" "$(grep -o '"name":"[a-z]*\.[^"]*"' "$work/pong.suggest" | wc -l) switches (Pong)" \
	grep -q '"name":"dip.Game Won"' "$work/pong.suggest"
# a name MAME does not know: refused, with the closest names
folder noname '{}' pnog.zip
"$wbxrun" "$core" "$work/noname" --suggest > "$work/noname.suggest" 2> /dev/null || true
"$wbxrun" "$core" "$work/noname" --frames 1 > "$work/noname.run" 2>&1 || true
check "database: an unknown name is refused with the closest names" "$(grep -o 'closest names: [^.]*' "$work/noname.suggest" | cut -c1-60)" \
	sh -c "grep -q 'no machine named' '$work/noname.suggest' && grep -q 'pong' '$work/noname.suggest' && grep -q 'Init failed.*no machine named' '$work/noname.run'"
# a game whose set is an empty zip: every file is named, with its set, before
# and at load. Robby Roto's driver is in both builds.
mkdir -p "$work/empty"
python3 -c "
import sys, zipfile
zipfile.ZipFile(sys.argv[1], 'w').close()" "$work/empty/robby.zip"
printf '{"romset":["robby.zip"],"savedata":[]}' > "$work/empty/slots"; printf '{}' > "$work/empty/settings"
"$wbxrun" "$core" "$work/empty" --suggest > "$work/empty.suggest" 2> /dev/null || true
"$wbxrun" "$core" "$work/empty" --frames 1 > "$work/empty.run" 2>&1 || true
check "database: a set with nothing in it names each missing file" "$(grep -o '[0-9]* files are missing or wrong' "$work/empty.suggest")" \
	sh -c "grep -q '(in robby.zip) - missing' '$work/empty.suggest' && grep -q 'Init failed.*(in robby.zip) - missing' '$work/empty.run'"
# the per-game setting reaches the machine: Pong's game length switch
"$wbxrun" "$core" "$work/pong" --frames 1 --list-dips 2> /dev/null | grep "^Game Won" > "$work/pong.dip" || true
folder pong15 '{"dip.Game Won":"15"}' pong.zip
"$wbxrun" "$core" "$work/pong15" --frames 1 --list-dips 2> /dev/null | grep "^Game Won" > "$work/pong15.dip" || true
check "settings: a switch set in the project is the machine's" "$(cut -f1,2 "$work/pong15.dip" | tr '\t' ' ')" \
	sh -c "cut -f2 '$work/pong.dip' | grep -qx 11 && cut -f2 '$work/pong15.dip' | grep -qx 15"
folder pongbad '{"dip.Game Won":"99"}' pong.zip
"$wbxrun" "$core" "$work/pongbad" --frames 1 > "$work/pongbad.run" 2>&1 || true
check "settings: a value the game does not have is refused" "" grep -q 'Init failed.*has no value' "$work/pongbad.run"

# ------------------------------------------------------- 4. real games
# game <name> <label> <boot frames> <sets...>: the three comparisons, lag,
# and a coin. The boot frames are how long the board tests itself before it
# takes a coin (Crypt Killer: a minute); the runs are that plus 900 frames.
game() {
	gm="$1"; gm_label="$2"; gm_boot="$3"; shift 3
	gm_n=$((gm_boot + 900))
	if ! have "$@"; then
		report SKIP "$gm_label" "not in $roms: $*"
		return 1
	fi
	folder "$gm" '{}' "$@"
	"$wbxrun" "$core" "$work/$gm" --frames 1 --list-panel > "$work/$gm.panel" 2> /dev/null || true
	gm_coin="$(index "$work/$gm.panel" "Coin 1")"
	"$native" "$work/$gm" --frames "$gm_n" --report 50 --exercise > "$work/$gm.n" 2> "$work/$gm.n.err" || true
	"$wbxrun" "$core" "$work/$gm" --frames "$gm_n" --report 50 --exercise > "$work/$gm.w" 2> "$work/$gm.w.err" || true
	"$wbxrun" "$core" "$work/$gm" --frames "$gm_n" --report 50 --exercise --rerecord > "$work/$gm.r" 2> "$work/$gm.r.err" || true
	"$wbxrun" "$core" "$work/$gm" --frames "$gm_n" --report 50 --exercise --session > "$work/$gm.s" 2> "$work/$gm.s.err" || true
	# a coin: three, because a board that is still testing itself takes none
	"$wbxrun" "$core" "$work/$gm" --frames "$((gm_n + 600))" --report 100 > "$work/$gm.idle" 2> /dev/null || true
	"$wbxrun" "$core" "$work/$gm" --frames "$((gm_n + 600))" --report 100 --press "$gm_coin:$gm_boot:5" \
		--press "$gm_coin:$((gm_boot + 400)):5" --press "$gm_coin:$((gm_boot + 800)):5" > "$work/$gm.coin" 2> /dev/null || true
	"$wbxrun" "$core" "$work/$gm" --frames "$gm_n" --report 50 > "$work/$gm.still" 2> /dev/null || true
	check "$gm: $gm_label, $gm_n frames" "$(tail -1 "$work/$gm.w" | cut -c1-60)" [ "$(stream "$work/$gm.w" | wc -l)" = "$((gm_n / 50))" ]
	check "$gm: the reference and the sandbox agree" "$((gm_n / 50)) digests" same "$work/$gm.n" "$work/$gm.w"
	check "$gm: saving and loading around every frame changes nothing" "$(grep -o 'stateBytes=[0-9]*' "$work/$gm.r.err" | head -1)" same "$work/$gm.w" "$work/$gm.r"
	check "$gm: a state reopened in a new sandbox goes on the same" "" same "$work/$gm.w" "$work/$gm.s"
	check "$gm: the controls reach the game (an idle run differs)" "" differ "$work/$gm.still" "$work/$gm.w"
	check "$gm: a coin is taken (a run without one differs)" "" differ "$work/$gm.idle" "$work/$gm.coin"
	return 0
}
# lag <name>: frames in which the game read no control - some, not all
lag() {
	lg="$(tail -1 "$work/$1.w" | awk '{print $NF}')"
	lg_n="$(tail -1 "$work/$1.w" | awk '{print $2}')"
	if [ -n "$lg" ] && [ "$lg" -gt 0 ] && [ "$lg" -lt "$lg_n" ]; then
		report PASS "$1: lag frames are counted" "$lg of $lg_n"
	else
		report FAIL "$1: lag frames are counted" "${lg:-none} of ${lg_n:-none}"
	fi
}

if game ssf2t "Super Street Fighter II Turbo (CPS-2)" 1500 ssf2t.zip qsound_hle.zip; then
	lag ssf2t
	# The device set the game needs beside its own is the project's firmware.
	# With only the game's set: the answer to the New Project window lists
	# qsound_hle.zip as firmware and says its Firmware page asks for it, and a
	# load without it is refused, naming the set and the file.
	folder ssf2t-noq '{}' ssf2t.zip
	"$wbxrun" "$core" "$work/ssf2t-noq" --suggest > "$work/ssf2t-noq.suggest" 2> /dev/null || true
	"$wbxrun" "$core" "$work/ssf2t-noq" --frames 1 > "$work/ssf2t-noq.run" 2>&1 || true
	"$wbxrun" "$core" "$work/ssf2t" --suggest > "$work/ssf2t.suggest" 2> /dev/null || true
	check "ssf2t: the device set it needs is asked for as firmware" "$(firmware_of "$work/ssf2t-noq.suggest")" \
		sh -c "[ '$(firmware_of "$work/ssf2t-noq.suggest")' = qsound_hle.zip ] && grep -q 'The Firmware page asks for qsound_hle.zip' '$work/ssf2t-noq.suggest'"
	check "ssf2t: the answer is the same once the set is given" "" \
		[ "$(firmware_of "$work/ssf2t.suggest")" = "$(firmware_of "$work/ssf2t-noq.suggest")" ]
	check "ssf2t: a load without the device set is refused, naming it" "dl-1425.bin, qsound_hle.zip" \
		sh -c "grep -q 'Init failed.*qsound_hle.zip (NOT GIVEN)' '$work/ssf2t-noq.run' && grep -q 'dl-1425.bin (in qsound_hle.zip) - missing' '$work/ssf2t-noq.run'"
	# save data: the EEPROM out, back in, out again
	mkdir -p "$work/ssf2t.save1" "$work/ssf2t.save2"
	"$wbxrun" "$core" "$work/ssf2t" --frames 2400 --report 2400 --exercise --savedata-out "$work/ssf2t.save1" > /dev/null 2>&1 || true
	if [ -s "$work/ssf2t.save1/eeprom.nv" ]; then
		folder ssf2t-saved '{}' ssf2t.zip qsound_hle.zip "$work/ssf2t.save1/eeprom.nv"
		"$wbxrun" "$core" "$work/ssf2t-saved" --frames 1 --report 1 --savedata-out "$work/ssf2t.save2" > /dev/null 2>&1 || true
		check "ssf2t: its save data goes out and back in unchanged" "eeprom.nv, $(wc -c < "$work/ssf2t.save1/eeprom.nv") bytes" \
			same "$work/ssf2t.save1/eeprom.nv" "$work/ssf2t.save2/eeprom.nv"
		printf 'not an eeprom' > "$work/wrong.nv"
		folder ssf2t-wrong '{}' ssf2t.zip qsound_hle.zip "$work/wrong.nv"
		"$wbxrun" "$core" "$work/ssf2t-wrong" --frames 1 > "$work/ssf2t-wrong.run" 2>&1 || true
		check "ssf2t: save data that is no device's is refused" "" grep -q "Init failed.*wrong.nv is not one of this machine's" "$work/ssf2t-wrong.run"
	else
		report FAIL "ssf2t: its save data goes out and back in unchanged" "no eeprom.nv exported"
	fi
	# through Chimera's engine, as a project - the game's set in its slot, the
	# device set as firmware - with a movie: a coin, Start and a punch change
	# the picture an idle movie leaves
	if [ -x "$run" ] && [ -f "$pkg" ]; then
		python3 "$root/tests/make-project.py" "$pkg" "$work/ssf2t.chimeraProject" 2400 "panel=$work/ssf2t.panel" \
			"press=Coin 1:1600:5" "press=1 Player Start:1800:5" "press=P1 Button 1:2000:300" \
			"firmware=$roms/qsound_hle.zip" -- "romset=$roms/ssf2t.zip" > "$work/ssf2t.project.log" 2>&1 || true
		python3 "$root/tests/make-project.py" "$pkg" "$work/ssf2t-idle.chimeraProject" 2400 "panel=$work/ssf2t.panel" \
			"firmware=$roms/qsound_hle.zip" -- "romset=$roms/ssf2t.zip" >> "$work/ssf2t.project.log" 2>&1 || true
		"$run" --project "$work/ssf2t.chimeraProject" "$pkg" --files "$roms" --firmware "qsound_hle.zip=$roms/qsound_hle.zip" \
			--screenshot "2399=$work/ssf2t.tga" --meta "$work/ssf2t.meta" > "$work/ssf2t.engine" 2>&1 || true
		"$run" --project "$work/ssf2t-idle.chimeraProject" "$pkg" --files "$roms" --firmware "qsound_hle.zip=$roms/qsound_hle.zip" \
			--screenshot "2399=$work/ssf2t-idle.tga" > "$work/ssf2t-idle.engine" 2>&1 || true
		check "ssf2t: a project with its firmware plays a movie in Chimera's engine" "2400 frames" \
			sh -c "grep -q '^frames=2400' '$work/ssf2t.engine' && grep -q '^frames=2400' '$work/ssf2t-idle.engine'"
		check "ssf2t: the movie's coin, Start and punch change the picture" "" differ "$work/ssf2t.tga" "$work/ssf2t-idle.tga"
	else
		report SKIP "ssf2t: a project with its firmware plays a movie in Chimera's engine" "no chimera-run or package"
	fi
fi

if game fatfury3 "Fatal Fury 3 (Neo Geo)" 600 fatfury3.zip neogeo.zip; then
	lag fatfury3
	# the BIOS setting is the machine's: another BIOS, another run
	folder fatfury3-jp '{"bios":"Japan MVS (Ver. 3)"}' fatfury3.zip neogeo.zip
	"$wbxrun" "$core" "$work/fatfury3-jp" --frames 1500 --report 50 --exercise > "$work/fatfury3-jp.w" 2> "$work/fatfury3-jp.err" || true
	# The BIOS set is firmware only as far as the game's own set lacks it. A
	# set with the BIOS files taken out needs neogeo.zip whatever the BIOS; a
	# set that carries some of them needs it with the other BIOS choices only,
	# and says with which.
	"$wbxrun" "$core" "$work/fatfury3" --suggest > "$work/fatfury3.suggest" 2> /dev/null || true
	mkdir -p "$work/fatfury3-split"
	python3 - "$roms/fatfury3.zip" "$roms/neogeo.zip" "$work/fatfury3-split/fatfury3.zip" <<'PY'
import sys, zipfile
bios = set(zipfile.ZipFile(sys.argv[2]).namelist())
game = zipfile.ZipFile(sys.argv[1])
out = zipfile.ZipFile(sys.argv[3], "w", zipfile.ZIP_DEFLATED)
for name in game.namelist():
    if name not in bios:
        out.writestr(name, game.read(name))
out.close()
PY
	printf '{"romset":["fatfury3.zip"],"savedata":[]}' > "$work/fatfury3-split/slots"; printf '{}' > "$work/fatfury3-split/settings"
	"$wbxrun" "$core" "$work/fatfury3-split" --suggest > "$work/fatfury3-split.suggest" 2> /dev/null || true
	"$wbxrun" "$core" "$work/fatfury3-split" --frames 1 > "$work/fatfury3-split.run" 2>&1 || true
	cp "$roms/neogeo.zip" "$work/fatfury3-split/"
	"$wbxrun" "$core" "$work/fatfury3-split" --frames 600 --report 600 > "$work/fatfury3-split.w" 2> /dev/null || true
	check "fatfury3: a set without its BIOS files asks for neogeo.zip as firmware" "$(firmware_of "$work/fatfury3-split.suggest")" \
		sh -c "[ '$(firmware_of "$work/fatfury3-split.suggest")' = neogeo.zip ] && grep -q 'Init failed.*neogeo.zip (NOT GIVEN)' '$work/fatfury3-split.run'"
	check "fatfury3: the same set runs once neogeo.zip is there" "$(tail -1 "$work/fatfury3-split.w" | cut -c1-40)" \
		[ "$(stream "$work/fatfury3-split.w" | wc -l)" = 1 ]
	if [ -n "$(firmware_of "$work/fatfury3.suggest")" ]; then
		check "fatfury3: a set that carries some BIOSes asks only with the others" "$(firmware_of "$work/fatfury3.suggest") (? = with some BIOS choices)" \
			[ "$(firmware_of "$work/fatfury3.suggest")" = "neogeo.zip?" ]
	fi
	# What the game calls its controls reaches Chimera, beside their names:
	# MAME's Neo Geo driver calls the four buttons A, B, C and D.
	if [ -x "$run" ] && [ -f "$pkg" ]; then
		: > "$work/empty.movie"
		"$run" "$pkg" "$roms/fatfury3.zip" "$work/empty.movie" --frames 1 --record "$work/fatfury3.rec" --meta "$work/fatfury3.meta" > "$work/fatfury3.engine" 2>&1 || true
		check "fatfury3: the game's own names for its controls reach Chimera" "$(grep '^label=P[12] Button' "$work/fatfury3.meta" 2> /dev/null | cut -c7- | tr '\n' ' ')" \
			sh -c "grep -qx 'label=P1 Button 1=A' '$work/fatfury3.meta' && grep -qx 'label=P2 Button 4=D' '$work/fatfury3.meta' && ! grep -q '^label=P1 Up=' '$work/fatfury3.meta'"
	else
		report SKIP "fatfury3: the game's own names for its controls reach Chimera" "no chimera-run or package"
	fi
	check "fatfury3: the BIOS setting changes the machine" "Japan MVS (Ver. 3)" \
		sh -c "[ \"\$(awk '/^frame/' '$work/fatfury3-jp.w' | wc -l)\" = 30 ] && ! cmp -s '$work/fatfury3-jp.w' '$work/fatfury3.w'"
fi

if game shinobi "Shinobi (System 16A)" 300 shinobi.zip; then
	lag shinobi
	# a dip switch is the machine's
	folder shinobi-lives '{"dip.Lives":"5"}' shinobi.zip
	"$wbxrun" "$core" "$work/shinobi-lives" --frames 1 --list-dips 2> /dev/null | grep "^Lives" > "$work/shinobi.dip" || true
	check "shinobi: a dip switch set in the project is the machine's" "$(cut -f1,2 "$work/shinobi.dip" | tr '\t' ' ')" \
		sh -c "cut -f2 '$work/shinobi.dip' | grep -qx 5"
fi

if game cryptklr "Crypt Killer (Konami GQ, a hard disk image)" 3600 cryptklr.zip 420uaa04.chd; then
	lag cryptklr
	# what the machine writes to its disk stays in memory: no file appears
	"$wbxrun" "$core" "$work/cryptklr" --frames 1 --list-axes > "$work/cryptklr.axes" 2> /dev/null || true
	check "cryptklr: the guns' declared range is the screen, edge to edge" "$(grep -m1 'LIGHTGUN_X declared' "$work/cryptklr.axes" | cut -d' ' -f3-)" \
		axes_right "$work/cryptklr.axes"
	check "cryptklr: nothing is written beside the disk image" "" \
		sh -c "[ ! -e '$work/cryptklr/diff' ] && [ \"\$(ls '$work/cryptklr' | wc -l)\" = 4 ]"
	folder cryptklr-nodisk '{}' cryptklr.zip
	"$wbxrun" "$core" "$work/cryptklr-nodisk" --suggest > "$work/cryptklr-nodisk.suggest" 2> /dev/null || true
	check "cryptklr: a missing disc image is named" "420uaa04.chd" grep -q '420uaa04.chd (a disc image) - missing' "$work/cryptklr-nodisk.suggest"
fi

game rampart "Rampart (Atari)" 600 rampart.zip && lag rampart

# ------------------------------------------------ 5. Chimera's own checks
if [ -x "$run" ] && [ -f "$pkg" ]; then
	# Pong through the engine: a rom "file" that is an empty zip named pong.zip
	python3 -c "
import sys, zipfile
zipfile.ZipFile(sys.argv[1], 'w').close()" "$work/pong.zip"
	: > "$work/empty.movie"
	"$run" "$pkg" "$work/pong.zip" "$work/empty.movie" --frames 300 --record "$work/pong.rec" > "$work/pong.engine" 2>&1 || true
	check "pong: runs in Chimera's engine" "300 frames" grep -q '^frames=300' "$work/pong.engine"
	"$run" "$pkg" "$work/pong.zip" --suggest > "$work/pong.engine.suggest" 2>&1 || true
	check "pong: the engine gets the database's answer" "" grep -q "This machine needs no files" "$work/pong.engine.suggest"
else
	report SKIP "pong: runs in Chimera's engine" "no chimera-run at $run, or no package"
fi
tests="$chimera_root/source/gui/Chimera.Tests.Client.Common/Chimera.Tests.Client.Common.csproj"
if [ -f "$tests" ] && command -v dotnet > /dev/null 2>&1 && [ -f "$pkg" ]; then
	if CHIMERA_CORES_DIR="$(dirname "$pkg")" env -u DISPLAY dotnet test "$tests" -c Release --no-build \
		--filter "FullyQualifiedName~InstalledCorePackagesTests|FullyQualifiedName~MnemonicUniquenessTests" > "$work/contract.log" 2>&1; then
		report PASS "Chimera's package checks" "$(grep -o 'Passed: *[0-9]*' "$work/contract.log" | tail -1)"
	else
		report FAIL "Chimera's package checks" "see $work/contract.log"
	fi
else
	report SKIP "Chimera's package checks" "no dotnet, or no chimera tests at $tests"
fi

finish

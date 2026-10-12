#!/bin/bash
# The window test: the package loaded by Chimera itself (Chimera.exe under
# Mono, on a private Xvfb display, with nobody at it), not by its headless
# runner. Three things are asked:
#  - Pong, which needs no files, runs in the window: Chimera takes the
#    package as the arcade core, the machine runs the frames asked for, and
#    its picture has Pong's size;
#  - the keys the package ships become Chimera's keys for the controls the
#    game has;
#  - with a real game (Shinobi, when its set is in tests/roms-local): the
#    game's memory after a number of frames in the window is byte for byte
#    what Chimera's headless runner leaves at the same frame.
#
# The window takes a .zip as an archive to choose a file from, so each game
# is opened as a project, written by make-project.py with its files beside it.
#
# Usage: tests/run-frontend.sh --chimera-root <chimera checkout> [--frames N] [-s dev]
# Needs: mono, Xvfb, Chimera built with its window (dotnet build), and the
# package installed in that checkout (waterbox/build-package.sh -r <chimera>;
# -s dev for the small build's).
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
wb="$root/waterbox"
frames=300
chimera_root=""
tail=""
while [ $# -gt 0 ]; do
	case "$1" in
		--chimera-root) chimera_root="$2"; shift ;;
		--frames) frames="$2"; shift ;;
		-s) [ "$2" = dev ] && tail="-dev"; shift ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*) break ;;
	esac
	shift
done
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || { echo "say where Chimera is: --chimera-root <chimera checkout>" >&2; exit 2; }
chimera_root="$(cd "$chimera_root" && pwd)"
roms="${MAME_ROMS:-$here/roms-local}"

emu_exe="$chimera_root/build/Chimera.exe"
package="$chimera_root/build/Cores/mame.chimeraCore"
if [ -n "$tail" ]; then bin="$root/build/guest-dev"; else bin="$wb/bin"; fi
runner="$chimera_root/build/meson-linux/chimera-run"
[ -f "$emu_exe" ] || { echo "Chimera's window is not built: $emu_exe" >&2; exit 1; }
[ -f "$package" ] || { echo "the package is not installed: $package (run waterbox/build-package.sh -r $chimera_root)" >&2; exit 1; }
[ -x "$bin/run-wbx" ] || { echo "the sandbox build is not there: $bin/run-wbx" >&2; exit 1; }

work="$(mktemp -d)"
# Pong's "rom set": the machine needs no files, but a project is opened by
# one, and its name says which machine
python3 -c "
import sys, zipfile
zipfile.ZipFile(sys.argv[1], 'w').close()" "$work/pong.zip"
xvfb_pid=""
cleanup() { [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null; rm -rf "$work"; }
trap cleanup EXIT

export LD_LIBRARY_PATH="$chimera_root/build/dll:$chimera_root/build:/usr/lib/x86_64-linux-gnu"
export MONO_CRASH_NOFILE=1 MONO_WINFORMS_XIM_STYLE=disabled ALSOFT_DRIVERS=null
# always a display of this test's own: nobody's desktop is drawn on
command -v Xvfb >/dev/null || { echo "Xvfb not found (apt install xvfb)" >&2; exit 1; }
for n in 90 91 92 93 94 95 96; do
	if [ ! -e "/tmp/.X11-unix/X$n" ]; then
		Xvfb ":$n" -screen 0 640x480x24 -nolisten tcp > /dev/null 2>&1 & xvfb_pid=$!
		export DISPLAY=":$n"; break
	fi
done
[ -n "$xvfb_pid" ] || { echo "no free display number for Xvfb" >&2; exit 1; }
sleep 1

config="$work/config.ini"
( cd "$chimera_root" && timeout 120 mono "$emu_exe" --headless "--config=$config" "--lua=$here/exit.lua" ) > "$work/bootstrap.log" 2>&1
[ -f "$config" ] || { echo "Chimera did not write a configuration:"; tail -5 "$work/bootstrap.log"; exit 1; } >&2
sed -i 's/"DispMethod": [0-9]/"DispMethod": 1/' "$config"

ok=0
failed=0
skipped=0
report() {
	printf '%-16s %-5s %s\n' "$1" "$2" "$3"
	case "$2" in PASS) ok=$((ok + 1)) ;; SKIP) skipped=$((skipped + 1)) ;; *) failed=$((failed + 1)) ;; esac
}

# run_frontend <tag> <configuration> <frames> <domain or ""> <rom set>...
# The game's files are copied beside its project, where Chimera looks.
run_frontend() {
	local tag="$1" cfg="$2" nframes="$3" domain="$4"; shift 4
	local dir="$work/$tag" job="$work/job.$tag.txt" slots="" list=""
	mkdir -p "$dir"
	for f in "$@"; do
		[ -f "$dir/$(basename "$f")" ] || cp "$f" "$dir/"
		slots="$slots romset=$dir/$(basename "$f")"
		list="$list${list:+,}\"$(basename "$f")\""
	done
	# which of the panel's controls this game has: a movie carries only those
	printf '{"romset":[%s],"savedata":[]}' "$list" > "$dir/slots"; printf '{}' > "$dir/settings"
	"$bin/run-wbx" "$bin/core.wbx" "$dir" --frames 1 --list-panel > "$work/$tag.panel" 2> /dev/null
	rm -f "$dir/slots" "$dir/settings"
	# shellcheck disable=SC2086
	python3 "$here/make-project.py" "$package" "$dir/$tag.chimeraProject" "$nframes" "panel=$work/$tag.panel" -- $slots > "$work/$tag.log" 2>&1 || return 1
	{
		echo "frames=$nframes"
		echo "out=$work/$tag.ram.bin"
		echo "meta=$work/$tag.meta.txt"
		echo "shot="
		echo "domain=$domain"
	} > "$job"
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 mono "$emu_exe" --headless \
		"--config=$cfg" "--project=$dir/$tag.chimeraProject" "--lua=$here/frontend-ram.lua" ) >> "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}
value() { sed -n "s/^$2=//p" "$work/$1.meta.txt" 2>/dev/null; }

python3 "$here/settings-config.py" "$config" "$work/config.base.ini" '{}'
if ! run_frontend pong "$work/config.base.ini" "$frames" "" "$work/pong.zip"; then
	report "window:pong" FAIL "Chimera did not finish the run: $(value pong detail) $(tail -2 "$work/pong.log" | tr '\n' ' ' | cut -c1-200)"
elif [ "$(value pong frames)" = "$frames" ] && [ "$(value pong width)" = 1552 ] && [ "$(value pong height)" = 240 ]; then
	report "window:pong" PASS "Pong ran $frames frames in Chimera's window as the arcade core, with no files; its picture is 1552x240"
else
	report "window:pong" FAIL "frames $(value pong frames) of $frames, picture $(value pong width)x$(value pong height) and not 1552x240"
fi

python3 "$here/forget-controller.py" "$work/config.base.ini" "$work/config.keys.ini" "Arcade Panel"
if run_frontend keys "$work/config.keys.ini" 1 "" "$work/pong.zip"; then
	if python3 "$here/check-keybinds.py" "$work/config.keys.ini" "$wb/default_keybinds.json" "Arcade Panel" "Coin 1" > "$work/keys.txt" 2>&1; then
		report "window:keys" PASS "$(cat "$work/keys.txt")"
	else
		report "window:keys" FAIL "$(head -1 "$work/keys.txt")"
	fi
else
	report "window:keys" FAIL "Chimera did not finish the run: $(tail -2 "$work/keys.log" | tr '\n' ' ' | cut -c1-200)"
fi

if [ ! -f "$roms/shinobi.zip" ] || [ ! -x "$runner" ]; then
	report "window:memory" SKIP "needs shinobi.zip in $roms and chimera-run"
else
	cp "$roms/shinobi.zip" "$work/shinobi.zip"
	: > "$work/empty.movie"
	"$runner" "$package" "$work/shinobi.zip" "$work/empty.movie" --frames "$frames" --record "$work/rec.txt" \
		--dump "nvram=$work/engine.ram.bin" > "$work/engine.log" 2>&1
	if ! run_frontend shinobi "$work/config.base.ini" "$frames" nvram "$work/shinobi.zip"; then
		report "window:memory" FAIL "Chimera did not finish the run: $(value shinobi detail) $(tail -2 "$work/shinobi.log" | tr '\n' ' ' | cut -c1-200)"
	elif [ -s "$work/engine.ram.bin" ] && cmp -s "$work/engine.ram.bin" "$work/shinobi.ram.bin"; then
		report "window:memory" PASS "Shinobi after $frames frames in the window: its work RAM ($(wc -c < "$work/shinobi.ram.bin") bytes) is what the headless runner leaves"
	else
		report "window:memory" FAIL "the memory differs from the headless runner's (or one of the two is missing)"
	fi
fi

echo
echo "$ok passed, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]

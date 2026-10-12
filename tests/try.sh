#!/bin/sh
# Works on one game by hand: makes a work folder holding the given sets (from
# tests/roms-local) the way the sandbox mounts them, and runs the native
# reference on it. The first set names the game.
#
# Usage: tests/try.sh [-g] [-k <settings json>] [-d <folder>] "<set> [<set>...]" [run options]
#   -g   the sandbox build and not the reference
#   -a   the full build (every arcade machine) and not the small one
#   -d   a folder of save data files (*.nv) the machine starts with
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
flavor="native"; settings="{}"; saves=""; tail="-dev"
while getopts "gak:d:" opt; do
	case "$opt" in
		g) flavor="guest" ;;
		a) tail="" ;;
		k) settings="$OPTARG" ;;
		d) saves="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
sets="$1"; shift
work="$(mktemp -d "${TMPDIR:-/tmp}/mame-try.XXXXXX")"
list=""
for s in $sets; do
	for f in "$here/roms-local/$s".zip "$here/roms-local/$s".7z "$here/roms-local/$s".chd; do
		[ -f "$f" ] && { cp "$f" "$work/"; list="$list${list:+,}\"$(basename "$f")\""; }
	done
done
[ -n "$list" ] || list="\"${sets%% *}.zip\""
savelist=""
if [ -n "$saves" ]; then
	for f in "$saves"/*.nv; do
		[ -f "$f" ] && { cp "$f" "$work/"; savelist="$savelist${savelist:+,}\"$(basename "$f")\""; }
	done
fi
printf '{"romset":[%s],"savedata":[%s]}' "$list" "$savelist" > "$work/slots"
printf '%s' "$settings" > "$work/settings"
echo "work folder: $work" >&2
if [ "$flavor" = native ]; then
	"$root/build/native$tail/run-native" "$work" "$@"
elif [ -n "$tail" ]; then
	"$root/build/guest-dev/run-wbx" "$root/build/guest-dev/core.wbx" "$work" "$@"
else
	"$root/waterbox/bin/run-wbx" "$root/waterbox/bin/core.wbx" "$work" "$@"
fi

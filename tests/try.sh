#!/bin/sh
# Works on one game by hand: makes a work folder holding the given sets (from
# tests/roms-local) the way the sandbox mounts them, and runs the native
# reference on it. The first set names the game.
#
# Usage: tests/try.sh [-g] [-k <settings json>] "<set> [<set>...]" [run options]
#   -g   the sandbox build (build/guest-dev) and not the reference
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
flavor="native"; settings="{}"
while getopts "gk:" opt; do
	case "$opt" in
		g) flavor="guest" ;;
		k) settings="$OPTARG" ;;
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
printf '{"romset":[%s]}' "$list" > "$work/slots"
printf '%s' "$settings" > "$work/settings"
echo "work folder: $work" >&2
if [ "$flavor" = native ]; then
	"$root/build/native-dev/run-native" "$work" "$@"
else
	"$root/build/guest-dev/run-wbx" "$root/build/guest-dev/core.wbx" "$work" "$@"
fi

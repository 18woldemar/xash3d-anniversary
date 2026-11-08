#!/bin/sh
# Starts xash3d-anniversary on the Half-Life that Steam installed: it looks through Steam's libraries, the
# SD card ones too. XASH3D_RODIR points at the game by hand. Arguments go to the engine (-gamepadui, -ref gl ...).
here=$(dirname "$(readlink -f "$0")")

fail()
{
	command -v zenity > /dev/null && zenity --error --no-wrap --text="$1" 2> /dev/null
	echo "xash3d-anniversary: $1" >&2
	exit 1
}

find_game()
{
	for root in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
		[ -d "$root/steamapps" ] || continue
		{
			echo "$root"
			sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$root/steamapps/libraryfolders.vdf" 2> /dev/null
		} | while read -r lib; do
			[ -d "$lib/steamapps/common/Half-Life/valve" ] && echo "$lib/steamapps/common/Half-Life"
		done
	done
}

data=${XASH3D_RODIR:-$(find_game | head -n 1)}
[ -d "$data/valve" ] || fail "Half-Life was not found. Install it in Steam, or set XASH3D_RODIR to its folder."

cd "$here" || exit 1
XASH3D_RODIR=$data LD_LIBRARY_PATH="$here${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec ./xash3d -game valve "$@"

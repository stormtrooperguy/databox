#!/usr/bin/env bash
#
# load-audio.sh — load the databox audio set onto a DFR1173 MP3 module (macOS).
#
# The DFR1173 plays tracks by index, so the source files are copied IN ORDER
# (sorted by name) and renamed 001.mp3, 002.mp3, 003.mp3, ...  `cp -X` skips
# extended attributes so no AppleDouble "._" sidecars are created — those would
# otherwise be counted as extra tracks and throw off the numbering. Existing
# audio and macOS cruft (._*, .DS_Store, .fseventsd, .Spotlight-V100) are wiped
# first.
#
# Usage:
#   tools/load-audio.sh [-y] [DEVICE_PATH] [SOURCE_DIR]
#     -y            skip the confirmation prompt
#     DEVICE_PATH   default: /Volumes/NO NAME   (the DFR1173 mounts unlabeled)
#     SOURCE_DIR    default: ~/Music/databox_audio
#
set -euo pipefail

AUTO=0
if [ "${1:-}" = "-y" ]; then AUTO=1; shift; fi

DEV="${1:-/Volumes/NO NAME}"
SRC="${2:-$HOME/Music/databox_audio}"

[ -d "$DEV" ] || { echo "Device not mounted: $DEV" >&2; exit 1; }
[ -w "$DEV" ] || { echo "Device not writable: $DEV" >&2; exit 1; }
[ -d "$SRC" ] || { echo "Source dir not found: $SRC" >&2; exit 1; }

# Source .mp3s in sorted order (glob expansion is already name-sorted).
shopt -s nullglob
FILES=("$SRC"/*.mp3)
[ "${#FILES[@]}" -gt 0 ] || { echo "No .mp3 files in $SRC" >&2; exit 1; }

echo "Target device : $DEV"
echo "Source dir    : $SRC"
echo "Will load ${#FILES[@]} file(s), in this order:"
i=1
for f in "${FILES[@]}"; do
    printf "  %03d.mp3  <-  %s\n" "$i" "$(basename "$f")"
    i=$((i + 1))
done

if [ "$AUTO" != "1" ]; then
    read -r -p "Replace ALL audio on '$DEV'? [y/N] " ans
    case "$ans" in
        y | Y) ;;
        *) echo "Aborted."; exit 1 ;;
    esac
fi

echo "Removing old audio + macOS cruft..."
rm -f "$DEV"/*.mp3 "$DEV"/*.MP3 2>/dev/null || true
rm -rf "$DEV/.fseventsd" "$DEV/.Spotlight-V100" "$DEV/.Trashes" 2>/dev/null || true
find "$DEV" -name '._*' -delete 2>/dev/null || true
find "$DEV" -name '.DS_Store' -delete 2>/dev/null || true

echo "Copying in order (cp -X = no ._ sidecars)..."
i=1
for f in "${FILES[@]}"; do
    dest=$(printf "%s/%03d.mp3" "$DEV" "$i")
    cp -X "$f" "$dest"
    printf "  %03d.mp3\n" "$i"
    i=$((i + 1))
done

echo "Cleanup pass + flush..."
dot_clean "$DEV" 2>/dev/null || true
find "$DEV" -name '._*' -delete 2>/dev/null || true
rm -rf "$DEV/.fseventsd" "$DEV/.Spotlight-V100" 2>/dev/null || true
sync

echo
echo "Done. Device now contains:"
ls -la "$DEV"/*.mp3
cruft=$(find "$DEV" \( -name '._*' -o -name '.DS_Store' \) 2>/dev/null || true)
if [ -z "$cruft" ]; then
    echo "No ._ / .DS_Store cruft."
else
    echo "WARNING — leftover cruft:"; echo "$cruft"
fi
echo "Eject cleanly before unplugging:  diskutil eject \"$DEV\""

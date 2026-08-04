#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE="$SCRIPT_DIR/../app/res/perigee.svg"
DESTINATION="$SCRIPT_DIR/../app/perigee.icns"
TEMP_DIRECTORY=$(mktemp -d)

cleanup()
{
    rm -rf "$TEMP_DIRECTORY"
}
trap cleanup EXIT HUP INT TERM

for SIZE in 32 64 128 256 512 1024; do
    convert -background none "$SOURCE" -resize "${SIZE}x${SIZE}" -strip \
        "$TEMP_DIRECTORY/$SIZE.png"
done

python3 "$SCRIPT_DIR/pngs-to-icns.py" "$TEMP_DIRECTORY" "$DESTINATION"

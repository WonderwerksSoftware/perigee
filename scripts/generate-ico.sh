#!/bin/sh
set -eu

# The ImageMagick conversion tool does not always generate ICO files with
# background transparency. Validate that the output is transparent.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE="$SCRIPT_DIR/../app/res/perigee.svg"

convert -density 256 -background none -define icon:auto-resize \
    "$SOURCE" -strip "$SCRIPT_DIR/../app/perigee.ico"
convert -density 256 -background none "$SOURCE" -resize 64x64 -strip \
    "$SCRIPT_DIR/../app/perigee_wix.png"

echo IMPORTANT: Validate that the icon has a transparent background before committing.

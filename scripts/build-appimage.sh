#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)"
# shellcheck source=scripts/lib/package_linux_common.sh
. "$SCRIPT_DIR/lib/package_linux_common.sh"

perigee_initialize
for tool in mksquashfs python3; do
    command -v "$tool" >/dev/null 2>&1 || perigee_fail "required tool is missing: $tool"
done

RUNTIME="${PERIGEE_APPIMAGE_RUNTIME:-}"
[ -n "$RUNTIME" ] || perigee_fail "set PERIGEE_APPIMAGE_RUNTIME to a pinned x86_64 type-2 AppImage runtime"
[ -f "$RUNTIME" ] || perigee_fail "AppImage runtime does not exist: $RUNTIME"
[ -r "$RUNTIME" ] || perigee_fail "AppImage runtime is not readable: $RUNTIME"
python3 -B "$SCRIPT_DIR/lib/verify_linux_artifacts.py" validate-runtime "$RUNTIME"

BINARY="$(perigee_build_release)"
perigee_stage_payload appimage "$BINARY"
mkdir -p -- "$PERIGEE_BUILD_DIR"

ARTIFACT="$PERIGEE_OUTPUT_DIR/Perigee-$PERIGEE_VERSION-x86_64.AppImage"
SQUASHFS="$PERIGEE_BUILD_DIR/Perigee-$PERIGEE_VERSION-x86_64.squashfs"
TEMP_ARTIFACT="$ARTIFACT.tmp"
rm -f -- "$SQUASHFS" "$TEMP_ARTIFACT"

env -u SOURCE_DATE_EPOCH mksquashfs "$PERIGEE_STAGE_DIR" "$SQUASHFS" \
    -noappend \
    -all-root \
    -mkfs-time "$SOURCE_DATE_EPOCH" \
    -all-time "$SOURCE_DATE_EPOCH" \
    -no-exports \
    -no-xattrs \
    -no-progress \
    -processors 1 \
    -comp zstd \
    -Xcompression-level 19 >/dev/null

cp -- "$RUNTIME" "$TEMP_ARTIFACT"
dd if="$SQUASHFS" of="$TEMP_ARTIFACT" oflag=append conv=notrunc status=none
mv -f -- "$TEMP_ARTIFACT" "$ARTIFACT"
chmod 0755 "$ARTIFACT"
printf 'Runtime SHA-256: '
sha256sum "$RUNTIME" | awk '{print $1}'
printf 'Built %s\n' "$ARTIFACT"

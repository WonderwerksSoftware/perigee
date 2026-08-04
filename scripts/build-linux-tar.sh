#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)"
# shellcheck source=scripts/lib/package_linux_common.sh
. "$SCRIPT_DIR/lib/package_linux_common.sh"

perigee_initialize
command -v tar >/dev/null 2>&1 || perigee_fail "required tool is missing: tar"
command -v zstd >/dev/null 2>&1 || perigee_fail "required tool is missing: zstd"

BINARY="$(perigee_build_release)"
perigee_stage_payload tar "$BINARY"

ARTIFACT="$PERIGEE_OUTPUT_DIR/Perigee-$PERIGEE_VERSION-linux-x86_64.tar.zst"
TEMP_ARTIFACT="$ARTIFACT.tmp"
rm -f -- "$TEMP_ARTIFACT"

tar \
    --sort=name \
    --format=posix \
    --pax-option=delete=atime,delete=ctime \
    --mtime="@$SOURCE_DATE_EPOCH" \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    -C "$PERIGEE_STAGE_DIR" \
    -cf - \
    LICENSE NOTICE.md bin lib plugins qml share translations \
    | zstd -q -T1 -19 -f -o "$TEMP_ARTIFACT"

mv -f -- "$TEMP_ARTIFACT" "$ARTIFACT"
chmod 0644 "$ARTIFACT"
printf 'Built %s\n' "$ARTIFACT"

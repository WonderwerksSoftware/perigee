#!/usr/bin/env bash
set -euo pipefail

fail()
{
    printf 'artifact verification failed: %s\n' "$1" >&2
    exit 1
}

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)"
SOURCE_ROOT="$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd -P)"
VERSION="$(tr -d '\r\n' < "$SOURCE_ROOT/app/version.txt")"
ARCH="${PERIGEE_ARTIFACT_ARCH:-$(uname -m)}"
ARTIFACT_DIR="${PERIGEE_ARTIFACT_DIR:-$SOURCE_ROOT}"
PYTHON_HELPER="$SCRIPT_DIR/lib/verify_linux_artifacts.py"

case "$VERSION" in
    ''|*[!0-9A-Za-z.+~-]*) fail "invalid app/version.txt" ;;
esac
[ "$ARCH" = x86_64 ] || fail "unsupported release architecture: $ARCH"

APPIMAGE="$ARTIFACT_DIR/Perigee-$VERSION-x86_64.AppImage"
TAR_ARTIFACT="$ARTIFACT_DIR/Perigee-$VERSION-linux-x86_64.tar.zst"

[ -f "$APPIMAGE" ] || fail "missing artifact: $(basename -- "$APPIMAGE")"
[ -x "$APPIMAGE" ] || fail "AppImage is not executable: $(basename -- "$APPIMAGE")"
[ -f "$TAR_ARTIFACT" ] || fail "missing artifact: $(basename -- "$TAR_ARTIFACT")"

for tool in python3 unsquashfs zstd readelf desktop-file-validate appstreamcli xmllint timeout; do
    command -v "$tool" >/dev/null 2>&1 || fail "required verifier tool is missing: $tool"
done

VERIFY_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/perigee-artifact-verify.XXXXXX")"
cleanup()
{
    rm -rf -- "$VERIFY_ROOT"
}
trap cleanup EXIT HUP INT TERM

APPIMAGE_ROOT="$VERIFY_ROOT/appimage"
TAR_ROOT="$VERIFY_ROOT/tar"

python3 -B "$PYTHON_HELPER" scan-file "$APPIMAGE"
python3 -B "$PYTHON_HELPER" scan-file "$TAR_ARTIFACT"
python3 -B "$PYTHON_HELPER" extract-appimage "$APPIMAGE" "$APPIMAGE_ROOT"
python3 -B "$PYTHON_HELPER" extract-tar "$TAR_ARTIFACT" "$TAR_ROOT"
python3 -B "$PYTHON_HELPER" verify-tree appimage "$APPIMAGE_ROOT"
python3 -B "$PYTHON_HELPER" verify-tree tar "$TAR_ROOT"

desktop-file-validate "$APPIMAGE_ROOT/usr/share/applications/app.perigee_stream.Perigee.desktop"
desktop-file-validate "$TAR_ROOT/share/applications/app.perigee_stream.Perigee.desktop"
appstreamcli validate --no-net "$APPIMAGE_ROOT/usr/share/metainfo/app.perigee_stream.Perigee.appdata.xml" >/dev/null
appstreamcli validate --no-net "$TAR_ROOT/share/metainfo/app.perigee_stream.Perigee.appdata.xml" >/dev/null
xmllint --noout \
    "$APPIMAGE_ROOT/usr/share/metainfo/app.perigee_stream.Perigee.appdata.xml" \
    "$APPIMAGE_ROOT/usr/share/icons/hicolor/scalable/apps/app.perigee_stream.Perigee.svg" \
    "$TAR_ROOT/share/metainfo/app.perigee_stream.Perigee.appdata.xml" \
    "$TAR_ROOT/share/icons/hicolor/scalable/apps/app.perigee_stream.Perigee.svg"

python3 -B "$PYTHON_HELPER" verify-version "$APPIMAGE" "Perigee $VERSION" "$VERIFY_ROOT/version-appimage"
python3 -B "$PYTHON_HELPER" verify-version "$TAR_ROOT/bin/perigee" "Perigee $VERSION" "$VERIFY_ROOT/version-tar"

if [ "${PERIGEE_SKIP_WAYLAND_LAUNCH:-0}" != 1 ]; then
    python3 -B "$PYTHON_HELPER" launch-gate \
        "$APPIMAGE" "$VERIFY_ROOT/launch-appimage" "$APPIMAGE_ROOT/usr/bin/perigee"
    python3 -B "$PYTHON_HELPER" launch-gate \
        "$TAR_ROOT/bin/perigee" "$VERIFY_ROOT/launch-tar" "$TAR_ROOT/bin/perigee"
fi

printf 'Verified %s\n' "$(basename -- "$APPIMAGE")"
printf 'Verified %s\n' "$(basename -- "$TAR_ARTIFACT")"

#!/usr/bin/env bash

perigee_fail()
{
    printf 'Perigee packaging failed: %s\n' "$1" >&2
    exit 1
}

perigee_initialize()
{
    PERIGEE_SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[1]}")" && pwd -P)"
    PERIGEE_SOURCE_ROOT="$(CDPATH='' cd -- "$PERIGEE_SCRIPT_DIR/.." && pwd -P)"
    PERIGEE_VERSION="$(tr -d '\r\n' < "$PERIGEE_SOURCE_ROOT/app/version.txt")"
    PERIGEE_ARCH="${PERIGEE_ARCH:-$(uname -m)}"
    PERIGEE_OUTPUT_DIR="${PERIGEE_OUTPUT_DIR:-$PERIGEE_SOURCE_ROOT}"
    PERIGEE_BUILD_DIR="${PERIGEE_BUILD_DIR:-$PERIGEE_SOURCE_ROOT/build/package-linux-release}"
    PERIGEE_STAGE_DIR="${PERIGEE_STAGE_DIR:-$PERIGEE_SOURCE_ROOT/build/package-linux-stage}"
    PERIGEE_JOBS="${PERIGEE_JOBS:-2}"
    SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$PERIGEE_SOURCE_ROOT" log -1 --format=%ct)}"
    export SOURCE_DATE_EPOCH

    case "$PERIGEE_VERSION" in
        ''|*[!0-9A-Za-z.+~-]*) perigee_fail "app/version.txt is invalid" ;;
    esac
    [ "$PERIGEE_ARCH" = x86_64 ] || perigee_fail "unsupported release architecture: $PERIGEE_ARCH"
    case "$SOURCE_DATE_EPOCH" in
        ''|*[!0-9]*) perigee_fail "SOURCE_DATE_EPOCH must be an integer" ;;
    esac
    case "$PERIGEE_JOBS" in
        ''|*[!0-9]*) perigee_fail "PERIGEE_JOBS must be a positive integer" ;;
        0) perigee_fail "PERIGEE_JOBS must be a positive integer" ;;
    esac

    for tool in qmake6 make python3 patchelf objcopy strip qtpaths6 readelf realpath; do
        command -v "$tool" >/dev/null 2>&1 || perigee_fail "required tool is missing: $tool"
    done
    mkdir -p -- "$PERIGEE_OUTPUT_DIR"
    export LANG=C.UTF-8
    export LC_ALL=C.UTF-8
    export TZ=UTC
    export CCACHE_DISABLE=1
}

perigee_reset_directory()
{
    local directory="$1"
    [ "$directory" = "$PERIGEE_BUILD_DIR" ] || [ "$directory" = "$PERIGEE_STAGE_DIR" ] \
        || perigee_fail "refusing to reset an unregistered packaging directory: $directory"
    case "$directory" in
        "$PERIGEE_SOURCE_ROOT"/build/*|/tmp/perigee-*) ;;
        *) perigee_fail "refusing to reset unsafe packaging directory: $directory" ;;
    esac
    local canonical
    canonical="$(realpath --canonicalize-missing -- "$directory")" \
        || perigee_fail "cannot canonicalize packaging directory: $directory"
    [ "$canonical" = "$directory" ] \
        || perigee_fail "packaging directory is not canonical: $directory"
    [ "$directory" != "$PERIGEE_SOURCE_ROOT" ] \
        || perigee_fail "refusing to reset the source checkout: $directory"
    case "$PERIGEE_SOURCE_ROOT/" in
        "$canonical/"*) perigee_fail "packaging directory contains the source checkout: $directory" ;;
    esac
    [ ! -L "$directory" ] \
        || perigee_fail "packaging directory must not be a symlink: $directory"
    rm -rf -- "$directory"
    mkdir -p -- "$directory"
}

perigee_build_release()
{
    if [ -n "${PERIGEE_BINARY:-}" ]; then
        [ -x "$PERIGEE_BINARY" ] || perigee_fail "PERIGEE_BINARY is not executable: $PERIGEE_BINARY"
        printf '%s\n' "$PERIGEE_BINARY"
        return 0
    fi

    perigee_reset_directory "$PERIGEE_BUILD_DIR"
    local map_flags
    map_flags="-ffile-prefix-map=$PERIGEE_SOURCE_ROOT=/usr/src/perigee -fdebug-prefix-map=$PERIGEE_SOURCE_ROOT=/usr/src/perigee -ffile-prefix-map=$PERIGEE_BUILD_DIR=/usr/src/perigee-build -fdebug-prefix-map=$PERIGEE_BUILD_DIR=/usr/src/perigee-build"
    (
        cd "$PERIGEE_BUILD_DIR" || exit 1
        CFLAGS="${CFLAGS:-} $map_flags" \
        CXXFLAGS="${CXXFLAGS:-} $map_flags" \
        qmake6 "$PERIGEE_SOURCE_ROOT/moonlight-qt.pro" \
            CONFIG+=release \
            CONFIG-=debug \
            QMAKE_CFLAGS+="$map_flags" \
            QMAKE_CXXFLAGS+="$map_flags" \
            QMAKE_CFLAGS_RELEASE+="$map_flags" \
            QMAKE_CXXFLAGS_RELEASE+="$map_flags" \
            QMAKE_LFLAGS+="-Wl,--build-id=none" >&2
        make -j"$PERIGEE_JOBS" release >&2
    )
    [ -x "$PERIGEE_BUILD_DIR/app/perigee" ] || perigee_fail "release build did not produce app/perigee"
    printf '%s\n' "$PERIGEE_BUILD_DIR/app/perigee"
}

perigee_assert_native_wayland()
{
    local binary="$1"
    local dynamic
    dynamic="$(readelf --dynamic --wide -- "$binary")" \
        || perigee_fail "cannot inspect native Wayland linkage: $binary"
    case "$dynamic" in
        *'Shared library: [libwayland-client.so'*) ;;
        *) perigee_fail "release binary is missing native libwayland-client.so linkage" ;;
    esac
    case "$dynamic" in
        *'Shared library: [libva-wayland.so'*) ;;
        *) perigee_fail "release binary is missing native libva-wayland.so linkage" ;;
    esac
}

perigee_stage_payload()
{
    local layout="$1"
    local binary="$2"
    perigee_assert_native_wayland "$binary"
    perigee_reset_directory "$PERIGEE_STAGE_DIR"
    python3 -B "$PERIGEE_SCRIPT_DIR/lib/stage_linux_payload.py" \
        --source-root "$PERIGEE_SOURCE_ROOT" \
        --binary "$binary" \
        --destination "$PERIGEE_STAGE_DIR" \
        --layout "$layout" \
        --epoch "$SOURCE_DATE_EPOCH"
}

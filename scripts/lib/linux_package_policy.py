#!/usr/bin/env python3
"""Shared Linux artifact policy for staging and verification."""

from __future__ import annotations

import hashlib
import re

HOST_GRAPHICS_AND_WAYLAND_PREFIXES = (
    "libwayland-client.so",
    "libwayland-egl.so",
    "libEGL.so",
    "libGL.so",
    "libGLES",
    "libGLX.so",
    "libOpenGL.so",
    "libGLdispatch.so",
    "libgbm.so",
    "libEGL_mesa.so",
    "libGLX_mesa.so",
    "libglapi.so",
)

HOST_RUNTIME_NEEDED = frozenset(
    {
        "ld-linux-x86-64.so.2",
        "libc.so.6",
        "libdl.so.2",
        "libm.so.6",
        "libpthread.so.0",
        "libresolv.so.2",
        "librt.so.1",
        "libutil.so.1",
        "libwayland-client.so.0",
        "libwayland-egl.so.1",
        "libEGL.so.1",
        "libGL.so.1",
        "libGLESv1_CM.so.1",
        "libGLESv2.so.2",
        "libGLX.so.0",
        "libOpenGL.so.0",
        "libGLdispatch.so.0",
        "libgbm.so.1",
    }
)

PRIVATE_KEY_MARKERS = (
    b"-----BEGIN PRIVATE KEY-----",
    b"-----BEGIN OPENSSH PRIVATE KEY-----",
    b"-----BEGIN ENCRYPTED PRIVATE KEY-----",
    b"-----BEGIN DSA PRIVATE KEY-----",
    b"-----BEGIN RSA PRIVATE KEY-----",
    b"-----BEGIN EC PRIVATE KEY-----",
    b"-----BEGIN PGP PRIVATE KEY BLOCK-----",
)

STATIC_SOURCE_COMPONENTS = (
    "source/perigee",
    "source/moonlight-common",
    "source/enet",
    "source/nanors",
    "source/qmdnsengine",
    "source/h264bitstream",
    "source/sdl-controller-db",
)

APP_RUN_TEXT = (
    "#!/bin/sh\n"
    "set -eu\n"
    "APPDIR=$(CDPATH='' cd -- \"$(dirname -- \"$0\")\" && pwd -P)\n"
    "export LD_LIBRARY_PATH=\"$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\n"
    "export QT_PLUGIN_PATH=\"$APPDIR/usr/plugins\"\n"
    "export QML2_IMPORT_PATH=\"$APPDIR/usr/qml\"\n"
    "exec \"$APPDIR/usr/bin/perigee\" \"$@\"\n"
)


def safe_identity(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9.+_-]", "-", value).strip("-")


def elf_license_component(relative: str, soname: str | None) -> str:
    """Return a verifier-reproducible component identity for one non-app ELF."""
    if soname:
        identity = safe_identity(soname)
        if identity:
            return f"elf/soname-{identity}"
    digest = hashlib.sha256(relative.encode("utf-8")).hexdigest()[:16]
    readable = safe_identity(relative.replace("/", "_"))[:80]
    return f"elf/path-{readable}-{digest}"

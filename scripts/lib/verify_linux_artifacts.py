#!/usr/bin/env python3
"""Extract and validate Perigee Linux release payloads."""

from __future__ import annotations

import argparse
import hashlib
import mmap
import os
import pathlib
import re
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tarfile
import time
import xml.etree.ElementTree as ElementTree
from typing import BinaryIO, Iterable, NamedTuple

from linux_package_policy import (
    APP_RUN_TEXT,
    HOST_GRAPHICS_AND_WAYLAND_PREFIXES,
    HOST_RUNTIME_NEEDED,
    PRIVATE_KEY_MARKERS,
    STATIC_SOURCE_COMPONENTS,
    elf_license_component,
)


SOURCE_ROOT = pathlib.Path(__file__).resolve().parents[2]
APP_ID = "app.perigee_stream.Perigee"
SOURCE_LICENSE_IDENTITIES = {
    "source/perigee/LICENSE": "LICENSE",
    "source/perigee/NOTICE.md": "NOTICE.md",
    "source/moonlight-common/LICENSE.txt": "moonlight-common-c/moonlight-common-c/LICENSE.txt",
    "source/enet/LICENSE": "moonlight-common-c/moonlight-common-c/enet/LICENSE",
    "source/nanors/LICENSE": "moonlight-common-c/moonlight-common-c/nanors/LICENSE",
    "source/qmdnsengine/LICENSE.txt": "qmdnsengine/qmdnsengine/LICENSE.txt",
    "source/h264bitstream/LICENSE": "h264bitstream/LICENSE",
    "source/sdl-controller-db/LICENSE": "app/SDL_GameControllerDB/LICENSE",
}
SHORT_SPDX_EXPRESSION = re.compile(
    rb"(?:MIT|ISC|Zlib|Unlicense|[A-Z][A-Za-z0-9.+-]*-[0-9][A-Za-z0-9.+-]*)"
    rb"(?:\s+(?:AND|OR|WITH)\s+"
    rb"(?:MIT|ISC|Zlib|Unlicense|[A-Z][A-Za-z0-9.+-]*-[0-9][A-Za-z0-9.+-]*))*"
)
LICENSE_EVIDENCE_GROUPS = (
    (b"copyright",),
    (b"permission", b"permitted"),
    (b"license", b"licence", b"licensing"),
    (b"public domain",),
    (b"redistribut", b"distribute"),
    (b"warranty", b"liability"),
    (b"patent",),
    (b"grant",),
    (b"terms", b"conditions", b"restrictions"),
    (b"free software", b"free of charge"),
    (b"use, copy", b"use and copy"),
    (b"lgpl", b"gpl", b"mit license", b"apache license", b"mozilla public license"),
    (b"see the file", b"licensing details"),
)
LICENSE_FILE_NAME_MARKERS = (
    "LICENSE",
    "LICENCE",
    "COPYING",
    "COPYRIGHT",
    "NOTICE",
    "PATENT",
)
REQUIRED_QML_MODULES = (
    "QML",
    "QtCore",
    "QtQml",
    "QtQml/Models",
    "QtQml/WorkerScript",
    "QtQuick",
    "QtQuick/Controls",
    "QtQuick/Controls/Material",
    "QtQuick/Layouts",
    "QtQuick/Templates",
    "QtQuick/Window",
)

WAYLAND_PLATFORM_PLUGINS = (
    "plugins/platforms/libqwayland.so",
    "plugins/platforms/libqwayland-generic.so",
    "plugins/platforms/libqwayland-egl.so",
)
REVIEWED_LICENSE_CATALOG_PATH = (
    SOURCE_ROOT / "scripts/lib/reviewed_linux_license_digests.tsv"
)
FORBIDDEN_CONTENT = (
    (b"TEST ONLY - Perigee TLS fixture", "test TLS fixture"),
    (b"PERIGEE_TEST_SESSION_TOKEN", "test session token"),
    (b"PRIVATE_HOST_SENTINEL", "private host data"),
    (b"/home/", "build-home path"),
    (b"/run/user/", "user runtime path"),
)
TEMPORARY_PATH_PATTERN = re.compile(
    rb"/(?:var/)?tmp/(?:[^\x00\r\n ]*/)*(?:perigee[^\x00\r\n /]*|"
    rb"build[^\x00\r\n /]*|worktree[^\x00\r\n /]*|tmp\.[^\x00\r\n /]+)"
)
SECRET_PATTERNS = (
    (re.compile(rb"(?<![A-Za-z0-9_])gh[pousr]_[A-Za-z0-9]{36,255}(?![A-Za-z0-9])"), "GitHub token"),
    (re.compile(rb"(?<![A-Z0-9])(?:AKIA|ASIA)[A-Z0-9]{16}(?![A-Z0-9])"), "AWS access key"),
    (
        re.compile(
            rb"(?<![A-Za-z0-9_-])eyJ[A-Za-z0-9_-]{8,}\."
            rb"[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}(?![A-Za-z0-9_-])"
        ),
        "JWT",
    ),
)


class VerificationError(RuntimeError):
    pass


class SystemProvenanceRow(NamedTuple):
    component: str
    manager: str
    package: str
    version: str
    library_source: pathlib.Path
    license_package: str
    license_version: str
    license_source: pathlib.Path
    packaged: str
    digest: str


def fail(message: str) -> None:
    raise VerificationError(message)


def load_reviewed_license_digests(path: pathlib.Path) -> dict[str, str]:
    if not path.is_file() or path.is_symlink():
        fail(f"missing reviewed license catalog: {path}")
    result: dict[str, str] = {}
    entries: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        entries.append(line)
        match = re.fullmatch(r"([0-9a-f]{64})  (elf/[^\r\n]+)", line)
        if match is None:
            fail("invalid reviewed license catalog entry")
        digest, relative = match.groups()
        if normalized_path(relative).as_posix() != relative:
            fail("unsafe reviewed license catalog path")
        if relative in result:
            fail("duplicate reviewed license catalog entry")
        result[relative] = digest
    if not result:
        fail("empty reviewed license catalog")
    if entries != sorted(entries):
        fail("reviewed license catalog is not sorted")
    return result


def normalized_path(name: str) -> pathlib.PurePosixPath:
    if not name or "\x00" in name or "\n" in name or "\r" in name:
        fail("unsafe archive path")
    path = pathlib.PurePosixPath(name)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        fail(f"unsafe archive path: {name!r}")
    return path


def normalized_absolute_path(name: str) -> pathlib.Path:
    if not name or "\x00" in name or "\n" in name or "\r" in name:
        fail("unsafe provenance path")
    pure = pathlib.PurePosixPath(name)
    if not pure.is_absolute() or any(part in ("", ".", "..") for part in pure.parts[1:]):
        fail(f"unsafe provenance path: {name!r}")
    return pathlib.Path(pure)


def run_dpkg_query(arguments: list[str]) -> str:
    if shutil.which("dpkg-query") is None:
        fail("system license provenance requires dpkg-query")
    result = subprocess.run(
        ["dpkg-query", *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        fail(f"dpkg-query rejected system provenance: {' '.join(arguments)}")
    return result.stdout


def dpkg_owners(path: pathlib.Path) -> set[str]:
    output = run_dpkg_query(["-S", path.as_posix()])
    return {
        fields[0]
        for line in output.splitlines()
        if len(fields := line.rsplit(": ", 1)) == 2
        and fields[1] == path.as_posix()
    }


def run_rpm_query(arguments: list[str]) -> str:
    if shutil.which("rpm") is None:
        fail("system license provenance requires rpm")
    result = subprocess.run(
        ["rpm", *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        fail(f"rpm rejected system provenance: {' '.join(arguments)}")
    return result.stdout


def rpm_owners(path: pathlib.Path) -> set[str]:
    output = run_rpm_query(["-qf", "--qf", "%{NAME}.%{ARCH}\n", path.as_posix()])
    return {
        line
        for line in output.splitlines()
        if line.endswith((".x86_64", ".noarch"))
    }


REVIEWED_LICENSE_DIGESTS = load_reviewed_license_digests(
    REVIEWED_LICENSE_CATALOG_PATH
)


def safe_symlink_target(path: pathlib.PurePosixPath, target: str) -> pathlib.PurePosixPath:
    if not target or "\x00" in target or "\n" in target or "\r" in target:
        fail(f"unsafe symlink target: {path}")
    target_path = pathlib.PurePosixPath(target)
    if target_path.is_absolute():
        fail(f"unsafe absolute symlink: {path}")
    resolved: list[str] = []
    for part in (path.parent / target_path).parts:
        if part in ("", "."):
            continue
        if part == "..":
            if not resolved:
                fail(f"symlink escapes payload: {path}")
            resolved.pop()
        else:
            resolved.append(part)
    return pathlib.PurePosixPath(*resolved)


def reject_symlink_loops(
    symlinks: dict[pathlib.PurePosixPath, pathlib.PurePosixPath],
) -> None:
    for start in symlinks:
        current = start
        visited: set[pathlib.PurePosixPath] = set()
        while current in symlinks:
            if current in visited:
                fail(f"symlink loop in archive: {start}")
            visited.add(current)
            current = symlinks[current]


def scan_metadata_value(value: str, display: str) -> None:
    try:
        data = value.encode("utf-8", errors="strict")
    except UnicodeError:
        fail(f"invalid UTF-8 archive metadata: {display}")
    for pattern, rule in FORBIDDEN_CONTENT:
        if pattern in data:
            fail(f"{rule} in archive metadata: {display}")
    for marker in PRIVATE_KEY_MARKERS:
        if marker in data:
            fail(f"private-key marker in archive metadata: {display}")
    for pattern, label in SECRET_PATTERNS:
        if pattern.search(data) is not None:
            fail(f"{label} in archive metadata: {display}")
    if TEMPORARY_PATH_PATTERN.search(data) is not None:
        fail(f"temporary path in archive metadata: {display}")


def copy_stream(source: BinaryIO, destination: pathlib.Path) -> None:
    with destination.open("wb") as output:
        shutil.copyfileobj(source, output, length=1024 * 1024)


def extract_tar_zst(archive: pathlib.Path, destination: pathlib.Path) -> None:
    destination.mkdir(parents=True, exist_ok=False)
    decoder = subprocess.Popen(
        ["zstd", "-q", "-d", "-c", "--", str(archive)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert decoder.stdout is not None
    assert decoder.stderr is not None
    seen: set[str] = set()
    path_types: dict[pathlib.PurePosixPath, str] = {}
    symlinks: dict[pathlib.PurePosixPath, pathlib.PurePosixPath] = {}
    caught: BaseException | None = None
    try:
        with tarfile.open(fileobj=decoder.stdout, mode="r|") as tar:
            for member in tar:
                path = normalized_path(member.name)
                key = path.as_posix()
                unexpected_pax = set(member.pax_headers) - {"path"}
                if unexpected_pax:
                    fail(
                        "unexpected PAX metadata: "
                        + ", ".join(sorted(unexpected_pax))
                    )
                if "path" in member.pax_headers:
                    pax_path = member.pax_headers["path"]
                    if normalized_path(pax_path).as_posix() != key:
                        fail(f"PAX path does not match member path: {key}")
                    scan_metadata_value(pax_path, f"PAX path for {key}")
                scan_metadata_value(key, f"member path {key}")
                if member.uname not in ("", "root") or member.gname not in ("", "root"):
                    fail(f"unexpected archive owner name: {key}")
                scan_metadata_value(member.uname, f"owner name for {key}")
                scan_metadata_value(member.gname, f"group name for {key}")
                if member.sparse:
                    fail(f"GNU sparse archive metadata is not allowed: {key}")
                if key in seen:
                    fail(f"duplicate archive path: {key}")
                seen.add(key)
                if member.uid != 0 or member.gid != 0:
                    fail(f"non-root archive ownership: {key}")
                if not member.issym():
                    if member.mode & (stat.S_ISUID | stat.S_ISGID):
                        fail(f"setuid or setgid archive entry: {key}")
                    if member.mode & 0o022:
                        fail(f"group-writable or world-writable archive entry: {key}")

                for depth in range(1, len(path.parts)):
                    ancestor = pathlib.PurePosixPath(*path.parts[:depth])
                    existing_type = path_types.get(ancestor)
                    if existing_type is not None and existing_type != "directory":
                        fail(f"non-directory archive ancestor: {ancestor}")
                    if existing_type is None:
                        path_types[ancestor] = "directory"
                        implicit = destination.joinpath(*ancestor.parts)
                        implicit.mkdir(exist_ok=True)
                        os.chmod(implicit, 0o755)

                output = destination.joinpath(*path.parts)
                if member.isdir():
                    existing_type = path_types.get(path)
                    if existing_type is not None and existing_type != "directory":
                        fail(f"archive entry changes type: {key}")
                    path_types[path] = "directory"
                    output.mkdir(parents=True, exist_ok=True)
                    os.chmod(output, member.mode & 0o777)
                elif member.isfile():
                    if path in path_types:
                        fail(f"archive entry changes type: {key}")
                    path_types[path] = "file"
                    extracted = tar.extractfile(member)
                    if extracted is None:
                        fail(f"cannot read archive entry: {key}")
                    copy_stream(extracted, output)
                    os.chmod(output, member.mode & 0o777)
                elif member.issym():
                    if path in path_types:
                        fail(f"archive entry changes type: {key}")
                    path_types[path] = "symlink"
                    scan_metadata_value(member.linkname, f"symlink target for {key}")
                    symlinks[path] = safe_symlink_target(path, member.linkname)
                    reject_symlink_loops(symlinks)
                    output.symlink_to(member.linkname)
                else:
                    fail(f"unsupported archive entry type: {key}")
    except BaseException as error:
        caught = error
    finally:
        decoder.stdout.close()

    if isinstance(caught, VerificationError):
        decoder.terminate()
    stderr = decoder.stderr.read().decode("utf-8", errors="replace").strip()
    result = decoder.wait()
    decoder.stderr.close()
    if isinstance(caught, VerificationError):
        raise caught
    if result != 0:
        fail(f"zstd rejected tar artifact with exit {result}: {stderr}")
    if caught is not None:
        fail(f"invalid tar payload: {caught}")


def elf_architecture(path: pathlib.Path) -> str:
    with path.open("rb") as handle:
        header = handle.read(20)
    if len(header) < 20 or header[:4] != b"\x7fELF":
        fail(f"not an ELF executable: {path.name}")
    if header[4] != 2 or header[5] != 1:
        fail(f"ELF is not little-endian 64-bit: {path.name}")
    machine = struct.unpack_from("<H", header, 18)[0]
    if machine != 62:
        fail(f"ELF architecture is not x86_64: {path.name}")
    return "x86_64"


def is_elf_file(path: pathlib.Path) -> bool:
    if not path.is_file():
        return False
    with path.open("rb") as handle:
        return handle.read(4) == b"\x7fELF"


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def squashfs_offsets(path: pathlib.Path) -> Iterable[int]:
    with path.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as data:
        start = 0
        while True:
            offset = data.find(b"hsqs", start)
            if offset < 0:
                return
            yield offset
            start = offset + 4


def extract_appimage(archive: pathlib.Path, destination: pathlib.Path) -> None:
    elf_architecture(archive)
    with archive.open("rb") as handle:
        magic = handle.read(11)
    if len(magic) < 11 or magic[8:11] != b"AI\x02":
        fail("artifact is not a type-2 AppImage")

    valid_offsets: list[int] = []
    for index, offset in enumerate(squashfs_offsets(archive)):
        if index >= 64:
            fail("too many SquashFS magic candidates in AppImage")
        result = subprocess.run(
            ["unsquashfs", "-s", "-o", str(offset), str(archive)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode == 0:
            valid_offsets.append(offset)
    if len(valid_offsets) != 1:
        fail(f"expected one AppImage SquashFS payload, found {len(valid_offsets)}")

    verify_squashfs_metadata(archive, valid_offsets[0])
    verify_squashfs_ownership(archive, valid_offsets[0])

    result = subprocess.run(
        [
            "unsquashfs",
            "-no-progress",
            "-no-xattrs",
            "-d",
            str(destination),
            "-o",
            str(valid_offsets[0]),
            str(archive),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        fail(f"cannot extract AppImage payload (unsquashfs exit {result.returncode})")


def verify_squashfs_metadata(path: pathlib.Path, offset: int) -> None:
    result = subprocess.run(
        ["unsquashfs", "-s", "-o", str(offset), str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        fail("cannot inspect SquashFS metadata")
    if "Xattrs are not stored" not in result.stdout:
        fail("SquashFS extended attributes are not allowed")


def verify_squashfs_ownership(path: pathlib.Path, offset: int) -> None:
    result = subprocess.run(
        ["unsquashfs", "-lln", "-o", str(offset), str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        fail("cannot inspect SquashFS ownership")
    owners = re.findall(r"(?m)^\S+\s+(\d+)/(\d+)\s+", result.stdout)
    if not owners:
        fail("cannot parse SquashFS ownership")
    if any(uid != "0" or gid != "0" for uid, gid in owners):
        fail("non-root SquashFS ownership")


def walk_payload(root: pathlib.Path) -> Iterable[tuple[pathlib.Path, pathlib.Path, os.stat_result]]:
    for directory, names, files in os.walk(root, topdown=True, followlinks=False):
        directory_path = pathlib.Path(directory)
        for name in sorted(names + files):
            path = directory_path / name
            relative = path.relative_to(root)
            yield relative, path, path.lstat()


def required_paths(kind: str) -> tuple[str, ...]:
    if kind == "tar":
        return (
            "bin/perigee",
            "qml/QML/qmldir",
            "qml/QtCore/qmldir",
            "qml/QtQml/qmldir",
            "qml/QtQml/Models/qmldir",
            "qml/QtQml/WorkerScript/qmldir",
            "qml/QtQuick/qmldir",
            "qml/QtQuick/Controls/qmldir",
            "qml/QtQuick/Controls/Material/qmldir",
            "qml/QtQuick/Layouts/qmldir",
            "qml/QtQuick/Templates/qmldir",
            "qml/QtQuick/Window/qmldir",
            "share/applications/app.perigee_stream.Perigee.desktop",
            "share/metainfo/app.perigee_stream.Perigee.appdata.xml",
            "share/icons/hicolor/scalable/apps/app.perigee_stream.Perigee.svg",
            "share/licenses/perigee/SHA256SUMS",
            "share/licenses/perigee/licenses.tsv",
            "share/licenses/perigee/system-provenance.tsv",
            "LICENSE",
            "NOTICE.md",
        )
    return (
        "AppRun",
        "usr/bin/perigee",
        "usr/qml/QML/qmldir",
        "usr/qml/QtCore/qmldir",
        "usr/qml/QtQml/qmldir",
        "usr/qml/QtQml/Models/qmldir",
        "usr/qml/QtQml/WorkerScript/qmldir",
        "usr/qml/QtQuick/qmldir",
        "usr/qml/QtQuick/Controls/qmldir",
        "usr/qml/QtQuick/Controls/Material/qmldir",
        "usr/qml/QtQuick/Layouts/qmldir",
        "usr/qml/QtQuick/Templates/qmldir",
        "usr/qml/QtQuick/Window/qmldir",
        "usr/share/applications/app.perigee_stream.Perigee.desktop",
        "usr/share/metainfo/app.perigee_stream.Perigee.appdata.xml",
        "usr/share/icons/hicolor/scalable/apps/app.perigee_stream.Perigee.svg",
        "usr/share/licenses/perigee/SHA256SUMS",
        "usr/share/licenses/perigee/licenses.tsv",
        "usr/share/licenses/perigee/system-provenance.tsv",
        "LICENSE",
        "NOTICE.md",
    )


def scan_file(path: pathlib.Path, display: str) -> None:
    overlap = max(
        max(len(pattern) for pattern, _ in FORBIDDEN_CONTENT),
        max(len(marker) for marker in PRIVATE_KEY_MARKERS),
        128 * 1024,
    ) - 1
    tail = b""
    offset = 0
    scan_private_key_markers = not is_elf_file(path)
    active_private_key_end: bytes | None = None
    active_private_key_offset = 0
    with path.open("rb") as handle:
        while True:
            block = handle.read(1024 * 1024)
            if not block:
                break
            data = tail + block
            base = offset - len(tail)
            for pattern, rule in FORBIDDEN_CONTENT:
                match = data.find(pattern)
                if match >= 0:
                    fail(f"{rule}: {display} at byte {base + match}")
            if scan_private_key_markers:
                for marker in PRIVATE_KEY_MARKERS:
                    match = data.find(marker)
                    if match >= 0:
                        fail(f"private-key marker: {display} at byte {base + match}")
            if active_private_key_end is not None:
                if data.find(b"\n" + active_private_key_end) >= 0:
                    fail(
                        f"private-key block: {display} "
                        f"at byte {active_private_key_offset}"
                    )
            else:
                beginnings: list[tuple[int, bytes]] = []
                for marker in PRIVATE_KEY_MARKERS:
                    start = 0
                    while True:
                        match = data.find(marker, start)
                        if match < 0:
                            break
                        suffix = data[match + len(marker) : match + len(marker) + 2]
                        if suffix.startswith(b"\n") or suffix.startswith(b"\r\n"):
                            beginnings.append((match, marker))
                            break
                        start = match + 1
                if beginnings:
                    match, marker = min(beginnings, key=lambda item: item[0])
                    active_private_key_offset = base + match
                    active_private_key_end = marker.replace(
                        b"-----BEGIN ", b"-----END ", 1
                    )
                    if data.find(
                        b"\n" + active_private_key_end,
                        match + len(marker),
                    ) >= 0:
                        fail(
                            f"private-key block: {display} "
                            f"at byte {active_private_key_offset}"
                        )
            for pattern, label in SECRET_PATTERNS:
                match = pattern.search(data)
                if match is not None:
                    fail(f"{label}: {display} at byte {base + match.start()}")
            match = TEMPORARY_PATH_PATTERN.search(data)
            if match is not None:
                fail(f"temporary path: {display} at byte {base + match.start()}")
            tail = data[-overlap:]
            offset += len(block)


def require_qml_target(module_root: pathlib.Path, target_text: str) -> None:
    target_path = normalized_path(target_text)
    target = module_root.joinpath(*target_path.parts)
    try:
        target.resolve().relative_to(module_root.resolve())
    except ValueError:
        fail(f"qmldir target escapes its module: {target_text}")
    if not target.is_file() or target.is_symlink():
        fail(f"missing qmldir target: {target.relative_to(module_root.parent)}")


def verify_qml_modules(root: pathlib.Path, kind: str) -> None:
    prefix = pathlib.Path() if kind == "tar" else pathlib.Path("usr")
    qml_root = root / prefix / "qml"
    for module in REQUIRED_QML_MODULES:
        module_root = qml_root.joinpath(*pathlib.PurePosixPath(module).parts)
        qmldir = module_root / "qmldir"
        try:
            lines = qmldir.read_text(encoding="utf-8").splitlines()
        except (OSError, UnicodeError) as error:
            fail(f"cannot parse required qmldir {module}: {error}")
        expected_uri = module.replace("/", ".")
        declared_uri: str | None = None
        runtime_references = 0
        for raw_line in lines:
            line = raw_line.split("#", 1)[0].strip()
            if not line:
                continue
            fields = line.split()
            if fields[0] == "module":
                if len(fields) != 2 or declared_uri is not None:
                    fail(f"invalid qmldir module declaration: {module}")
                declared_uri = fields[1]
                continue
            if fields[0] == "typeinfo":
                if len(fields) != 2:
                    fail(f"invalid qmldir typeinfo declaration: {module}")
                require_qml_target(module_root, fields[1])
                runtime_references += 1
                continue
            plugin_index = 1 if fields[:1] == ["plugin"] else -1
            if fields[:2] == ["optional", "plugin"]:
                plugin_index = 2
            if plugin_index >= 0:
                if len(fields) <= plugin_index:
                    fail(f"invalid qmldir plugin declaration: {module}")
                require_qml_target(module_root, f"lib{fields[plugin_index]}.so")
                runtime_references += 1
                continue
            if fields[-1].endswith(".qml"):
                require_qml_target(module_root, fields[-1])
                runtime_references += 1
        if declared_uri != expected_uri:
            fail(
                f"qmldir module mismatch: expected {expected_uri}, "
                f"found {declared_uri!r}"
            )
        if runtime_references == 0:
            fail(f"required QML module has no runtime content: {module}")


def require_source_identity(payload: pathlib.Path, source: pathlib.Path, label: str) -> None:
    if payload.read_bytes() != source.read_bytes():
        fail(f"source identity mismatch: {label}")


def desktop_values(path: pathlib.Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#") or line.startswith("[") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key] = value
    return result


def verify_metadata(root: pathlib.Path, kind: str) -> None:
    prefix = pathlib.Path() if kind == "tar" else pathlib.Path("usr")
    desktop = root / prefix / "share/applications/app.perigee_stream.Perigee.desktop"
    appstream = root / prefix / "share/metainfo/app.perigee_stream.Perigee.appdata.xml"
    icon = root / prefix / f"share/icons/hicolor/scalable/apps/{APP_ID}.svg"
    source_desktop = SOURCE_ROOT / f"app/deploy/linux/{APP_ID}.desktop"
    source_appstream = SOURCE_ROOT / f"app/deploy/linux/{APP_ID}.appdata.xml"
    source_icon = SOURCE_ROOT / "app/res/perigee.svg"
    values = desktop_values(desktop)
    if values.get("Exec") != "perigee":
        fail("desktop metadata does not launch perigee")
    icon_id = values.get("Icon")
    if not icon_id or not (icon.parent / f"{icon_id}.svg").is_file():
        fail("desktop icon does not resolve to an installed icon")

    try:
        document = ElementTree.parse(appstream)
    except ElementTree.ParseError:
        fail("invalid AppStream metadata")
    if document.findtext(".//id") != APP_ID:
        fail("AppStream metadata has the wrong component ID")
    expected_version = (SOURCE_ROOT / "app/version.txt").read_text(encoding="utf-8").strip()
    releases = document.findall(".//release")
    if not any(release.get("version") == expected_version for release in releases):
        fail("AppStream release version does not match app/version.txt")

    require_source_identity(desktop, source_desktop, "desktop metadata")
    require_source_identity(appstream, source_appstream, "AppStream metadata")
    require_source_identity(icon, source_icon, "application icon")
    require_source_identity(root / "LICENSE", SOURCE_ROOT / "LICENSE", "root LICENSE")
    require_source_identity(root / "NOTICE.md", SOURCE_ROOT / "NOTICE.md", "root NOTICE")

    if kind == "appimage":
        require_source_identity(root / f"{APP_ID}.desktop", source_desktop, "AppDir desktop metadata")
        require_source_identity(root / f"{APP_ID}.svg", source_icon, "AppDir application icon")
        require_source_identity(root / ".DirIcon", source_icon, "AppDir .DirIcon")


def elf_dynamic(path: pathlib.Path) -> str:
    result = subprocess.run(
        ["readelf", "--dynamic", "--wide", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        fail(f"cannot inspect ELF dependencies: {path}")
    return result.stdout


def elf_soname(path: pathlib.Path) -> str | None:
    match = re.search(r"\(SONAME\).*?\[([^]]+)\]", elf_dynamic(path))
    return match.group(1) if match is not None else None


def verify_elf_dependency_closure(root: pathlib.Path, elfs: list[pathlib.Path]) -> None:
    private_library_root = root / ("usr/lib" if (root / "usr/lib").is_dir() else "lib")
    for path in elfs:
        for needed in re.findall(r"\(NEEDED\).*?\[([^]]+)\]", elf_dynamic(path)):
            if needed in HOST_RUNTIME_NEEDED:
                continue
            if "/" in needed or needed in ("", ".", ".."):
                fail(f"unresolved ELF dependency: {path.relative_to(root)} needs {needed}")
            provider = private_library_root / needed
            if not provider.is_file() or provider.is_symlink() or not is_elf_file(provider):
                fail(f"unresolved ELF dependency: {path.relative_to(root)} needs {needed}")
            soname = elf_soname(provider)
            if soname != needed:
                fail(
                    f"ELF provider SONAME mismatch: {provider.relative_to(root)} "
                    f"provides {soname!r}, expected {needed!r}"
                )


def credible_license_notice(content: bytes) -> bool:
    stripped = content.strip()
    if SHORT_SPDX_EXPRESSION.fullmatch(stripped) is not None:
        return True
    if len(stripped) < 64:
        return False
    lowered = stripped.lower()
    evidence_count = sum(
        any(term in lowered for term in alternatives)
        for alternatives in LICENSE_EVIDENCE_GROUPS
    )
    return evidence_count >= 1


def parse_system_provenance(
    path: pathlib.Path,
    checksums: dict[str, str],
) -> list[SystemProvenanceRow]:
    rows: list[SystemProvenanceRow] = []
    packaged_entries: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 10:
            fail("invalid system license provenance")
        (
            component,
            manager,
            package,
            version,
            library_text,
            license_package,
            license_version,
            source_text,
            packaged,
            digest,
        ) = fields
        if normalized_path(component).as_posix() != component or not component.startswith("elf/"):
            fail("invalid system provenance component")
        if manager not in {"dpkg", "rpm"}:
            fail("unsupported system provenance manager")
        library_package_pattern = (
            r"[a-z0-9][a-z0-9+.-]*:amd64"
            if manager == "dpkg"
            else r"[A-Za-z0-9][A-Za-z0-9+._-]*\.x86_64"
        )
        license_package_pattern = (
            r"[a-z0-9][a-z0-9+.-]*:(?:amd64|all)"
            if manager == "dpkg"
            else r"[A-Za-z0-9][A-Za-z0-9+._-]*\.(?:x86_64|noarch)"
        )
        if re.fullmatch(library_package_pattern, package) is None:
            fail("system provenance package must include x86_64 architecture")
        if re.fullmatch(license_package_pattern, license_package) is None:
            fail("system provenance license package must include architecture")
        if not version or any(character in version for character in "\r\n\t"):
            fail("invalid system provenance package version")
        if not license_version or any(character in license_version for character in "\r\n\t"):
            fail("invalid system provenance license package version")
        library_source = normalized_absolute_path(library_text)
        license_source = normalized_absolute_path(source_text)
        if normalized_path(packaged).as_posix() != packaged:
            fail("invalid packaged system license path")
        if not packaged.startswith(component + "/"):
            fail("system provenance component/path mismatch")
        if re.fullmatch(r"[0-9a-f]{64}", digest) is None:
            fail("invalid system provenance digest")
        if checksums.get(packaged) != digest:
            fail("system provenance does not match license digest manifest")
        if packaged in packaged_entries:
            fail("duplicate system provenance packaged path")
        packaged_entries.add(packaged)
        rows.append(
            SystemProvenanceRow(
                component,
                manager,
                package,
                version,
                library_source,
                license_package,
                license_version,
                license_source,
                packaged,
                digest,
            )
        )
    return rows


def verify_system_component_provenance(
    root: pathlib.Path,
    component: str,
    payload_elfs: list[pathlib.Path],
    component_files: list[pathlib.Path],
    rows: list[SystemProvenanceRow],
    license_root: pathlib.Path,
    owner_cache: dict[tuple[str, pathlib.Path], set[str]] | None = None,
    package_cache: dict[tuple[str, str], tuple[str, set[str]]] | None = None,
) -> None:
    if owner_cache is None:
        owner_cache = {}
    if package_cache is None:
        package_cache = {}
    rpm_source_cache: dict[str, str] = {}

    def owners_for(manager_name: str, path: pathlib.Path) -> set[str]:
        key = (manager_name, path)
        if key not in owner_cache:
            owner_cache[key] = (
                dpkg_owners(path) if manager_name == "dpkg" else rpm_owners(path)
            )
        return owner_cache[key]

    def package_info(manager_name: str, package_name: str) -> tuple[str, set[str]]:
        key = (manager_name, package_name)
        if key not in package_cache:
            if manager_name == "dpkg":
                found_version = run_dpkg_query(
                    ["-W", "-f=${Version}", package_name]
                ).strip()
                listing = run_dpkg_query(["-L", package_name])
            else:
                found_version = run_rpm_query(
                    ["-q", "--qf", "%{EVR}", package_name]
                ).strip()
                listing = run_rpm_query(["-ql", package_name])
            package_cache[key] = (
                found_version,
                {line for line in listing.splitlines() if line},
            )
        return package_cache[key]

    def rpm_source_package(package_name: str) -> str:
        if package_name not in rpm_source_cache:
            source_package = run_rpm_query(
                ["-q", "--qf", "%{SOURCERPM}", package_name]
            ).strip()
            if not source_package or source_package == "(none)":
                fail(f"system provenance package has no source RPM: {component}")
            rpm_source_cache[package_name] = source_package
        return rpm_source_cache[package_name]

    if not rows:
        fail(f"missing system license provenance: {component}")
    identities = {
        (row.manager, row.package, row.version, row.library_source)
        for row in rows
    }
    if len(identities) != 1:
        fail(f"inconsistent system license provenance: {component}")
    manager, package, version, library_source = next(iter(identities))
    if manager not in {"dpkg", "rpm"}:
        fail(f"unsupported system provenance manager: {manager}")
    if len(payload_elfs) != 1:
        fail(f"system provenance must identify one payload ELF: {component}")
    payload_soname = elf_soname(payload_elfs[0])
    if payload_soname is None or elf_license_component(
        payload_elfs[0].relative_to(root).as_posix(), payload_soname
    ) != component:
        fail(f"system provenance SONAME mismatch: {component}")

    if owners_for(manager, library_source) != {package}:
        fail(f"system provenance library owner mismatch: {component}")
    installed_version, listed_paths = package_info(manager, package)
    if installed_version != version:
        fail(f"system provenance package version mismatch: {component}")
    if library_source.as_posix() not in listed_paths:
        fail(f"system provenance library is not package-listed: {component}")
    if not library_source.is_file():
        fail(f"system provenance library is missing: {component}")
    elf_architecture(library_source)
    if elf_soname(library_source) != payload_soname:
        fail(f"system provenance provider SONAME mismatch: {component}")

    expected_packaged = {
        path.relative_to(license_root).as_posix() for path in component_files
    }
    if {row.packaged for row in rows} != expected_packaged:
        fail(f"system provenance license path set mismatch: {component}")
    for row in rows:
        if row.license_package not in owners_for(manager, row.license_source):
            fail(f"system provenance license owner mismatch: {component}")
        license_version, license_paths = package_info(
            manager, row.license_package
        )
        if license_version != row.license_version:
            fail(f"system provenance license package version mismatch: {component}")
        if row.license_source.as_posix() not in license_paths:
            fail(f"system provenance license is not package-listed: {component}")
        if row.license_source.as_posix() not in listed_paths:
            same_source_rpm = manager == "rpm" and rpm_source_package(
                package
            ) == rpm_source_package(row.license_package)
            if not same_source_rpm:
                fail(f"system provenance license is unrelated to package: {component}")
        if not row.license_source.is_file():
            fail(f"system provenance license is missing: {component}")
        source_digest = hashlib.sha256(row.license_source.read_bytes()).hexdigest()
        packaged_path = license_root.joinpath(*normalized_path(row.packaged).parts)
        if source_digest != row.digest or packaged_path.read_bytes() != row.license_source.read_bytes():
            fail(f"system provenance license bytes mismatch: {component}")


def verify_licenses(root: pathlib.Path, kind: str, elfs: list[pathlib.Path]) -> None:
    prefix = pathlib.Path() if kind == "tar" else pathlib.Path("usr")
    license_root = root / prefix / "share/licenses/perigee"
    checksum_path = license_root / "SHA256SUMS"
    mapping_path = license_root / "licenses.tsv"
    provenance_path = license_root / "system-provenance.tsv"
    checksums: dict[str, str] = {}
    for line in checksum_path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([^\r\n]+)", line)
        if match is None:
            fail("invalid license digest manifest")
        digest, relative_text = match.groups()
        relative = normalized_path(relative_text)
        if relative_text in checksums:
            fail("duplicate license digest entry")
        target = license_root.joinpath(*relative.parts)
        if not target.is_file() or target.is_symlink():
            fail(f"missing license digest target: {relative_text}")
        if hashlib.sha256(target.read_bytes()).hexdigest() != digest:
            fail(f"license digest mismatch: {relative_text}")
        checksums[relative_text] = digest
    if not checksums:
        fail("empty license digest manifest")
    actual_license_files = {
        path.relative_to(license_root).as_posix()
        for path in license_root.rglob("*")
        if path.is_file()
        and path.name not in {"SHA256SUMS", "licenses.tsv", "system-provenance.tsv"}
    }
    if set(checksums) != actual_license_files:
        fail("license digest manifest does not cover the license tree exactly")

    provenance_rows = parse_system_provenance(provenance_path, checksums)
    provenance_by_packaged = {row.packaged: row for row in provenance_rows}
    for relative, digest in checksums.items():
        if not relative.startswith("elf/"):
            continue
        if relative in provenance_by_packaged:
            continue
        reviewed_digest = REVIEWED_LICENSE_DIGESTS.get(relative)
        if reviewed_digest is None:
            fail(f"reviewed license catalog has no entry: {relative}")
        if reviewed_digest != digest:
            fail(f"reviewed license catalog digest mismatch: {relative}")

    for packaged, source in SOURCE_LICENSE_IDENTITIES.items():
        target = license_root.joinpath(*normalized_path(packaged).parts)
        if target.read_bytes() != (SOURCE_ROOT / source).read_bytes():
            fail(f"license source identity mismatch: {packaged}")
        if packaged not in checksums:
            fail(f"license digest missing source identity: {packaged}")

    mappings: dict[str, set[str]] = {}
    mapping_pairs: set[tuple[str, str]] = set()
    for line in mapping_path.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 2:
            fail("invalid ELF license mapping")
        elf_text, component_text = fields
        elf_relative = normalized_path(elf_text).as_posix()
        component = normalized_path(component_text)
        pair = (elf_relative, component.as_posix())
        if pair in mapping_pairs:
            fail("duplicate ELF license mapping")
        mapping_pairs.add(pair)
        component_root = license_root.joinpath(*component.parts)
        if not component_root.is_dir():
            fail(f"ELF license mapping references missing component: {component_text}")
        component_prefix = component.as_posix() + "/"
        if not any(entry.startswith(component_prefix) for entry in checksums):
            fail(f"ELF license mapping references empty component: {component_text}")
        mappings.setdefault(elf_relative, set()).add(component.as_posix())
    expected = {path.relative_to(root).as_posix() for path in elfs}
    if set(mappings) != expected:
        fail("ELF license mapping does not cover the payload exactly")

    for path in elfs:
        elf_relative = path.relative_to(root).as_posix()
        prefix_relative = path.relative_to(root / prefix).as_posix()
        if prefix_relative == "bin/perigee":
            expected_components = set(STATIC_SOURCE_COMPONENTS)
        else:
            expected_components = {
                elf_license_component(prefix_relative, elf_soname(path))
            }
        if mappings[elf_relative] != expected_components:
            fail(f"license component identity mismatch: {elf_relative}")

    mapped_components = set().union(*mappings.values())
    for entry in checksums:
        owners = [component for component in mapped_components if entry.startswith(component + "/")]
        if len(owners) != 1:
            fail(f"unused license component or ambiguous license path: {entry}")

    for component in mapped_components:
        component_prefix = component + "/"
        component_files = [
            license_root.joinpath(*normalized_path(entry).parts)
            for entry in checksums
            if entry.startswith(component_prefix)
        ]
        if not any(credible_license_notice(path.read_bytes()) for path in component_files):
            fail(f"component has no credible license notice: {component}")
        for path in component_files:
            if (
                any(marker in path.name.upper() for marker in LICENSE_FILE_NAME_MARKERS)
                and not credible_license_notice(path.read_bytes())
            ):
                fail(f"component has a non-credible license notice: {path.relative_to(license_root)}")

    provenance_by_component: dict[str, list[SystemProvenanceRow]] = {}
    for row in provenance_rows:
        provenance_by_component.setdefault(row.component, []).append(row)
    if not set(provenance_by_component).issubset(mapped_components):
        fail("system provenance references an unmapped component")
    component_elfs: dict[str, list[pathlib.Path]] = {}
    for elf_relative, components in mappings.items():
        for component in components:
            component_elfs.setdefault(component, []).append(
                root.joinpath(*normalized_path(elf_relative).parts)
            )
    provenance_owner_cache: dict[tuple[str, pathlib.Path], set[str]] = {}
    provenance_package_cache: dict[tuple[str, str], tuple[str, set[str]]] = {}
    for component, rows in provenance_by_component.items():
        component_prefix = component + "/"
        component_files = [
            license_root.joinpath(*normalized_path(entry).parts)
            for entry in checksums
            if entry.startswith(component_prefix)
        ]
        verify_system_component_provenance(
            root,
            component,
            component_elfs.get(component, []),
            component_files,
            rows,
            license_root,
            provenance_owner_cache,
            provenance_package_cache,
        )


def verify_elf_metadata(path: pathlib.Path, display: pathlib.Path) -> None:
    result = subprocess.run(
        ["readelf", "-d", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        fail(f"cannot inspect ELF metadata: {display}")
    for line in result.stdout.splitlines():
        if "(RPATH)" not in line and "(RUNPATH)" not in line:
            continue
        match = re.search(r"\[([^]]*)\]", line)
        if match is None:
            fail(f"cannot parse ELF runtime path: {display}")
        if any(entry.startswith("/") for entry in match.group(1).split(":")):
            fail(f"absolute ELF runtime path: {display}")
    sections = subprocess.run(
        ["readelf", "-SW", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if sections.returncode != 0:
        fail(f"cannot inspect ELF sections: {display}")
    if ".note.gnu.build-id" in sections.stdout:
        fail(f"ELF build ID is not reproducible metadata: {display}")


def verify_tree(root: pathlib.Path, kind: str) -> None:
    missing = [entry for entry in required_paths(kind) if not (root / entry).exists()]
    if missing:
        fail(f"missing payload path: {missing[0]}")
    prefix = pathlib.Path() if kind == "tar" else pathlib.Path("usr")
    if not any((root / prefix / relative).is_file() for relative in WAYLAND_PLATFORM_PLUGINS):
        fail("missing Qt Wayland platform plugin")

    executable = root / ("bin/perigee" if kind == "tar" else "usr/bin/perigee")
    if not executable.is_file() or not os.access(executable, os.X_OK):
        fail(f"missing or non-executable packaged binary: {executable.relative_to(root)}")
    elf_architecture(executable)
    if kind == "appimage":
        apprun = root / "AppRun"
        if not apprun.is_file() or not os.access(apprun, os.X_OK):
            fail("AppRun is not a regular executable")
        if apprun.read_bytes() != APP_RUN_TEXT.encode("utf-8"):
            fail("AppRun content does not match the packaged entry point")

    private_library_root = root / ("lib" if kind == "tar" else "usr/lib")
    if not private_library_root.is_dir() or not any(private_library_root.glob("*.so*")):
        fail("missing required private runtime libraries")

    file_count = 0
    elfs: list[pathlib.Path] = []
    for relative, path, metadata in walk_payload(root):
        file_count += 1
        if stat.S_ISLNK(metadata.st_mode):
            fail(f"payload symlink is not allowed: {relative}")
        if metadata.st_mode & (stat.S_ISUID | stat.S_ISGID):
            fail(f"setuid or setgid payload entry: {relative}")
        if metadata.st_mode & 0o022:
            fail(f"group-writable or world-writable payload entry: {relative}")
        if stat.S_ISDIR(metadata.st_mode):
            continue
        if not stat.S_ISREG(metadata.st_mode):
            fail(f"unsupported payload entry type: {relative}")

        lower_name = relative.name.lower()
        lower_path = relative.as_posix().lower()
        if any(lower_name.startswith(item.lower()) for item in HOST_GRAPHICS_AND_WAYLAND_PREFIXES):
            fail(f"bundled host graphics or Wayland library: {relative}")
        if "/dri/" in f"/{lower_path}/" and lower_name.endswith("_dri.so"):
            fail(f"bundled Mesa driver: {relative}")
        if lower_name.endswith((".key", ".pem")) or "/tests/fixtures/tls/" in f"/{lower_path}/":
            fail(f"test or private key file in payload: {relative}")
        if is_elf_file(path):
            elf_architecture(path)
            verify_elf_metadata(path, relative)
            elfs.append(path)
        scan_file(path, relative.as_posix())
    if file_count == 0:
        fail("empty payload")
    verify_qml_modules(root, kind)
    verify_elf_dependency_closure(root, elfs)
    verify_licenses(root, kind, elfs)
    verify_metadata(root, kind)


def command_extract_tar(args: argparse.Namespace) -> None:
    extract_tar_zst(args.archive, args.destination)


def command_extract_appimage(args: argparse.Namespace) -> None:
    extract_appimage(args.archive, args.destination)


def command_verify_tree(args: argparse.Namespace) -> None:
    verify_tree(args.root, args.kind)


def command_scan_file(args: argparse.Namespace) -> None:
    scan_file(args.path, args.path.name)


def validate_runtime(path: pathlib.Path) -> None:
    elf_architecture(path)
    if path.stat().st_size > 4 * 1024 * 1024:
        fail("AppImage runtime is unexpectedly large")
    with path.open("rb") as handle:
        magic = handle.read(11)
    if len(magic) < 11 or magic[8:11] != b"AI\x02":
        fail("AppImage runtime is not type 2")
    scan_file(path, path.name)
    for offset in squashfs_offsets(path):
        result = subprocess.run(
            ["unsquashfs", "-s", "-o", str(offset), str(path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode == 0:
            fail("AppImage runtime already contains a SquashFS payload")


def command_validate_runtime(args: argparse.Namespace) -> None:
    validate_runtime(args.path)


def verify_version(executable: pathlib.Path, expected: str, work_root: pathlib.Path) -> None:
    for name in ("config", "cache", "data", "runtime", "home"):
        (work_root / name).mkdir(parents=True, exist_ok=True)
    os.chmod(work_root / "runtime", 0o700)
    environment = {
        "HOME": str(work_root / "home"),
        "PATH": "/usr/bin:/bin",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
        "XDG_CONFIG_HOME": str(work_root / "config"),
        "XDG_CACHE_HOME": str(work_root / "cache"),
        "XDG_DATA_HOME": str(work_root / "data"),
        "XDG_RUNTIME_DIR": str(work_root / "runtime"),
        "QT_QPA_PLATFORM": "offscreen",
        "SDL_VIDEODRIVER": "dummy",
        "APPIMAGE_EXTRACT_AND_RUN": "1",
    }
    try:
        result = subprocess.run(
            [str(executable), "--version"],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=20,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        fail(f"packaged --version command failed: {executable.name}")
    if result.returncode != 0:
        fail(f"packaged --version command failed: {executable.name}")
    try:
        output = result.stdout.decode("utf-8").rstrip("\n")
    except UnicodeDecodeError:
        fail(f"version output is not UTF-8: {executable.name}")
    if output != expected or b"\x00" in result.stdout:
        fail(f"unexpected version output from {executable.name}")


def command_verify_version(args: argparse.Namespace) -> None:
    verify_version(args.executable, args.expected, args.work_root)


def process_parent(pid: int) -> int | None:
    try:
        fields = pathlib.Path(f"/proc/{pid}/stat").read_text(encoding="ascii").split()
        return int(fields[3])
    except (FileNotFoundError, IndexError, PermissionError, ValueError):
        return None


def descendants(parent_pid: int) -> set[int]:
    result = {parent_pid}
    changed = True
    while changed:
        changed = False
        for entry in pathlib.Path("/proc").iterdir():
            if not entry.name.isdigit():
                continue
            pid = int(entry.name)
            if pid not in result and process_parent(pid) in result:
                result.add(pid)
                changed = True
    return result


def executable_path(pid: int) -> pathlib.Path | None:
    try:
        return pathlib.Path(os.readlink(f"/proc/{pid}/exe")).resolve()
    except (FileNotFoundError, PermissionError, OSError):
        return None


def process_matches_expected_binary(pid: int, expected_digest: str) -> bool:
    candidate = executable_path(pid)
    if candidate is None or not candidate.is_file():
        return False
    try:
        return file_sha256(candidate) == expected_digest
    except (FileNotFoundError, OSError, PermissionError):
        return False


def socket_inodes(pids: Iterable[int]) -> set[str]:
    result: set[str] = set()
    for pid in pids:
        directory = pathlib.Path(f"/proc/{pid}/fd")
        try:
            entries = list(directory.iterdir())
        except (FileNotFoundError, PermissionError):
            continue
        for entry in entries:
            try:
                target = os.readlink(entry)
            except (FileNotFoundError, PermissionError, OSError):
                continue
            if target.startswith("socket:[") and target.endswith("]"):
                result.add(target[8:-1])
    return result


def tcp_listeners(inodes: set[str]) -> list[str]:
    matches: list[str] = []
    for table_name in ("tcp", "tcp6"):
        table = pathlib.Path(f"/proc/net/{table_name}")
        try:
            lines = table.read_text(encoding="ascii").splitlines()[1:]
        except (FileNotFoundError, PermissionError):
            continue
        for line in lines:
            fields = line.split()
            if len(fields) >= 10 and fields[3] == "0A" and fields[9] in inodes:
                matches.append(f"{table_name}:{fields[1]}")
    return matches


def process_group_has_live_members(process_group: int) -> bool:
    for stat_path in pathlib.Path("/proc").glob("[0-9]*/stat"):
        try:
            contents = stat_path.read_text(encoding="ascii")
            fields = contents[contents.rfind(")") + 2 :].split()
            state = fields[0]
            group = int(fields[2])
        except (FileNotFoundError, IndexError, PermissionError, ValueError):
            continue
        if group == process_group and state != "Z":
            return True
    return False


def terminate_process(process: subprocess.Popen[bytes]) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass

    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and process_group_has_live_members(process.pid):
        process.poll()
        time.sleep(0.05)

    if process_group_has_live_members(process.pid):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    if process.poll() is None:
        process.wait(timeout=5)


def assert_launch_liveness(
    process: subprocess.Popen[bytes],
    application_pid: int,
    expected_payload_digest: str,
) -> None:
    if process.poll() is not None:
        fail("packaged process exited during isolated Wayland launch")
    if not process_matches_expected_binary(application_pid, expected_payload_digest):
        fail("packaged process executable identity changed during isolated Wayland launch")


def launch_gate(
    executable: pathlib.Path,
    work_root: pathlib.Path,
    expected_payload_executable: pathlib.Path,
) -> None:
    if shutil.which("dbus-run-session") is None or shutil.which("kwin_wayland") is None:
        fail("isolated Wayland launch requires dbus-run-session and kwin_wayland")
    executable = executable.resolve()
    expected_payload_digest = file_sha256(expected_payload_executable)

    runtime = work_root / "runtime"
    config = work_root / "config"
    cache = work_root / "cache"
    data = work_root / "data"
    home = work_root / "home"
    for directory in (runtime, config, cache, data, home):
        directory.mkdir(parents=True, exist_ok=True)
    os.chmod(runtime, 0o700)
    socket_name = "perigee-artifact-wayland"
    environment = {
        "HOME": str(home),
        "PATH": "/usr/bin:/bin",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
        "XDG_CONFIG_HOME": str(config),
        "XDG_CACHE_HOME": str(cache),
        "XDG_DATA_HOME": str(data),
        "XDG_RUNTIME_DIR": str(runtime),
        "WAYLAND_DISPLAY": socket_name,
        "QT_QPA_PLATFORM": "wayland",
        "SDL_VIDEODRIVER": "wayland",
        "LIBGL_ALWAYS_SOFTWARE": "1",
        "NO_AT_BRIDGE": "1",
        "APPIMAGE_EXTRACT_AND_RUN": "1",
    }
    command = [
        "dbus-run-session",
        "--",
        "kwin_wayland",
        "--virtual",
        "--width",
        "1280",
        "--height",
        "720",
        "--no-lockscreen",
        "--no-global-shortcuts",
        "--no-kactivities",
        "--socket",
        socket_name,
        "--exit-with-session",
        str(executable),
    ]
    log_path = work_root / "launch.log"
    with log_path.open("wb") as log:
        process = subprocess.Popen(
            command,
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            application_pid: int | None = None
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    fail(f"isolated Wayland launch exited early with status {process.returncode}")
                for pid in descendants(process.pid):
                    if process_matches_expected_binary(pid, expected_payload_digest):
                        application_pid = pid
                        break
                if application_pid is not None:
                    break
                time.sleep(0.1)
            if application_pid is None:
                fail("packaged process did not start under isolated Wayland")

            time.sleep(2)
            assert_launch_liveness(process, application_pid, expected_payload_digest)
            owned_pids = descendants(application_pid)
            listeners = tcp_listeners(socket_inodes(owned_pids))
            if listeners:
                fail(f"packaged process owns a TCP listening socket: {listeners[0]}")
            assert_launch_liveness(process, application_pid, expected_payload_digest)
        finally:
            terminate_process(process)


def command_launch_gate(args: argparse.Namespace) -> None:
    launch_gate(args.executable, args.work_root, args.expected_payload_executable)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    subcommands = result.add_subparsers(dest="command", required=True)

    tar_parser = subcommands.add_parser("extract-tar")
    tar_parser.add_argument("archive", type=pathlib.Path)
    tar_parser.add_argument("destination", type=pathlib.Path)
    tar_parser.set_defaults(function=command_extract_tar)

    appimage_parser = subcommands.add_parser("extract-appimage")
    appimage_parser.add_argument("archive", type=pathlib.Path)
    appimage_parser.add_argument("destination", type=pathlib.Path)
    appimage_parser.set_defaults(function=command_extract_appimage)

    tree_parser = subcommands.add_parser("verify-tree")
    tree_parser.add_argument("kind", choices=("tar", "appimage"))
    tree_parser.add_argument("root", type=pathlib.Path)
    tree_parser.set_defaults(function=command_verify_tree)

    scan_parser = subcommands.add_parser("scan-file")
    scan_parser.add_argument("path", type=pathlib.Path)
    scan_parser.set_defaults(function=command_scan_file)

    runtime_parser = subcommands.add_parser("validate-runtime")
    runtime_parser.add_argument("path", type=pathlib.Path)
    runtime_parser.set_defaults(function=command_validate_runtime)

    version_parser = subcommands.add_parser("verify-version")
    version_parser.add_argument("executable", type=pathlib.Path)
    version_parser.add_argument("expected")
    version_parser.add_argument("work_root", type=pathlib.Path)
    version_parser.set_defaults(function=command_verify_version)

    launch_parser = subcommands.add_parser("launch-gate")
    launch_parser.add_argument("executable", type=pathlib.Path)
    launch_parser.add_argument("work_root", type=pathlib.Path)
    launch_parser.add_argument("expected_payload_executable", type=pathlib.Path)
    launch_parser.set_defaults(function=command_launch_gate)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        args.function(args)
    except VerificationError as error:
        print(f"artifact verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

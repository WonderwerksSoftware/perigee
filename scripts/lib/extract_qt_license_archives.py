#!/usr/bin/env python3

"""Safely collect license texts from pinned Qt source archives."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import tarfile
from pathlib import Path, PurePosixPath


QT_LICENSE_MODULES = {
    "qtbase",
    "qtsvg",
    "qtdeclarative",
    "qtwayland",
    "qtvirtualkeyboard",
}


class LicenseArchiveError(ValueError):
    """Raised when an archive does not match the reviewed license layout."""


def _license_members(archive: Path) -> tuple[str, list[tuple[str, bytes]]]:
    root = archive.name
    for suffix in (".tar.xz", ".tar.gz", ".tar.bz2", ".tar"):
        if root.endswith(suffix):
            root = root[: -len(suffix)]
            break
    suffix = "-everywhere-src-6.8.3"
    if not root.endswith(suffix):
        raise LicenseArchiveError(f"unexpected Qt source archive name: {archive.name}")
    module = root[: -len(suffix)]
    if module not in QT_LICENSE_MODULES:
        raise LicenseArchiveError(f"unsupported Qt source module: {module}")
    prefix = PurePosixPath(root) / "LICENSES"
    prefix_parts = prefix.parts
    selected: list[tuple[str, bytes]] = []
    names: set[str] = set()
    with tarfile.open(archive, mode="r:*") as handle:
        for member in handle.getmembers():
            path = PurePosixPath(member.name)
            if path.parts[: len(prefix_parts)] != prefix_parts:
                continue
            if len(path.parts) == len(prefix_parts) and member.isdir():
                continue
            if len(path.parts) != len(prefix_parts) + 1 or path.name in {"", ".", ".."}:
                raise LicenseArchiveError(f"nested or invalid Qt license member: {member.name}")
            if not member.isfile():
                raise LicenseArchiveError(f"Qt license member is not a regular file: {member.name}")
            if member.name in names:
                raise LicenseArchiveError(f"duplicate Qt license member: {member.name}")
            names.add(member.name)
            extracted = handle.extractfile(member)
            if extracted is None:
                raise LicenseArchiveError(f"cannot read Qt license member: {member.name}")
            selected.append((path.name, extracted.read()))
    if not selected:
        raise LicenseArchiveError(f"Qt source archive has no LICENSES files: {archive.name}")
    return module, selected


def extract_archives(archives: list[Path], destination: Path) -> None:
    """Validate all archives, then write their license texts into a new directory."""
    if destination.exists():
        raise LicenseArchiveError(f"license destination already exists: {destination}")
    collected: dict[tuple[str, str], bytes] = {}
    for archive in archives:
        module, members = _license_members(archive)
        for name, content in members:
            key = (module, name)
            existing = collected.get(key)
            if existing is None:
                collected[key] = content
                continue
            if existing == content:
                continue
            digest = hashlib.sha256(content).hexdigest()[:12]
            collected[(module, f"{name}.{digest}")] = content

    destination.mkdir(parents=False)
    try:
        for (module, name), content in sorted(collected.items()):
            target = destination / module / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
            target.chmod(0o644)
    except Exception:
        shutil.rmtree(destination)
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("archives", type=Path, nargs="+")
    args = parser.parse_args(argv)
    try:
        extract_archives(args.archives, args.destination)
    except (OSError, tarfile.TarError, LicenseArchiveError) as error:
        print(f"Qt license extraction failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Print a deterministic reviewed-license catalog candidate for manual review."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import sys


CATALOG_NAME = "scripts/lib/reviewed_linux_license_digests.tsv"


def fail(message: str) -> None:
    raise ValueError(message)


def license_root(payload: pathlib.Path, layout: str) -> pathlib.Path:
    prefix = pathlib.Path() if layout == "tar" else pathlib.Path("usr")
    root = payload / prefix / "share/licenses/perigee"
    if not root.is_dir():
        fail(f"missing staged license root: {root}")
    return root


def catalog_lines(payload: pathlib.Path, layout: str) -> list[str]:
    root = license_root(payload.resolve(), layout)
    manifest = root / "SHA256SUMS"
    provenance = root / "system-provenance.tsv"
    if not manifest.is_file():
        fail(f"missing staged license manifest: {manifest}")
    if not provenance.is_file():
        fail(f"missing staged system provenance: {provenance}")
    system_paths: set[str] = set()
    for line in provenance.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 10:
            fail("invalid staged system provenance")
        system_paths.add(fields[8])
    entries: list[str] = []
    seen: set[str] = set()
    for line in manifest.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (elf/[^\r\n]+)", line)
        if match is None:
            if "  source/" in line:
                continue
            fail(f"invalid non-source license manifest entry: {line!r}")
        digest, relative = match.groups()
        if relative in seen:
            fail(f"duplicate staged license entry: {relative}")
        seen.add(relative)
        target = root.joinpath(*pathlib.PurePosixPath(relative).parts)
        if not target.is_file() or target.is_symlink():
            fail(f"missing staged license file: {relative}")
        actual = hashlib.sha256(target.read_bytes()).hexdigest()
        if actual != digest:
            fail(f"staged license digest mismatch: {relative}")
        if relative in system_paths:
            continue
        entries.append(f"{digest}  {relative}")
    if not system_paths.issubset(seen):
        fail("staged system provenance has an orphan packaged path")
    if not entries:
        fail("staged artifact has no non-system dependency license entries")
    return sorted(entries)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description=(
            "Print a catalog candidate. Review its source identities and legal texts, then "
            f"redirect approved output to {CATALOG_NAME}."
        )
    )
    result.add_argument("payload", type=pathlib.Path)
    result.add_argument("--layout", choices=("tar", "appimage"), required=True)
    result.add_argument("--profile", required=True)
    return result


def main() -> int:
    try:
        args = parser().parse_args()
        if not args.profile.strip() or any(character in args.profile for character in "\r\n"):
            fail("profile must be a non-empty single line")
        print("# Perigee reviewed Linux dependency license digests, schema 1.")
        print(f"# Candidate producer profile: {args.profile.strip()}")
        print("# Format: sha256, two spaces, deterministic staged component/path.")
        print("# Regenerate a candidate with scripts/lib/generate_linux_license_catalog.py;")
        print("# review every changed source identity and legal text before replacing this file.")
        print(*catalog_lines(args.payload, args.layout), sep="\n")
    except ValueError as error:
        print(f"license catalog generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

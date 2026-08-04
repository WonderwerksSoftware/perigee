#!/usr/bin/env python3
"""Create normalized Perigee Linux payload trees."""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import sys
from typing import NamedTuple

from linux_package_policy import (
    APP_RUN_TEXT,
    HOST_GRAPHICS_AND_WAYLAND_PREFIXES,
    HOST_RUNTIME_NEEDED,
    STATIC_SOURCE_COMPONENTS,
    elf_license_component,
)

EXCLUDED_LIBRARIES = HOST_RUNTIME_NEEDED

SOURCE_COMPONENT_LICENSES = {
    "moonlight-common": ("moonlight-common-c/moonlight-common-c/LICENSE.txt",),
    "enet": ("moonlight-common-c/moonlight-common-c/enet/LICENSE",),
    "nanors": ("moonlight-common-c/moonlight-common-c/nanors/LICENSE",),
    "qmdnsengine": ("qmdnsengine/qmdnsengine/LICENSE.txt",),
    "h264bitstream": ("h264bitstream/LICENSE",),
    "sdl-controller-db": ("app/SDL_GameControllerDB/LICENSE",),
}

SOURCE_BUILD_LICENSES = {
    "sdl3": ("deps/SDL/LICENSE.txt",),
    "sdl2-compat": ("deps/sdl2-compat/LICENSE.txt",),
    "sdl-ttf": ("deps/SDL_ttf/LICENSE.txt",),
    "libva": ("deps/libva/COPYING",),
    "libplacebo": ("deps/libplacebo/LICENSE",),
    "dav1d": ("deps/dav1d/COPYING",),
    "ffmpeg": ("deps/FFmpeg/COPYING.LGPLv2.1", "deps/FFmpeg/LICENSE.md"),
}

QML_MODULES = (
    "QML",
    "QtCore",
    "QtQml/Models",
    "QtQml/WorkerScript",
    "QtQuick/Controls",
    "QtQuick/Layouts",
    "QtQuick/Templates",
    "QtQuick/Window",
)

OPTIONAL_QML_MODULES = ("QtQuick/Effects", "QtQuick/Shapes")

QML_ROOT_MODULES = ("QtQml", "QtQuick")

PLUGIN_FILES = (
    "platforms/libqxcb.so",
    "platforms/libqoffscreen.so",
    "platforms/libqminimal.so",
    "imageformats/libqgif.so",
    "imageformats/libqico.so",
    "imageformats/libqjpeg.so",
    "imageformats/libqsvg.so",
    "iconengines/libqsvgicon.so",
)

WAYLAND_PLUGIN_FILES = (
    "platforms/libqwayland.so",
    "platforms/libqwayland-generic.so",
    "platforms/libqwayland-egl.so",
)

PLUGIN_DIRECTORIES = (
    "networkinformation",
    "platforminputcontexts",
    "tls",
    "wayland-decoration-client",
    "wayland-graphics-integration-client",
    "wayland-shell-integration",
)


class PackagingError(RuntimeError):
    pass


COPIED_SOURCES: dict[pathlib.Path, pathlib.Path] = {}
RPM_LICENSE_CACHE: dict[str, list[pathlib.Path]] = {}
DPKG_LICENSE_CACHE: dict[str, list[pathlib.Path]] = {}
DPKG_SOURCE_CACHE: dict[str, tuple[str, str] | None] = {}
PACKAGE_LICENSE_CACHE: dict[str, "PackageLicenseSource" | None] = {}
PATH_PACKAGE_IDENTITY_CACHE: dict[tuple[str, pathlib.Path, str], tuple[str, str]] = {}
QT_LICENSE_CACHE: dict[str, list[pathlib.Path]] = {}

QT_LICENSE_MODULE_PREFIXES = (
    (
        "qtvirtualkeyboard",
        (
            "lib/libQt6VirtualKeyboard",
            "plugins/platforminputcontexts/libqtvirtualkeyboard",
            "qml/QtQuick/VirtualKeyboard",
        ),
    ),
    (
        "qtwayland",
        (
            "lib/libQt6Wayland",
            "lib/libQt6WlShellIntegration",
            "plugins/platforms/libqwayland",
            "plugins/wayland-",
        ),
    ),
    (
        "qtsvg",
        (
            "lib/libQt6Svg",
            "plugins/imageformats/libqsvg",
            "plugins/iconengines/libqsvgicon",
        ),
    ),
    (
        "qtdeclarative",
        (
            "lib/libQt6Qml",
            "lib/libQt6Quick",
            "qml/QtQml",
            "qml/QtCore",
            "qml/QtQuick",
        ),
    ),
    (
        "qtbase",
        (
            "lib/libQt6Core",
            "lib/libQt6DBus",
            "lib/libQt6Gui",
            "lib/libicu",
            "lib/libQt6Network",
            "lib/libQt6OpenGL",
            "lib/libQt6XcbQpa",
            "plugins/iconengines/libqsvgicon",
            "plugins/imageformats/libqgif",
            "plugins/imageformats/libqico",
            "plugins/imageformats/libqjpeg",
            "plugins/imageformats/libqsvg",
            "plugins/networkinformation/",
            "plugins/platforminputcontexts/",
            "plugins/platforms/libqminimal",
            "plugins/platforms/libqoffscreen",
            "plugins/platforms/libqxcb",
            "plugins/tls/",
        ),
    ),
)


class PackageLicenseSource(NamedTuple):
    manager: str
    package: str
    version: str
    library: pathlib.Path
    licenses: tuple[pathlib.Path, ...]


def run(command: list[str], *, capture: bool = False) -> str:
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip()
        raise PackagingError(f"command failed ({result.returncode}): {command[0]}: {detail}")
    return result.stdout if capture else ""


def copy_regular(source: pathlib.Path, destination: pathlib.Path) -> None:
    if not source.exists():
        raise PackagingError(f"missing packaging input: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    discovered = source.absolute()
    resolved = source.resolve()
    # A dependency can be discovered from a library that was already copied
    # into the staging tree. Keep the original host path in that case so
    # package ownership and license lookup use the real source file.
    origin = COPIED_SOURCES.get(resolved, discovered)
    COPIED_SOURCES[destination.resolve()] = origin
    if destination.exists():
        if hashlib.sha256(destination.read_bytes()).digest() != hashlib.sha256(resolved.read_bytes()).digest():
            raise PackagingError(f"dependency name collision: {destination.name}")
        return
    shutil.copyfile(resolved, destination)


def copy_tree(source: pathlib.Path, destination: pathlib.Path) -> None:
    if not source.is_dir():
        raise PackagingError(f"missing packaging directory: {source}")
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        target = destination / relative
        if path.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif path.is_file() or path.is_symlink():
            copy_regular(path, target)
        else:
            raise PackagingError(f"unsupported packaging input: {path}")


def copy_tree_if_present(source: pathlib.Path, destination: pathlib.Path) -> None:
    if source.is_dir():
        copy_tree(source, destination)


def copy_module_root(source: pathlib.Path, destination: pathlib.Path) -> None:
    if not source.is_dir():
        raise PackagingError(f"missing packaging directory: {source}")
    destination.mkdir(parents=True, exist_ok=True)
    for path in sorted(source.iterdir()):
        if path.is_file() or path.is_symlink():
            copy_regular(path, destination / path.name)


def qt_path(name: str) -> pathlib.Path:
    output = run(["qtpaths6", "--query", name], capture=True).strip()
    if not output:
        raise PackagingError(f"Qt path is empty: {name}")
    return pathlib.Path(output)


def is_elf(path: pathlib.Path) -> bool:
    if not path.is_file():
        return False
    with path.open("rb") as handle:
        return handle.read(4) == b"\x7fELF"


def elf_soname(path: pathlib.Path) -> str | None:
    output = run(["readelf", "--dynamic", "--wide", str(path)], capture=True)
    match = re.search(r"\(SONAME\).*?\[([^]]+)\]", output)
    return match.group(1) if match is not None else None


def dependencies(path: pathlib.Path) -> list[tuple[str, pathlib.Path]]:
    output = run(["ldd", str(path)], capture=True)
    result: list[tuple[str, pathlib.Path]] = []
    for line in output.splitlines():
        stripped = line.strip()
        if "=> not found" in stripped:
            raise PackagingError(f"unresolved dependency for {path.name}: {stripped.split()[0]}")
        match = re.match(r"([^\s]+)\s+=>\s+(/[^\s]+)\s+\(", stripped)
        if match:
            result.append((match.group(1), pathlib.Path(match.group(2))))
            continue
        match = re.match(r"(/[^\s]+)\s+\(", stripped)
        if match:
            library = pathlib.Path(match.group(1))
            result.append((library.name, library))
    return result


def is_excluded_library(name: str) -> bool:
    return name == "linux-vdso.so.1" or name in EXCLUDED_LIBRARIES


def collect_libraries(prefix: pathlib.Path) -> None:
    library_root = prefix / "lib"
    library_root.mkdir(parents=True, exist_ok=True)
    pending = [path for path in sorted(prefix.rglob("*")) if is_elf(path)]
    inspected: set[pathlib.Path] = set()
    while pending:
        path = pending.pop(0)
        resolved = path.resolve()
        if resolved in inspected:
            continue
        inspected.add(resolved)
        for name, source in dependencies(path):
            if is_excluded_library(name):
                continue
            destination = library_root / name
            existed = destination.exists()
            copy_regular(source, destination)
            if not existed:
                pending.append(destination)


def patch_elfs(prefix: pathlib.Path) -> None:
    library_root = prefix / "lib"
    for path in sorted(prefix.rglob("*")):
        if not is_elf(path):
            continue
        run(["strip", "--strip-unneeded", str(path)])
        run(["objcopy", "--remove-section", ".note.gnu.build-id", str(path)])
        if path == prefix / "bin/perigee":
            relative_library = "../lib"
        else:
            relative_library = os.path.relpath(library_root, path.parent)
        run(
            [
                "patchelf",
                "--force-rpath",
                "--set-rpath",
                f"$ORIGIN/{relative_library}",
                str(path),
            ]
        )


def write_text(path: pathlib.Path, text: str, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    path.chmod(mode)


def stage_metadata(source_root: pathlib.Path, root: pathlib.Path, prefix: pathlib.Path) -> None:
    desktop_name = "app.perigee_stream.Perigee.desktop"
    appstream_name = "app.perigee_stream.Perigee.appdata.xml"
    icon_name = "app.perigee_stream.Perigee.svg"
    copy_regular(
        source_root / "app/deploy/linux" / desktop_name,
        prefix / "share/applications" / desktop_name,
    )
    copy_regular(
        source_root / "app/deploy/linux" / appstream_name,
        prefix / "share/metainfo" / appstream_name,
    )
    copy_regular(
        source_root / "app/res/perigee.svg",
        prefix / "share/icons/hicolor/scalable/apps" / icon_name,
    )
    copy_regular(source_root / "LICENSE", root / "LICENSE")
    copy_regular(source_root / "NOTICE.md", root / "NOTICE.md")


def stage_qt(prefix: pathlib.Path) -> None:
    qml_root = qt_path("QT_INSTALL_QML")
    plugin_root = qt_path("QT_INSTALL_PLUGINS")
    translations_root = qt_path("QT_INSTALL_TRANSLATIONS")
    for module in QML_ROOT_MODULES:
        copy_module_root(qml_root / module, prefix / "qml" / module)
    for module in QML_MODULES:
        copy_tree(qml_root / module, prefix / "qml" / module)
    for module in OPTIONAL_QML_MODULES:
        copy_tree_if_present(qml_root / module, prefix / "qml" / module)
    wayland_plugins = 0
    for relative in WAYLAND_PLUGIN_FILES:
        source = plugin_root / relative
        if source.is_file():
            copy_regular(source, prefix / "plugins" / relative)
            wayland_plugins += 1
    if wayland_plugins == 0:
        raise PackagingError("Qt Wayland platform plugin is missing")
    for relative in PLUGIN_FILES:
        copy_regular(plugin_root / relative, prefix / "plugins" / relative)
    for relative in PLUGIN_DIRECTORIES:
        copy_tree(plugin_root / relative, prefix / "plugins" / relative)
    if translations_root.is_dir():
        for translation in sorted(translations_root.glob("qt_*.qm")):
            copy_regular(translation, prefix / "translations" / translation.name)


def safe_component_name(value: str) -> str:
    result = re.sub(r"[^A-Za-z0-9.+_-]", "-", value).strip("-")
    if not result:
        raise PackagingError("empty license component name")
    return result


def component_directory(license_root: pathlib.Path, component: str) -> pathlib.Path:
    parts = component.split("/")
    if not parts or any(safe_component_name(part) != part for part in parts):
        raise PackagingError(f"invalid license component name: {component}")
    return license_root.joinpath(*parts)


def rpm_direct_license_files(package: str) -> list[pathlib.Path]:
    listing = subprocess.run(
        ["rpm", "-ql", package],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if listing.returncode != 0:
        return []
    return sorted(
        {
            pathlib.Path(line)
            for line in listing.stdout.splitlines()
            if pathlib.Path(line).is_file()
            and (
                "/share/licenses/" in line
                or pathlib.Path(line).name.upper().startswith(
                    ("LICENSE", "COPYING", "COPYRIGHT", "NOTICE")
                )
            )
        }
    )


def rpm_license_files(package: str) -> list[pathlib.Path]:
    if package in RPM_LICENSE_CACHE:
        return RPM_LICENSE_CACHE[package]
    if subprocess.run(
        ["rpm", "-q", package],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    ).returncode != 0:
        return []
    licenses = rpm_direct_license_files(package)
    if not licenses:
        source_package = subprocess.run(
            ["rpm", "-q", "--qf", "%{SOURCERPM}", package],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
        installed_packages = subprocess.run(
            ["rpm", "-qa", "--qf", "%{NAME}.%{ARCH}\t%{SOURCERPM}\n"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
        if source_package.returncode == 0 and installed_packages.returncode == 0:
            expected_source = source_package.stdout.strip()
            for line in installed_packages.stdout.splitlines():
                fields = line.split("\t", 1)
                if (
                    len(fields) == 2
                    and fields[1] == expected_source
                    and fields[0] != package
                ):
                    licenses.extend(rpm_direct_license_files(fields[0]))
    result = sorted(set(licenses))
    RPM_LICENSE_CACHE[package] = result
    return result


def dpkg_direct_license_files(package: str) -> list[pathlib.Path]:
    listing = subprocess.run(
        ["dpkg-query", "-L", package],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if listing.returncode != 0:
        return []
    return sorted(
        {
            pathlib.Path(line)
            for line in listing.stdout.splitlines()
            if pathlib.Path(line).is_file()
            and (line.endswith("/copyright") or "/licenses/" in line)
        }
    )


def dpkg_source_identity(package: str) -> tuple[str, str] | None:
    if package in DPKG_SOURCE_CACHE:
        return DPKG_SOURCE_CACHE[package]
    result = subprocess.run(
        ["dpkg-query", "-W", "-f=${source:Package}\t${source:Version}", package],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )
    fields = result.stdout.strip().split("\t")
    identity = (
        (fields[0], fields[1])
        if result.returncode == 0 and len(fields) == 2 and all(fields)
        else None
    )
    DPKG_SOURCE_CACHE[package] = identity
    return identity


def dpkg_license_files(package: str) -> list[pathlib.Path]:
    if package in DPKG_LICENSE_CACHE:
        return DPKG_LICENSE_CACHE[package]
    licenses = dpkg_direct_license_files(package)
    if not licenses:
        expected_source = dpkg_source_identity(package)
        installed = subprocess.run(
            [
                "dpkg-query",
                "-W",
                "-f=${binary:Package}\t${source:Package}\t${source:Version}\n",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
        if expected_source is not None and installed.returncode == 0:
            candidates: list[str] = []
            for line in installed.stdout.splitlines():
                fields = line.split("\t")
                if (
                    len(fields) == 3
                    and (fields[1], fields[2]) == expected_source
                    and fields[0] != package
                ):
                    candidates.append(fields[0])
            for candidate in sorted(set(candidates)):
                licenses.extend(dpkg_direct_license_files(candidate))
    result = sorted(set(licenses))
    DPKG_LICENSE_CACHE[package] = result
    return result


def package_license_files(source: pathlib.Path) -> PackageLicenseSource | None:
    # Ubuntu's merged-/usr layout exposes libraries through both /lib and
    # /usr/lib, but the dpkg database contains a mixture of both spellings.
    # Keep the discovered spelling for a fallback query while preferring the
    # resolved spelling when both paths are package-owned.
    discovered_source = source.absolute()
    source = source.resolve()
    if shutil.which("rpm") is not None:
        owner = subprocess.run(
            ["rpm", "-qf", "--qf", "%{NAME}.%{ARCH}\n", str(source)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
        owner_candidates = sorted(
            {
                line
                for line in owner.stdout.splitlines()
                if line.endswith(".x86_64")
            }
        )
        if owner.returncode == 0 and len(owner_candidates) == 1:
            package = owner_candidates[0]
            if package in PACKAGE_LICENSE_CACHE:
                cached = PACKAGE_LICENSE_CACHE[package]
                return cached._replace(library=source) if cached is not None else None
            licenses = rpm_license_files(package)
            if licenses:
                version = subprocess.run(
                    ["rpm", "-q", "--qf", "%{EVR}", package],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    text=True,
                    check=False,
                )
                if version.returncode != 0 or not version.stdout.strip():
                    raise PackagingError(f"cannot identify package version: {package}")
                result = PackageLicenseSource(
                    "rpm",
                    package,
                    version.stdout.strip(),
                    source,
                    tuple(licenses),
                )
                PACKAGE_LICENSE_CACHE[package] = result
                return result
            PACKAGE_LICENSE_CACHE[package] = None

    if shutil.which("dpkg-query") is not None:
        query_paths = [source]
        if discovered_source != source:
            query_paths.append(discovered_source)
        candidates: list[PackageLicenseSource] = []
        reported_packages: set[str] = set()
        for query_path in query_paths:
            owner = subprocess.run(
                ["dpkg-query", "-S", str(query_path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                check=False,
            )
            if owner.returncode != 0 or not owner.stdout.strip():
                continue
            owner_lines = [
                line.rsplit(": ", 1)
                for line in owner.stdout.splitlines()
                if line.endswith(f": {query_path}")
            ]
            if len(owner_lines) != 1 or len(owner_lines[0]) != 2:
                raise PackagingError(f"cannot identify unique package owner: {query_path}")
            package = owner_lines[0][0]
            reported_packages.add(package)
            if len(reported_packages) > 1:
                raise PackagingError(f"conflicting package owners: {source}")
            listing = subprocess.run(
                ["dpkg-query", "-L", package],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
            if listing.returncode != 0:
                raise PackagingError(f"cannot list license files for package: {package}")
            listed_paths = set(listing.stdout.splitlines())
            if query_path.as_posix() not in listed_paths:
                continue
            licenses = dpkg_license_files(package)
            if not licenses:
                raise PackagingError(f"package has no standalone license files: {package}")
            version = subprocess.run(
                ["dpkg-query", "-W", "-f=${Version}", package],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
            if version.returncode != 0 or not version.stdout.strip():
                raise PackagingError(f"cannot identify package version: {package}")
            candidates.append(
                PackageLicenseSource(
                    "dpkg",
                    package,
                    version.stdout.strip(),
                    query_path,
                    tuple(sorted(set(licenses))),
                )
            )
        if candidates:
            for candidate in candidates:
                if candidate.library == source:
                    return candidate
            return candidates[0]
    return None


def package_identity_for_path(
    path: pathlib.Path,
    manager: str,
    preferred_package: str,
) -> tuple[str, str]:
    cache_key = (manager, path, preferred_package)
    if cache_key in PATH_PACKAGE_IDENTITY_CACHE:
        return PATH_PACKAGE_IDENTITY_CACHE[cache_key]
    if manager == "rpm":
        owner = subprocess.run(
            ["rpm", "-qf", "--qf", "%{NAME}.%{ARCH}\n", str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
        candidates = sorted(
            {
                line
                for line in owner.stdout.splitlines()
                if line.endswith((".x86_64", ".noarch"))
            }
        )
        preferred = [preferred_package] if preferred_package in candidates else []
        if not preferred:
            preferred = [line for line in candidates if line.endswith(".x86_64")]
        if not preferred:
            preferred = [line for line in candidates if line.endswith(".noarch")]
        if owner.returncode != 0 or not preferred:
            raise PackagingError(f"cannot identify license package owner: {path}")
        package = sorted(preferred)[0]
        version = subprocess.run(
            ["rpm", "-q", "--qf", "%{EVR}", package],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
    elif manager == "dpkg":
        owner = subprocess.run(
            ["dpkg-query", "-S", str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
        matches = [
            fields[0]
            for line in owner.stdout.splitlines()
            if len(fields := line.rsplit(": ", 1)) == 2
            and fields[1] == path.as_posix()
        ]
        if owner.returncode != 0 or len(set(matches)) != 1:
            raise PackagingError(f"cannot identify license package owner: {path}")
        package = matches[0]
        version = subprocess.run(
            ["dpkg-query", "-W", "-f=${Version}", package],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
    else:
        raise PackagingError(f"unsupported package manager: {manager}")
    if version.returncode != 0 or not version.stdout.strip():
        raise PackagingError(f"cannot identify license package version: {package}")
    result = (package, version.stdout.strip())
    PATH_PACKAGE_IDENTITY_CACHE[cache_key] = result
    return result


def qt_module_for_source(source: pathlib.Path, qt_prefix: pathlib.Path) -> str | None:
    try:
        relative = source.resolve().relative_to(qt_prefix.resolve()).as_posix()
    except ValueError:
        return None
    for module, prefixes in QT_LICENSE_MODULE_PREFIXES:
        if any(relative.startswith(prefix) for prefix in prefixes):
            return module
    return None


def qt_license_files(source: pathlib.Path) -> list[pathlib.Path]:
    """Return license texts shipped with a non-system Qt installation."""
    global QT_LICENSE_CACHE
    qt_prefix = qt_path("QT_INSTALL_PREFIX").resolve()
    if qt_prefix == pathlib.Path("/usr"):
        return []
    module = qt_module_for_source(source, qt_prefix)
    if module is None:
        return []
    if module in QT_LICENSE_CACHE:
        return QT_LICENSE_CACHE[module]

    roots: list[pathlib.Path] = []
    base = qt_prefix
    for _ in range(4):
        for name in ("LICENSES", "Licenses", "licenses"):
            candidate = base / name
            if candidate.is_dir() and candidate not in roots:
                roots.append(candidate)
        if base.parent == base:
            break
        base = base.parent

    files: list[pathlib.Path] = []
    for root in roots:
        module_root = root / module
        if module_root.is_dir():
            files.extend(path for path in sorted(module_root.rglob("*")) if path.is_file())
    QT_LICENSE_CACHE[module] = sorted(set(files))
    return QT_LICENSE_CACHE[module]


def source_build_component(library_name: str) -> str | None:
    prefixes = (
        ("libSDL2_ttf", "sdl-ttf"),
        ("libSDL2", "sdl2-compat"),
        ("libSDL3", "sdl3"),
        ("libva", "libva"),
        ("libplacebo", "libplacebo"),
        ("libdav1d", "dav1d"),
        ("libavcodec", "ffmpeg"),
        ("libavformat", "ffmpeg"),
        ("libavutil", "ffmpeg"),
        ("libswscale", "ffmpeg"),
        ("libswresample", "ffmpeg"),
    )
    for prefix, component in prefixes:
        if library_name.startswith(prefix):
            return component
    return None


def existing_source_licenses(source_root: pathlib.Path, component: str) -> list[pathlib.Path]:
    candidates = SOURCE_BUILD_LICENSES[component]
    result = [source_root / relative for relative in candidates if (source_root / relative).is_file()]
    if not result:
        raise PackagingError(f"missing source-build license text: {component}")
    return result


def copy_component_licenses(
    license_root: pathlib.Path,
    component: str,
    sources: list[pathlib.Path],
) -> list[pathlib.Path]:
    component_root = component_directory(license_root, component)
    result: list[pathlib.Path] = []
    for source in sources:
        if not source.is_file():
            raise PackagingError(f"missing license text: {source}")
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        destination = component_root / source.name
        if destination.exists() and hashlib.sha256(destination.read_bytes()).hexdigest() != digest:
            destination = component_root / f"{source.name}.{digest[:12]}"
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            shutil.copyfile(source, destination)
        result.append(destination)
    return result


def stage_licenses(source_root: pathlib.Path, root: pathlib.Path, prefix: pathlib.Path) -> None:
    license_root = prefix / "share/licenses/perigee"
    component_files: dict[str, list[pathlib.Path]] = {}

    component_files["source/perigee"] = copy_component_licenses(
        license_root, "source/perigee", [source_root / "LICENSE", source_root / "NOTICE.md"]
    )
    for component, relative_paths in SOURCE_COMPONENT_LICENSES.items():
        sources = [source_root / relative for relative in relative_paths]
        component_files[f"source/{component}"] = copy_component_licenses(
            license_root, f"source/{component}", sources
        )
    owner_cache: dict[pathlib.Path, list[pathlib.Path]] = {}
    package_sources: dict[pathlib.Path, PackageLicenseSource | None] = {}
    system_provenance: set[str] = set()
    mappings: list[tuple[str, str]] = []
    unresolved: list[str] = []
    for elf in sorted(path for path in prefix.rglob("*") if is_elf(path)):
        elf_relative = elf.relative_to(root).as_posix()
        prefix_relative = elf.relative_to(prefix).as_posix()
        if prefix_relative == "bin/perigee":
            mappings.extend((elf_relative, component) for component in STATIC_SOURCE_COMPONENTS)
            continue

        component = elf_license_component(prefix_relative, elf_soname(elf))
        origin = COPIED_SOURCES.get(elf.resolve())
        if origin is None:
            unresolved.append(elf_relative)
            continue
        if origin not in owner_cache:
            package = package_license_files(origin)
            if package is not None:
                sources = list(package.licenses)
            else:
                source_component = source_build_component(origin.name)
                if source_component is not None:
                    sources = existing_source_licenses(source_root, source_component)
                else:
                    sources = qt_license_files(origin)
            if not sources:
                unresolved.append(f"{elf_relative} (source={origin})")
                continue
            owner_cache[origin] = sources
            package_sources[origin] = package
        sources = owner_cache[origin]
        component_files.setdefault(component, [])
        destinations = copy_component_licenses(license_root, component, sources)
        component_files[component].extend(destinations)
        package = package_sources[origin]
        if package is not None and package.manager in {"dpkg", "rpm"}:
            fields = (
                component,
                package.manager,
                package.package,
                package.version,
                package.library.as_posix(),
            )
            if any("\t" in field or "\n" in field or "\r" in field for field in fields):
                raise PackagingError(f"invalid system provenance field: {component}")
            for source, destination in zip(sources, destinations, strict=True):
                packaged = destination.relative_to(license_root).as_posix()
                digest = hashlib.sha256(source.read_bytes()).hexdigest()
                license_package, license_version = package_identity_for_path(
                    source, package.manager, package.package
                )
                row = (
                    *fields,
                    license_package,
                    license_version,
                    source.as_posix(),
                    packaged,
                    digest,
                )
                if any("\t" in field or "\n" in field or "\r" in field for field in row):
                    raise PackagingError(f"invalid system provenance path: {component}")
                system_provenance.add("\t".join(row))
        mappings.append((elf_relative, component))

    if unresolved:
        raise PackagingError(
            "cannot identify license owners for ELFs: " + ", ".join(sorted(unresolved))
        )

    license_files = sorted(
        path
        for path in license_root.rglob("*")
        if path.is_file()
        and path.name not in {"SHA256SUMS", "licenses.tsv", "system-provenance.tsv"}
    )
    if not mappings or not license_files:
        raise PackagingError("license manifest would be empty")
    checksum_text = "".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(license_root).as_posix()}\n"
        for path in license_files
    )
    mapping_text = "".join(f"{path}\t{component}\n" for path, component in mappings)
    write_text(license_root / "SHA256SUMS", checksum_text)
    write_text(license_root / "licenses.tsv", mapping_text)
    write_text(
        license_root / "system-provenance.tsv",
        "".join(f"{row}\n" for row in sorted(system_provenance)),
    )


def normalize(root: pathlib.Path, epoch: int) -> None:
    executable_paths = {root / "AppRun"}
    for path in sorted(root.rglob("*"), reverse=True):
        if path.is_symlink():
            raise PackagingError(f"staged payload contains a symlink: {path.relative_to(root)}")
        if path.is_dir():
            path.chmod(0o755)
        elif path.is_file():
            mode = 0o755 if path in executable_paths or path.name == "perigee" else 0o644
            path.chmod(mode)
        else:
            raise PackagingError(f"staged payload has unsupported entry: {path.relative_to(root)}")
        os.utime(path, (epoch, epoch), follow_symlinks=False)
    root.chmod(0o755)
    os.utime(root, (epoch, epoch))


def stage(args: argparse.Namespace) -> None:
    global QT_LICENSE_CACHE
    COPIED_SOURCES.clear()
    RPM_LICENSE_CACHE.clear()
    DPKG_LICENSE_CACHE.clear()
    DPKG_SOURCE_CACHE.clear()
    PACKAGE_LICENSE_CACHE.clear()
    PATH_PACKAGE_IDENTITY_CACHE.clear()
    QT_LICENSE_CACHE = {}
    source_root = args.source_root.resolve()
    root = args.destination.resolve()
    prefix = root if args.layout == "tar" else root / "usr"
    binary = prefix / "bin/perigee"
    copy_regular(args.binary, binary)
    stage_metadata(source_root, root, prefix)
    stage_qt(prefix)
    write_text(
        prefix / "bin/qt.conf",
        "[Paths]\nPrefix=..\nLibraries=lib\nPlugins=plugins\nQmlImports=qml\nTranslations=translations\n",
    )
    if args.layout == "appimage":
        write_text(root / "AppRun", APP_RUN_TEXT, 0o755)
        desktop = root / "usr/share/applications/app.perigee_stream.Perigee.desktop"
        icon = root / "usr/share/icons/hicolor/scalable/apps/app.perigee_stream.Perigee.svg"
        copy_regular(desktop, root / "app.perigee_stream.Perigee.desktop")
        copy_regular(icon, root / "app.perigee_stream.Perigee.svg")
        copy_regular(icon, root / ".DirIcon")
    collect_libraries(prefix)
    stage_licenses(source_root, root, prefix)
    patch_elfs(prefix)
    normalize(root, args.epoch)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument("--source-root", type=pathlib.Path, required=True)
    result.add_argument("--binary", type=pathlib.Path, required=True)
    result.add_argument("--destination", type=pathlib.Path, required=True)
    result.add_argument("--layout", choices=("tar", "appimage"), required=True)
    result.add_argument("--epoch", type=int, required=True)
    return result


def main() -> int:
    try:
        stage(parser().parse_args())
    except PackagingError as error:
        print(f"Perigee packaging failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

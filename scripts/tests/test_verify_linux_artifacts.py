#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import hashlib
import io
import os
import pathlib
import shutil
import signal
import subprocess
import tarfile
import tempfile
import time
import unittest
import sys
from unittest import mock


SCRIPT_ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = SCRIPT_ROOT / "lib/verify_linux_artifacts.py"
SPEC = importlib.util.spec_from_file_location("verify_linux_artifacts", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
VERIFY = importlib.util.module_from_spec(SPEC)
sys.path.insert(0, str(MODULE_PATH.parent))
try:
    SPEC.loader.exec_module(VERIFY)
finally:
    sys.path.pop(0)

STAGE_MODULE_PATH = SCRIPT_ROOT / "lib/stage_linux_payload.py"
STAGE_SPEC = importlib.util.spec_from_file_location("stage_linux_payload", STAGE_MODULE_PATH)
assert STAGE_SPEC is not None and STAGE_SPEC.loader is not None
STAGE = importlib.util.module_from_spec(STAGE_SPEC)
sys.path.insert(0, str(STAGE_MODULE_PATH.parent))
try:
    STAGE_SPEC.loader.exec_module(STAGE)
finally:
    sys.path.pop(0)

FIXTURE_DEPENDENCY_LICENSE = (
    b"Fixture dependency license\n\n"
    b"Copyright 2026 Fixture Authors\n"
    b"Permission is hereby granted to use, copy, and distribute this software.\n"
)


def write(path: pathlib.Path, content: bytes, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    path.chmod(mode)


def normalize_fixture_elf(path: pathlib.Path, rpath: str) -> None:
    subprocess.run(
        ["objcopy", "--remove-section", ".note.gnu.build-id", str(path)],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        ["patchelf", "--force-rpath", "--set-rpath", rpath, str(path)],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def valid_tree(
    root: pathlib.Path,
    kind: str,
    *,
    excluded_xcb_gl_plugin: str | None = None,
) -> pathlib.Path:
    prefix = pathlib.Path() if kind == "tar" else pathlib.Path("usr")
    binary = root / prefix / "bin/perigee"
    binary.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2("/usr/bin/true", binary)
    binary.chmod(0o755)
    normalize_fixture_elf(binary, "$ORIGIN/../lib")
    private_library = root / prefix / "lib/libfixture.so.1"
    private_library.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2("/usr/bin/true", private_library)
    private_library.chmod(0o644)
    normalize_fixture_elf(private_library, "$ORIGIN")
    plugin = root / prefix / "plugins/platforms/libqwayland-generic.so"
    plugin.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2("/usr/bin/true", plugin)
    plugin.chmod(0o644)
    normalize_fixture_elf(plugin, "$ORIGIN/../../lib")
    xcb_gl_plugins = tuple(
        root / prefix / "plugins/xcbglintegrations" / plugin_name
        for plugin_name in (
            "libqxcb-egl-integration.so",
            "libqxcb-glx-integration.so",
        )
        if plugin_name != excluded_xcb_gl_plugin
    )
    for xcb_gl_plugin in xcb_gl_plugins:
        xcb_gl_plugin.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2("/usr/bin/true", xcb_gl_plugin)
        xcb_gl_plugin.chmod(0o644)
        normalize_fixture_elf(xcb_gl_plugin, "$ORIGIN/../../lib")
    for module in (
        "QML",
        "QtCore",
        "QtQml",
        "QtQuick",
        "QtQml/Models",
        "QtQml/WorkerScript",
        "QtQuick/Controls",
        "QtQuick/Controls/Material",
        "QtQuick/Layouts",
        "QtQuick/Templates",
        "QtQuick/Window",
    ):
        module_root = root / prefix / "qml" / module
        write(
            module_root / "qmldir",
            (
                f"module {module.replace('/', '.')}\n"
                "FixtureType 1.0 FixtureType.qml\n"
            ).encode(),
        )
        write(module_root / "FixtureType.qml", b"import QtQml\nQtObject {}\n")

    source_root = SCRIPT_ROOT.parent
    desktop_bytes = (source_root / "app/deploy/linux/app.perigee_stream.Perigee.desktop").read_bytes()
    appstream_bytes = (source_root / "app/deploy/linux/app.perigee_stream.Perigee.appdata.xml").read_bytes()
    icon_bytes = (source_root / "app/res/perigee.svg").read_bytes()
    write(root / prefix / "share/applications/app.perigee_stream.Perigee.desktop", desktop_bytes)
    write(root / prefix / "share/metainfo/app.perigee_stream.Perigee.appdata.xml", appstream_bytes)
    write(root / prefix / "share/icons/hicolor/scalable/apps/app.perigee_stream.Perigee.svg", icon_bytes)
    write(root / "LICENSE", (source_root / "LICENSE").read_bytes())
    write(root / "NOTICE.md", (source_root / "NOTICE.md").read_bytes())

    license_root = root / prefix / "share/licenses/perigee"
    for packaged, source in VERIFY.SOURCE_LICENSE_IDENTITIES.items():
        write(license_root / packaged, (source_root / source).read_bytes())
    elf_components: dict[pathlib.Path, str] = {}
    for elf in (private_library, plugin, *xcb_gl_plugins):
        prefix_relative = elf.relative_to(root / prefix).as_posix()
        component = VERIFY.elf_license_component(prefix_relative, VERIFY.elf_soname(elf))
        elf_components[elf] = component
        write(
            license_root / component / "LICENSE.txt",
            FIXTURE_DEPENDENCY_LICENSE,
        )
    license_files = sorted(path for path in license_root.rglob("*") if path.is_file())
    checksums = "".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(license_root).as_posix()}\n"
        for path in license_files
    )
    write(license_root / "SHA256SUMS", checksums.encode())
    mappings = "".join(
        f"{binary.relative_to(root).as_posix()}\t{component}\n"
        for component in VERIFY.STATIC_SOURCE_COMPONENTS
    )
    mappings += "".join(
        f"{path.relative_to(root).as_posix()}\t{elf_components[path]}\n"
        for path in sorted(elf_components)
    )
    write(license_root / "licenses.tsv", mappings.encode())
    write(license_root / "system-provenance.tsv", b"")
    if kind == "appimage":
        write(root / "AppRun", VERIFY.APP_RUN_TEXT.encode("utf-8"), 0o755)
        write(root / "app.perigee_stream.Perigee.desktop", desktop_bytes)
        write(root / "app.perigee_stream.Perigee.svg", icon_bytes)
        write(root / ".DirIcon", icon_bytes)
    return binary


def refresh_license_checksum(root: pathlib.Path, kind: str, relative: str) -> None:
    prefix = pathlib.Path() if kind == "tar" else pathlib.Path("usr")
    license_root = root / prefix / "share/licenses/perigee"
    manifest = license_root / "SHA256SUMS"
    digest = hashlib.sha256((license_root / relative).read_bytes()).hexdigest()
    lines = manifest.read_text(encoding="utf-8").splitlines()
    lines = [
        f"{digest}  {relative}" if line.endswith(f"  {relative}") else line
        for line in lines
    ]
    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")


def system_provenance_fixture(root: pathlib.Path, manager: str) -> dict[str, object]:
    payload_root = root / "payload"
    payload_elf = payload_root / "lib/libfixture.so.1"
    provider = root / "host/usr/lib/libfixture.so.1"
    for path in (payload_elf, provider):
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2("/usr/bin/true", path)
        subprocess.run(["patchelf", "--set-soname", "libfixture.so.1", str(path)], check=True)
    component = "elf/soname-libfixture.so.1"
    license_source = root / "host/usr/share/doc/libfixture/copyright"
    content = (
        b"Copyright 2026 Fixture Authors\n"
        b"Permission is granted to use, copy, modify, and distribute this software.\n"
        b"The software is provided without warranty or liability.\n"
    )
    write(license_source, content)
    license_root = payload_root / "share/licenses/perigee"
    packaged = f"{component}/copyright"
    packaged_path = license_root / packaged
    write(packaged_path, content)
    digest = hashlib.sha256(content).hexdigest()
    if manager == "dpkg":
        package = "libfixture:amd64"
        version = "1.0-1"

        def query(arguments: list[str]) -> str:
            if arguments[:1] == ["-S"]:
                return f"{package}: {arguments[1]}\n"
            if arguments[:2] == ["-W", "-f=${Version}"]:
                return version
            if arguments[:1] == ["-L"]:
                return f"{provider}\n{license_source}\n"
            raise AssertionError(arguments)

        patcher = mock.patch.object(VERIFY, "run_dpkg_query", side_effect=query)
    else:
        package = "libfixture.x86_64"
        version = "1.0-1"

        def query(arguments: list[str]) -> str:
            if arguments[:3] == ["-qf", "--qf", "%{NAME}.%{ARCH}\n"]:
                return f"{package}\n"
            if arguments[:3] == ["-q", "--qf", "%{EVR}"]:
                return version
            if arguments[:1] == ["-ql"]:
                return f"{provider}\n{license_source}\n"
            raise AssertionError(arguments)

        patcher = mock.patch.object(VERIFY, "run_rpm_query", side_effect=query)
    row = VERIFY.SystemProvenanceRow(
        component,
        manager,
        package,
        version,
        provider,
        package,
        version,
        license_source,
        packaged,
        digest,
    )
    return {
        "payload_root": payload_root,
        "payload_elf": payload_elf,
        "provider": provider,
        "component": component,
        "license_root": license_root,
        "license_source": license_source,
        "packaged_path": packaged_path,
        "row": row,
        "query_patch": patcher,
    }


def tar_info(name: str, kind: str = "file", linkname: str = "", mode: int = 0o644) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.mode = mode
    info.mtime = 1
    if kind == "symlink":
        info.type = tarfile.SYMTYPE
        info.linkname = linkname
    elif kind == "directory":
        info.type = tarfile.DIRTYPE
    return info


def make_tar_zst(path: pathlib.Path, members: list[tuple[tarfile.TarInfo, bytes]]) -> None:
    raw = path.with_suffix("")
    with tarfile.open(raw, "w", format=tarfile.PAX_FORMAT) as archive:
        for info, content in members:
            if info.isfile():
                info.size = len(content)
                archive.addfile(info, io.BytesIO(content))
            else:
                archive.addfile(info)
    with path.open("wb") as output:
        result = subprocess.run(
            ["zstd", "-q", "-c", "--", str(raw)],
            stdout=output,
            stderr=subprocess.PIPE,
            check=False,
        )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.decode("utf-8", errors="replace"))


class VerifyLinuxArtifactsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="perigee-verifier-test-")
        self.root = pathlib.Path(self.temporary.name)
        self.reviewed_license_digests = VERIFY.REVIEWED_LICENSE_DIGESTS
        fixture_digest = hashlib.sha256(FIXTURE_DEPENDENCY_LICENSE).hexdigest()
        VERIFY.REVIEWED_LICENSE_DIGESTS = dict(VERIFY.REVIEWED_LICENSE_DIGESTS)
        for relative in (
            "lib/libfixture.so.1",
            "plugins/platforms/libqwayland-generic.so",
            "plugins/xcbglintegrations/libqxcb-egl-integration.so",
            "plugins/xcbglintegrations/libqxcb-glx-integration.so",
        ):
            component = VERIFY.elf_license_component(relative, None)
            VERIFY.REVIEWED_LICENSE_DIGESTS[f"{component}/LICENSE.txt"] = fixture_digest

    def tearDown(self) -> None:
        VERIFY.REVIEWED_LICENSE_DIGESTS = self.reviewed_license_digests
        self.temporary.cleanup()

    def assert_rejected(self, root: pathlib.Path, kind: str, expected: str) -> None:
        with self.assertRaisesRegex(VERIFY.VerificationError, expected):
            VERIFY.verify_tree(root, kind)

    def test_valid_tar_and_appimage_trees_are_accepted(self) -> None:
        for kind in ("tar", "appimage"):
            with self.subTest(kind=kind):
                root = self.root / kind
                valid_tree(root, kind)
                VERIFY.verify_tree(root, kind)

    def test_qt_wayland_plugin_is_required_but_accepts_modern_names(self) -> None:
        root = self.root / "missing-wayland"
        valid_tree(root, "tar")
        (root / "plugins/platforms/libqwayland-generic.so").unlink()
        self.assert_rejected(root, "tar", "missing Qt Wayland platform plugin")

    def test_qt_xcb_opengl_integrations_are_required(self) -> None:
        for plugin_name in (
            "libqxcb-egl-integration.so",
            "libqxcb-glx-integration.so",
        ):
            with self.subTest(plugin=plugin_name):
                root = self.root / plugin_name
                valid_tree(root, "tar", excluded_xcb_gl_plugin=plugin_name)
                self.assert_rejected(root, "tar", "missing Qt XCB OpenGL integration plugin")

    def test_terminate_process_stops_the_launch_process_group(self) -> None:
        process = subprocess.Popen(
            ["/bin/sh", "-c", "sleep 30 & wait"],
            start_new_session=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            deadline = time.monotonic() + 2
            children: set[int] = set()
            while time.monotonic() < deadline:
                children = VERIFY.descendants(process.pid) - {process.pid}
                if children:
                    break
                time.sleep(0.02)
            self.assertTrue(children)

            VERIFY.terminate_process(process)

            deadline = time.monotonic() + 2
            while time.monotonic() < deadline and VERIFY.process_group_has_live_members(process.pid):
                time.sleep(0.02)
            self.assertFalse(VERIFY.process_group_has_live_members(process.pid))
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=2)

    def test_optional_qml_module_can_be_absent(self) -> None:
        destination = self.root / "destination"
        STAGE.copy_tree_if_present(self.root / "missing", destination)
        self.assertFalse(destination.exists())

        source = self.root / "present"
        write(source / "qmldir", b"module Optional\n")
        STAGE.copy_tree_if_present(source, destination)
        self.assertEqual((destination / "qmldir").read_bytes(), b"module Optional\n")

    def test_reset_directory_rejects_traversal_and_symlink_roots(self) -> None:
        package_library = SCRIPT_ROOT / "lib/package_linux_common.sh"
        shell = (
            '. "$PACKAGE_LIBRARY"\n'
            'PERIGEE_SOURCE_ROOT="$SOURCE_ROOT"\n'
            'PERIGEE_BUILD_DIR="$BUILD_ROOT"\n'
            'PERIGEE_STAGE_DIR="$STAGE_ROOT"\n'
            'perigee_reset_directory "$RESET_TARGET"\n'
        )

        with tempfile.TemporaryDirectory(prefix="perigee-reset-test-", dir="/tmp") as temporary:
            reset_root = pathlib.Path(temporary)
            victim = reset_root / "victim"
            write(victim / "sentinel", b"keep\n")
            allowed = reset_root / "allowed"
            allowed.mkdir()
            traversal = allowed / ".." / "victim"
            environment = os.environ.copy()
            environment.update(
                {
                    "PACKAGE_LIBRARY": str(package_library),
                    "SOURCE_ROOT": str(SCRIPT_ROOT.parent),
                    "BUILD_ROOT": str(allowed),
                    "STAGE_ROOT": str(reset_root / "stage"),
                    "RESET_TARGET": str(traversal),
                }
            )
            result = subprocess.run(["bash", "-c", shell], env=environment, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((victim / "sentinel").read_bytes(), b"keep\n")

            link = reset_root / "linked-build"
            link.symlink_to(victim, target_is_directory=True)
            environment["BUILD_ROOT"] = str(link)
            environment["RESET_TARGET"] = str(link)
            result = subprocess.run(["bash", "-c", shell], env=environment, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(link.is_symlink())
            self.assertEqual((victim / "sentinel").read_bytes(), b"keep\n")

            checkout = reset_root / "checkout"
            write(checkout / "sentinel", b"source\n")
            environment.update(
                {
                    "SOURCE_ROOT": str(checkout),
                    "BUILD_ROOT": str(checkout),
                    "RESET_TARGET": str(checkout),
                }
            )
            result = subprocess.run(["bash", "-c", shell], env=environment, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((checkout / "sentinel").read_bytes(), b"source\n")

            parent = reset_root / "source-parent"
            checkout = parent / "checkout"
            write(checkout / "sentinel", b"source\n")
            environment.update(
                {
                    "SOURCE_ROOT": str(checkout),
                    "BUILD_ROOT": str(parent),
                    "RESET_TARGET": str(parent),
                }
            )
            result = subprocess.run(["bash", "-c", shell], env=environment, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((checkout / "sentinel").read_bytes(), b"source\n")

    def test_each_required_payload_is_rejected_when_missing(self) -> None:
        for required in VERIFY.required_paths("tar"):
            with self.subTest(required=required):
                root = self.root / required.replace("/", "-")
                valid_tree(root, "tar")
                path = root / required
                if path.is_dir():
                    shutil.rmtree(path)
                else:
                    path.unlink()
                self.assert_rejected(root, "tar", "missing payload path")

    def test_required_qml_module_is_rejected_when_missing(self) -> None:
        root = self.root / "missing-controls"
        valid_tree(root, "tar")
        (root / "qml/QtQuick/Controls/qmldir").unlink()
        self.assert_rejected(root, "tar", "missing payload path")

    def test_license_manifest_is_required(self) -> None:
        required = set(VERIFY.required_paths("tar"))
        self.assertIn("share/licenses/perigee/SHA256SUMS", required)
        self.assertIn("share/licenses/perigee/licenses.tsv", required)
        self.assertIn("share/licenses/perigee/system-provenance.tsv", required)
        self.assertTrue(
            {
                "qml/QML/qmldir",
                "qml/QtCore/qmldir",
                "qml/QtQuick/Controls/Material/qmldir",
            }.issubset(required)
        )

    def test_source_identity_bytes_and_icon_cross_reference_are_required(self) -> None:
        root = self.root / "source-identity"
        valid_tree(root, "tar")
        desktop = root / "share/applications/app.perigee_stream.Perigee.desktop"
        desktop.write_bytes(desktop.read_bytes() + b"X-Fixture=changed\n")
        self.assert_rejected(root, "tar", "source identity")

        root = self.root / "icon-cross-reference"
        valid_tree(root, "tar")
        desktop = root / "share/applications/app.perigee_stream.Perigee.desktop"
        desktop.write_text(
            "[Desktop Entry]\nType=Application\nName=Perigee\nExec=perigee\nIcon=missing-icon\n",
            encoding="utf-8",
        )
        self.assert_rejected(root, "tar", "desktop icon")

        root = self.root / "appstream-version"
        valid_tree(root, "tar")
        appstream = root / "share/metainfo/app.perigee_stream.Perigee.appdata.xml"
        appstream.write_bytes(appstream.read_bytes().replace(b'version="0.1.0"', b'version="9.9.9"'))
        self.assert_rejected(root, "tar", "AppStream release version")

    def test_non_executable_binary_is_rejected(self) -> None:
        root = self.root / "non-executable"
        binary = valid_tree(root, "tar")
        binary.chmod(0o644)
        self.assert_rejected(root, "tar", "non-executable")

    def test_wrong_binary_architecture_is_rejected(self) -> None:
        root = self.root / "wrong-architecture"
        binary = valid_tree(root, "tar")
        binary.write_text("not an ELF binary\n", encoding="utf-8")
        self.assert_rejected(root, "tar", "not an ELF")

    def test_missing_elf_needed_dependency_is_rejected(self) -> None:
        root = self.root / "missing-needed"
        binary = valid_tree(root, "tar")
        subprocess.run(["patchelf", "--add-needed", "libperigee-missing.so.1", str(binary)], check=True)
        self.assert_rejected(root, "tar", "unresolved ELF dependency")

    def test_host_needed_allowlist_requires_an_exact_soname(self) -> None:
        root = self.root / "spoofed-host-needed"
        binary = valid_tree(root, "tar")
        subprocess.run(["patchelf", "--add-needed", "libc.so.6.perigee-spoof", str(binary)], check=True)
        with self.assertRaisesRegex(VERIFY.VerificationError, "unresolved ELF dependency"):
            VERIFY.verify_elf_dependency_closure(root, [binary])

    def test_qt_xcb_glx_dependency_remains_host_provided(self) -> None:
        soname = "libxcb-glx.so.0"
        self.assertTrue(STAGE.is_excluded_library(soname))

        root = self.root / "host-xcb-glx"
        binary = valid_tree(root, "tar")
        subprocess.run(["patchelf", "--add-needed", soname, str(binary)], check=True)
        VERIFY.verify_elf_dependency_closure(root, [binary])

    def test_needed_provider_must_be_a_private_library_with_matching_soname(self) -> None:
        root = self.root / "mislocated-needed"
        binary = valid_tree(root, "tar")
        name = "libperigee-mislocated.so.1"
        subprocess.run(["patchelf", "--add-needed", name, str(binary)], check=True)
        provider = root / "qml/fixture" / name
        provider.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2("/usr/bin/true", provider)
        normalize_fixture_elf(provider, "$ORIGIN")
        with self.assertRaisesRegex(VERIFY.VerificationError, "unresolved ELF dependency"):
            VERIFY.verify_elf_dependency_closure(root, [binary, provider])

        private_provider = root / "lib" / name
        shutil.copy2("/usr/bin/true", private_provider)
        normalize_fixture_elf(private_provider, "$ORIGIN")
        with self.assertRaisesRegex(VERIFY.VerificationError, "SONAME"):
            VERIFY.verify_elf_dependency_closure(root, [binary, private_provider])

    def test_license_bytes_and_elf_mapping_are_verified(self) -> None:
        root = self.root / "license-bytes"
        valid_tree(root, "tar")
        license_file = root / "share/licenses/perigee/source/moonlight-common/LICENSE.txt"
        license_file.write_bytes(license_file.read_bytes() + b"changed\n")
        self.assert_rejected(root, "tar", "license digest")

        root = self.root / "license-mapping"
        valid_tree(root, "tar")
        mapping = root / "share/licenses/perigee/licenses.tsv"
        mapping.write_text("", encoding="utf-8")
        self.assert_rejected(root, "tar", "ELF license mapping")

    def test_dependency_license_content_must_be_credible(self) -> None:
        root = self.root / "license-content-forgery"
        valid_tree(root, "tar")
        license_root = root / "share/licenses/perigee"
        license_file = next(license_root.glob("elf/**/LICENSE.txt"))
        license_file.write_bytes(b"not a license\n")
        relative = license_file.relative_to(license_root).as_posix()
        refresh_license_checksum(root, "tar", relative)
        self.assert_rejected(root, "tar", "reviewed license catalog")

        root = self.root / "license-content-generic-keyword-forgery"
        valid_tree(root, "tar")
        license_root = root / "share/licenses/perigee"
        license_file = next(license_root.glob("elf/**/LICENSE.txt"))
        license_file.write_bytes(b"license " + b"x" * 200)
        relative = license_file.relative_to(license_root).as_posix()
        refresh_license_checksum(root, "tar", relative)
        self.assert_rejected(root, "tar", "reviewed license catalog")

    def test_license_component_identity_and_exact_file_set_are_verified(self) -> None:
        root = self.root / "license-component-forgery"
        valid_tree(root, "tar")
        mapping = root / "share/licenses/perigee/licenses.tsv"
        lines = mapping.read_text(encoding="utf-8").splitlines()
        lines = [
            f"{line.split(chr(9), 1)[0]}\tsource/perigee"
            if "libfixture.so.1" in line
            else line
            for line in lines
        ]
        mapping.write_text("\n".join(lines) + "\n", encoding="utf-8")
        self.assert_rejected(root, "tar", "license component identity")

        root = self.root / "unlisted-license"
        valid_tree(root, "tar")
        write(root / "share/licenses/perigee/unused/LICENSE", b"unused\n")
        self.assert_rejected(root, "tar", "license digest manifest does not cover")

        root = self.root / "unused-component"
        valid_tree(root, "tar")
        license_root = root / "share/licenses/perigee"
        unused = license_root / "unused/LICENSE"
        write(unused, b"unused\n")
        checksum = hashlib.sha256(unused.read_bytes()).hexdigest()
        with (license_root / "SHA256SUMS").open("a", encoding="utf-8") as manifest:
            manifest.write(f"{checksum}  unused/LICENSE\n")
        self.assert_rejected(root, "tar", "unused license component")

    def test_system_license_provenance_accepts_authoritative_dpkg_and_rpm(self) -> None:
        for manager in ("dpkg", "rpm"):
            with self.subTest(manager=manager):
                fixture = system_provenance_fixture(self.root / manager, manager)
                with fixture["query_patch"]:
                    VERIFY.verify_system_component_provenance(
                        fixture["payload_root"],
                        fixture["component"],
                        [fixture["payload_elf"]],
                        [fixture["packaged_path"]],
                        [fixture["row"]],
                        fixture["license_root"],
                    )

    def test_dpkg_system_license_provenance_accepts_same_source_license_owner(self) -> None:
        fixture = system_provenance_fixture(self.root / "dpkg-split-owner", "dpkg")
        row = fixture["row"]._replace(
            license_package="fixture-license:all",
            license_version="1.0-1",
        )

        def query(arguments: list[str]) -> str:
            if arguments[:1] == ["-S"]:
                return (
                    f"{row.package}: {fixture['provider']}\n"
                    if arguments[1] == str(fixture["provider"])
                    else f"{row.license_package}: {fixture['license_source']}\n"
                )
            if arguments[:2] == ["-W", "-f=${Version}"]:
                return "1.0-1"
            if arguments[:2] == ["-W", "-f=${source:Package}\t${source:Version}"]:
                return "fixture-source\t1.0-1"
            if arguments[:1] == ["-L"]:
                return (
                    f"{fixture['provider']}\n"
                    if arguments[1] == row.package
                    else f"{fixture['license_source']}\n"
                )
            raise AssertionError(arguments)

        with mock.patch.object(VERIFY, "run_dpkg_query", side_effect=query):
            VERIFY.verify_system_component_provenance(
                fixture["payload_root"],
                fixture["component"],
                [fixture["payload_elf"]],
                [fixture["packaged_path"]],
                [row],
                fixture["license_root"],
            )

    def test_rpm_system_provenance_accepts_split_license_owner(self) -> None:
        fixture = system_provenance_fixture(self.root / "rpm-split-owner", "rpm")
        row = fixture["row"]._replace(
            license_package="libfixture-license.noarch",
            license_version="1.0-1",
        )

        def query(arguments: list[str]) -> str:
            if arguments[:3] == ["-qf", "--qf", "%{NAME}.%{ARCH}\n"]:
                owners = (
                    ["libfixture.x86_64"]
                    if arguments[3] == str(fixture["provider"])
                    else ["libfixture-license.noarch", "shared-license-owner.x86_64"]
                )
                return "".join(f"{owner}\n" for owner in owners)
            if arguments[:3] == ["-q", "--qf", "%{EVR}"]:
                return "1.0-1"
            if arguments[:3] == ["-q", "--qf", "%{SOURCERPM}"]:
                return "libfixture-1.0-1.src.rpm"
            if arguments[:1] == ["-ql"]:
                if arguments[1] == "libfixture.x86_64":
                    return f"{fixture['provider']}\n"
                return f"{fixture['license_source']}\n"
            raise AssertionError(arguments)

        with mock.patch.object(VERIFY, "run_rpm_query", side_effect=query):
            VERIFY.verify_system_component_provenance(
                fixture["payload_root"],
                fixture["component"],
                [fixture["payload_elf"]],
                [fixture["packaged_path"]],
                [row],
                fixture["license_root"],
            )

    def test_rpm_system_provenance_rejects_unrelated_co_owner_identity(self) -> None:
        fixture = system_provenance_fixture(self.root / "rpm-co-owner-drift", "rpm")
        row = fixture["row"]._replace(
            license_package="unrelated-license.noarch",
            license_version="1.0-1",
        )

        def query(arguments: list[str]) -> str:
            if arguments[:3] == ["-qf", "--qf", "%{NAME}.%{ARCH}\n"]:
                if arguments[3] == str(fixture["provider"]):
                    return "libfixture.x86_64\n"
                return "libfixture-license.noarch\nshared-license-owner.x86_64\n"
            if arguments[:3] == ["-q", "--qf", "%{EVR}"]:
                return "1.0-1"
            if arguments[:1] == ["-ql"]:
                if arguments[1] == "libfixture.x86_64":
                    return f"{fixture['provider']}\n"
                return f"{fixture['license_source']}\n"
            raise AssertionError(arguments)

        with mock.patch.object(VERIFY, "run_rpm_query", side_effect=query):
            with self.assertRaisesRegex(
                VERIFY.VerificationError,
                "system provenance license owner mismatch",
            ):
                VERIFY.verify_system_component_provenance(
                    fixture["payload_root"],
                    fixture["component"],
                    [fixture["payload_elf"]],
                    [fixture["packaged_path"]],
                    [row],
                    fixture["license_root"],
                )

    def test_rpm_system_provenance_rejects_unrelated_dependency_license(self) -> None:
        fixture = system_provenance_fixture(self.root / "rpm-dependency-license", "rpm")
        dependency_license = (
            self.root
            / "rpm-dependency-license/host/usr/share/doc/bash/COPYING"
        )
        write(dependency_license, fixture["license_source"].read_bytes())
        row = fixture["row"]._replace(
            license_package="bash.x86_64",
            license_version="5.3-1",
            license_source=dependency_license,
        )

        def query(arguments: list[str]) -> str:
            if arguments[:3] == ["-qf", "--qf", "%{NAME}.%{ARCH}\n"]:
                owner = (
                    "libfixture.x86_64"
                    if arguments[3] == str(fixture["provider"])
                    else "bash.x86_64"
                )
                return f"{owner}\n"
            if arguments[:3] == ["-q", "--qf", "%{EVR}"]:
                return "1.0-1" if arguments[3] == "libfixture.x86_64" else "5.3-1"
            if arguments[:3] == ["-q", "--qf", "%{SOURCERPM}"]:
                return (
                    "libfixture-1.0-1.src.rpm"
                    if arguments[3] == "libfixture.x86_64"
                    else "bash-5.3-1.src.rpm"
                )
            if arguments[:1] == ["-ql"]:
                if arguments[1] == "libfixture.x86_64":
                    return f"{fixture['provider']}\n"
                return f"{dependency_license}\n"
            raise AssertionError(arguments)

        with mock.patch.object(VERIFY, "run_rpm_query", side_effect=query):
            with self.assertRaisesRegex(
                VERIFY.VerificationError,
                "system provenance license is unrelated to package",
            ):
                VERIFY.verify_system_component_provenance(
                    fixture["payload_root"],
                    fixture["component"],
                    [fixture["payload_elf"]],
                    [fixture["packaged_path"]],
                    [row],
                    fixture["license_root"],
                )

    def test_dpkg_system_license_provenance_rejects_identity_and_byte_drift(self) -> None:
        cases = (
            "unrelated-package",
            "forged-version",
            "forged-license-path",
            "same-soname-wrong-owner",
            "altered-license-bytes",
            "missing-owner",
            "missing-provenance-row",
        )
        for case in cases:
            with self.subTest(case=case):
                fixture = system_provenance_fixture(self.root / case, "dpkg")
                row = fixture["row"]
                component_files = [fixture["packaged_path"]]
                owner_patch = mock.patch.object(
                    VERIFY,
                    "dpkg_owners",
                    return_value={row.package},
                )
                if case == "unrelated-package":
                    row = row._replace(package="unrelated:amd64")
                elif case == "forged-version":
                    row = row._replace(version="9.9-forged")
                elif case == "forged-license-path":
                    row = row._replace(
                        license_source=self.root / case / "host/usr/share/doc/forged/copyright"
                    )
                elif case == "same-soname-wrong-owner":
                    owner_patch = mock.patch.object(
                        VERIFY,
                        "dpkg_owners",
                        return_value={"wrong-owner:amd64"},
                    )
                elif case == "altered-license-bytes":
                    fixture["license_source"].write_bytes(b"altered authoritative bytes\n")
                elif case == "missing-owner":
                    owner_patch = mock.patch.object(VERIFY, "dpkg_owners", return_value=set())
                elif case == "missing-provenance-row":
                    extra = fixture["license_root"] / fixture["component"] / "NOTICE"
                    write(extra, b"Copyright Fixture\nPermission granted.\n")
                    component_files.append(extra)
                with fixture["query_patch"], owner_patch:
                    with self.assertRaises(VERIFY.VerificationError):
                        VERIFY.verify_system_component_provenance(
                            fixture["payload_root"],
                            fixture["component"],
                            [fixture["payload_elf"]],
                            component_files,
                            [row],
                            fixture["license_root"],
                        )

    def test_system_provenance_fails_when_package_manager_is_absent(self) -> None:
        with mock.patch.object(VERIFY.shutil, "which", return_value=None):
            with self.assertRaisesRegex(VERIFY.VerificationError, "requires dpkg-query"):
                VERIFY.run_dpkg_query(["-W", "fixture:amd64"])
            with self.assertRaisesRegex(VERIFY.VerificationError, "requires rpm"):
                VERIFY.run_rpm_query(["-q", "fixture.x86_64"])

    def test_apprun_must_be_executable_and_regular(self) -> None:
        root = self.root / "apprun-mode"
        valid_tree(root, "appimage")
        (root / "AppRun").chmod(0o644)
        self.assert_rejected(root, "appimage", "AppRun")

        root = self.root / "apprun-content"
        valid_tree(root, "appimage")
        write(root / "AppRun", b"#!/bin/sh\nexec usr/bin/perigee \"$@\"\n", 0o755)
        self.assert_rejected(root, "appimage", "AppRun content")

    def test_forbidden_wayland_and_graphics_libraries_are_rejected(self) -> None:
        for name in (
            "libwayland-client.so.0",
            "libwayland-egl.so.1",
            "libEGL.so.1",
            "libGL.so.1",
            "libEGL_mesa.so.0",
            "libGLX_mesa.so.0",
            "libwayland-cursor.so.0",
            "libvulkan.so.1",
            "libva.so.2",
            "libdrm.so.2",
            "libX11.so.6",
            "libX11-xcb.so.1",
            "libxcb.so.1",
            "libxkbcommon.so.0",
            "libstdc++.so.6",
            "libgcc_s.so.1",
        ):
            with self.subTest(name=name):
                root = self.root / name
                valid_tree(root, "tar")
                write(root / "lib" / name, b"fixture\n")
                self.assert_rejected(root, "tar", "bundled host graphics or Wayland")

    def test_complete_keys_and_common_tokens_are_rejected_inside_elfs(self) -> None:
        fixtures = (
            (
                b"-----BEGIN RSA PRIVATE KEY-----\n"
                b"MIIEowIBAAKCAQEA0123456789abcdefghijklmnopqrstuv\n"
                b"-----END RSA PRIVATE KEY-----\n",
                "private-key block",
            ),
            (
                b"-----BEGIN PGP PRIVATE KEY BLOCK-----\n"
                b"Version: GnuPG v2\n"
                b"\n"
                b"lQOYBGY8QY0BCADL0M5pUsQ2G3F1Y2VOb3QtYS1yZWFsLWtleQ==\n"
                b"=AbCd\n"
                b"-----END PGP PRIVATE KEY BLOCK-----\n",
                "private-key block",
            ),
            (
                b"-----BEGIN RSA PRIVATE KEY-----\n"
                b"Proc-Type: 4,ENCRYPTED\n"
                b"DEK-Info: AES-256-CBC,0123456789ABCDEF0123456789ABCDEF\n"
                b"\n"
                b"MIIE6TAbBgkqhkiG9w0BBQMwDgQIxmZpeGxlc3QCAggABIIEyA==\n"
                b"-----END RSA PRIVATE KEY-----\n",
                "private-key block",
            ),
            (b"ghp_0123456789abcdefghijklmnopqrstuvwxyz", "GitHub token"),
            (b"AKIA0123456789ABCDEF", "AWS access key"),
            (b"eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJwZXJpZ2VlIn0.signature123", "JWT"),
        )
        for index, (content, expected) in enumerate(fixtures):
            with self.subTest(expected=expected):
                root = self.root / f"elf-secret-{index}"
                valid_tree(root, "tar")
                library = root / "lib/libfixture.so.1"
                with library.open("ab") as handle:
                    handle.write(content)
                self.assert_rejected(root, "tar", expected)

        root = self.root / "elf-secret-long-pgp"
        valid_tree(root, "tar")
        library = root / "lib/libfixture.so.1"
        with library.open("ab") as handle:
            handle.write(
                b"-----BEGIN PGP PRIVATE KEY BLOCK-----\n"
                + b"A" * 140_000
                + b"\n-----END PGP PRIVATE KEY BLOCK-----\n"
            )
        self.assert_rejected(root, "tar", "private-key block")

    def test_every_payload_elf_must_be_x86_64(self) -> None:
        root = self.root / "foreign-architecture"
        valid_tree(root, "tar")
        source = root / "foreign.s"
        obj = root / "foreign.o"
        library = root / "lib/libforeign.so"
        write(source, b".text\n.global foreign_symbol\nforeign_symbol:\n  ret\n")
        subprocess.run(["as", "--32", "-o", str(obj), str(source)], check=True)
        subprocess.run(
            [
                "ld",
                "-m",
                "elf_i386",
                "-shared",
                "-soname",
                "libforeign.so",
                "-o",
                str(library),
                str(obj),
            ],
            check=True,
        )
        source.unlink()
        obj.unlink()

        license_root = root / "share/licenses/perigee"
        component = "elf/soname-libforeign.so"
        license_file = license_root / component / "LICENSE.txt"
        write(
            license_file,
            b"Copyright 2026 Fixture Authors\n"
            b"Permission is hereby granted, free of charge, to use, copy, modify, "
            b"merge, publish, distribute, sublicense, and/or sell this software.\n"
            b"The software is provided without warranty and without liability.\n",
        )
        digest = hashlib.sha256(license_file.read_bytes()).hexdigest()
        with (license_root / "SHA256SUMS").open("a", encoding="utf-8") as manifest:
            manifest.write(f"{digest}  {component}/LICENSE.txt\n")
        with (license_root / "licenses.tsv").open("a", encoding="utf-8") as mappings:
            mappings.write(f"lib/libforeign.so\t{component}\n")

        self.assert_rejected(root, "tar", "64-bit")

    def test_required_qml_modules_must_contain_declared_runtime_files(self) -> None:
        root = self.root / "stripped-qml-module"
        valid_tree(root, "tar")
        (root / "qml/QtQuick/Controls/FixtureType.qml").unlink()
        self.assert_rejected(root, "tar", "qmldir target")

    def test_mesa_driver_is_rejected(self) -> None:
        root = self.root / "mesa"
        valid_tree(root, "tar")
        write(root / "lib/dri/radeonsi_dri.so", b"fixture\n")
        self.assert_rejected(root, "tar", "bundled Mesa driver")

    def test_secrets_private_data_and_absolute_paths_are_rejected(self) -> None:
        fixtures = (
            (
                b"-----BEGIN PRIVATE KEY-----\n"
                b"QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=\n"
                b"-----END PRIVATE KEY-----\n",
                "private-key marker",
            ),
            (b"PERIGEE_TEST_SESSION_TOKEN", "test session token"),
            (b"PRIVATE_HOST_SENTINEL", "private host data"),
            (b"/home/example/perigee", "build-home path"),
            (b"/tmp/perigee-build", "temporary path"),
            (b"-----BEGIN OPENSSH PRIVATE KEY-----", "private-key marker"),
            (b"-----BEGIN ENCRYPTED PRIVATE KEY-----", "private-key marker"),
            (b"-----BEGIN DSA PRIVATE KEY-----", "private-key marker"),
            (b"-----BEGIN RSA PRIVATE KEY-----", "private-key marker"),
            (b"-----BEGIN EC PRIVATE KEY-----", "private-key marker"),
            (b"-----BEGIN PGP PRIVATE KEY BLOCK-----", "private-key marker"),
        )
        for index, (content, expected) in enumerate(fixtures):
            with self.subTest(expected=expected):
                root = self.root / f"secret-{index}"
                valid_tree(root, "tar")
                write(root / "lib/leak.bin", content)
                self.assert_rejected(root, "tar", expected)

    def test_absolute_elf_runtime_path_is_rejected(self) -> None:
        root = self.root / "absolute-rpath"
        binary = valid_tree(root, "tar")
        subprocess.run(
            ["patchelf", "--force-rpath", "--set-rpath", "/home/example/lib", str(binary)],
            check=True,
        )
        self.assert_rejected(root, "tar", "absolute ELF runtime path")

    def test_elf_build_id_is_rejected(self) -> None:
        root = self.root / "build-id"
        valid_tree(root, "tar")
        library = root / "lib/libfixture.so.1"
        shutil.copy2("/usr/bin/true", library)
        self.assert_rejected(root, "tar", "ELF build ID")

    def test_unsafe_modes_are_rejected(self) -> None:
        for mode, expected in ((0o4755, "setuid"), (0o664, "group-writable")):
            with self.subTest(mode=oct(mode)):
                root = self.root / f"mode-{mode:o}"
                valid_tree(root, "tar")
                path = root / "NOTICE.md"
                path.chmod(mode)
                self.assert_rejected(root, "tar", expected)

    def test_symlink_that_escapes_payload_is_rejected(self) -> None:
        root = self.root / "symlink"
        valid_tree(root, "tar")
        (root / "lib/escape").symlink_to("../../../outside")
        self.assert_rejected(root, "tar", "payload symlink is not allowed")

    def test_all_payload_symlinks_including_loops_are_rejected(self) -> None:
        root = self.root / "symlink-loop-tree"
        valid_tree(root, "appimage")
        (root / "loop-a").symlink_to("loop-b")
        (root / "loop-b").symlink_to("loop-a")
        self.assert_rejected(root, "appimage", "payload symlink")

    def test_process_identity_uses_the_expected_binary_digest(self) -> None:
        candidate_dir = self.root / "candidate"
        expected_dir = self.root / "expected"
        candidate_dir.mkdir()
        expected_dir.mkdir()
        candidate = candidate_dir / "perigee"
        expected = expected_dir / "perigee"
        shutil.copy2("/usr/bin/sleep", candidate)
        shutil.copy2("/usr/bin/true", expected)
        process = subprocess.Popen([str(candidate), "30"])
        try:
            self.assertFalse(
                VERIFY.process_matches_expected_binary(
                    process.pid,
                    VERIFY.file_sha256(expected),
                )
            )
            shutil.copy2(candidate, expected)
            self.assertTrue(
                VERIFY.process_matches_expected_binary(
                    process.pid,
                    VERIFY.file_sha256(expected),
                )
            )
        finally:
            process.terminate()
            process.wait(timeout=2)

    def test_exact_version_output_is_required(self) -> None:
        good = self.root / "good-version"
        bad = self.root / "bad-version"
        write(good, b"#!/bin/sh\nprintf 'Perigee 0.1.0\\n'\n", 0o755)
        write(bad, b"#!/bin/sh\nprintf 'Perigee version 0.1.0\\n'\n", 0o755)
        VERIFY.verify_version(good, "Perigee 0.1.0", self.root / "good-home")
        with self.assertRaisesRegex(VERIFY.VerificationError, "unexpected version"):
            VERIFY.verify_version(bad, "Perigee 0.1.0", self.root / "bad-home")

    def test_version_timeout_kills_the_entire_process_group(self) -> None:
        slow = self.root / "slow-version"
        write(slow, b"#!/bin/sh\nsleep 30\n", 0o755)
        with mock.patch.object(VERIFY, "VERSION_COMMAND_TIMEOUT_SECONDS", 0.05):
            with self.assertRaisesRegex(VERIFY.VerificationError, "timed out"):
                VERIFY.verify_version(slow, "Perigee 0.1.0", self.root / "slow-home")

    def test_version_failure_reports_return_code_and_stderr(self) -> None:
        failing = self.root / "failing-version"
        write(failing, b"#!/bin/sh\nprintf 'fixture failure\\n' >&2\nexit 7\n", 0o755)
        with self.assertRaisesRegex(
            VERIFY.VerificationError,
            r"packaged --version command failed \(7\): failing-version: fixture failure",
        ):
            VERIFY.verify_version(failing, "Perigee 0.1.0", self.root / "failing-home")

    def test_whole_appimage_version_sets_extract_and_run(self) -> None:
        appimage = self.root / "Perigee-0.1.0-x86_64.AppImage"
        write(
            appimage,
            b"#!/bin/sh\n"
            b"[ \"${APPIMAGE_EXTRACT_AND_RUN:-}\" = 1 ] || exit 9\n"
            b"printf 'Perigee 0.1.0\\n'\n",
            0o755,
        )
        VERIFY.verify_version(appimage, "Perigee 0.1.0", self.root / "appimage-version")

    def test_launch_liveness_rejects_a_short_lived_process(self) -> None:
        process = subprocess.Popen(["/bin/sh", "-c", "exit 0"], start_new_session=True)
        process.wait(timeout=2)
        with self.assertRaisesRegex(VERIFY.VerificationError, "exited during isolated Wayland launch"):
            VERIFY.assert_launch_liveness(process, process.pid, VERIFY.file_sha256(pathlib.Path("/bin/sh")))

    def test_launch_liveness_rechecks_the_payload_digest(self) -> None:
        candidate = self.root / "perigee"
        replacement = self.root / "replacement"
        shutil.copy2("/usr/bin/sleep", candidate)
        shutil.copy2("/usr/bin/true", replacement)
        expected_digest = VERIFY.file_sha256(candidate)
        process = subprocess.Popen([str(candidate), "30"], start_new_session=True)
        try:
            VERIFY.assert_launch_liveness(process, process.pid, expected_digest)
            os.replace(replacement, candidate)
            with self.assertRaisesRegex(VERIFY.VerificationError, "executable identity changed"):
                VERIFY.assert_launch_liveness(process, process.pid, expected_digest)
        finally:
            process.terminate()
            process.wait(timeout=2)

    def test_launch_environment_selects_qt_quick_software_backend(self) -> None:
        environment = VERIFY.launch_environment(self.root / "launch-work", "perigee-test")
        self.assertEqual(environment["QT_QPA_PLATFORM"], "wayland")
        self.assertEqual(environment["QT_QUICK_BACKEND"], "software")

    def test_duplicate_tar_path_is_rejected(self) -> None:
        archive = self.root / "duplicate.tar.zst"
        entry = tar_info("NOTICE.md")
        make_tar_zst(archive, [(entry, b"one"), (tar_info("NOTICE.md"), b"two")])
        with self.assertRaisesRegex(VERIFY.VerificationError, "duplicate archive path"):
            VERIFY.extract_tar_zst(archive, self.root / "duplicate-out")

    def test_tar_path_traversal_is_rejected(self) -> None:
        archive = self.root / "traversal.tar.zst"
        make_tar_zst(archive, [(tar_info("../outside"), b"unsafe")])
        with self.assertRaisesRegex(VERIFY.VerificationError, "unsafe archive path"):
            VERIFY.extract_tar_zst(archive, self.root / "traversal-out")

    def test_tar_pax_metadata_and_secret_member_names_are_rejected(self) -> None:
        archive = self.root / "pax-secret.tar.zst"
        entry = tar_info("LICENSE")
        entry.pax_headers = {"comment": "PERIGEE_TEST_SESSION_TOKEN"}
        make_tar_zst(archive, [(entry, b"fixture\n")])
        with self.assertRaisesRegex(VERIFY.VerificationError, "unexpected PAX metadata"):
            VERIFY.extract_tar_zst(archive, self.root / "pax-secret-out")

        archive = self.root / "path-secret.tar.zst"
        make_tar_zst(
            archive,
            [(tar_info("PERIGEE_TEST_SESSION_TOKEN"), b"fixture\n")],
        )
        with self.assertRaisesRegex(VERIFY.VerificationError, "test session token"):
            VERIFY.extract_tar_zst(archive, self.root / "path-secret-out")

    def test_tar_member_beneath_prior_symlink_is_rejected(self) -> None:
        archive = self.root / "symlink-parent.tar.zst"
        make_tar_zst(
            archive,
            [
                (tar_info("target", "directory", mode=0o755), b""),
                (tar_info("alias", "symlink", "target", mode=0o777), b""),
                (tar_info("alias/escaped", mode=0o644), b"must not follow alias"),
            ],
        )
        with self.assertRaisesRegex(VERIFY.VerificationError, "non-directory archive ancestor"):
            VERIFY.extract_tar_zst(archive, self.root / "symlink-parent-out")

    def test_tar_symlink_loop_is_rejected(self) -> None:
        archive = self.root / "symlink-loop.tar.zst"
        make_tar_zst(
            archive,
            [
                (tar_info("a", "symlink", "b", mode=0o777), b""),
                (tar_info("b", "symlink", "a", mode=0o777), b""),
            ],
        )
        with self.assertRaisesRegex(VERIFY.VerificationError, "symlink loop"):
            VERIFY.extract_tar_zst(archive, self.root / "symlink-loop-out")

    def test_truncated_zstd_input_is_rejected_safely(self) -> None:
        archive = self.root / "truncated.tar.zst"
        archive.write_bytes(b"not a zstd stream")
        with self.assertRaisesRegex(VERIFY.VerificationError, "zstd rejected tar artifact"):
            VERIFY.extract_tar_zst(archive, self.root / "truncated-out")

    def test_non_root_squashfs_ownership_is_rejected(self) -> None:
        source = self.root / "squash-source"
        write(source / "payload", b"fixture\n")
        squashfs = self.root / "non-root.squashfs"
        subprocess.run(
            [
                "mksquashfs",
                str(source),
                str(squashfs),
                "-noappend",
                "-force-uid",
                "1000",
                "-force-gid",
                "1000",
                "-no-progress",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        with self.assertRaisesRegex(VERIFY.VerificationError, "non-root SquashFS ownership"):
            VERIFY.verify_squashfs_ownership(squashfs, 0)

    def test_squashfs_extended_attributes_are_rejected(self) -> None:
        source = self.root / "xattr-squash-source"
        payload = source / "payload"
        write(payload, b"fixture\n")
        subprocess.run(
            ["setfattr", "-n", "user.comment", "-v", "PERIGEE_TEST_SESSION_TOKEN", str(payload)],
            check=True,
        )
        squashfs = self.root / "xattr.squashfs"
        subprocess.run(
            [
                "mksquashfs",
                str(source),
                str(squashfs),
                "-noappend",
                "-no-progress",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        with self.assertRaisesRegex(VERIFY.VerificationError, "extended attributes"):
            VERIFY.verify_squashfs_metadata(squashfs, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)

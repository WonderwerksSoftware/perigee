#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import inspect
import pathlib
import re
import sys
import tempfile
import unittest
from unittest import mock


SOURCE_ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOW = SOURCE_ROOT / ".github/workflows/build-appimage.yml"
PIPELINE = SOURCE_ROOT / ".github/workflows/perigee-ci.yml"
STAGE_PATH = SOURCE_ROOT / "scripts/lib/stage_linux_payload.py"
VERIFY_PATH = SOURCE_ROOT / "scripts/lib/verify_linux_artifacts.py"
LICENSE_CATALOG_PATH = SOURCE_ROOT / "scripts/lib/reviewed_linux_license_digests.tsv"
LICENSE_CATALOG_GENERATOR = SOURCE_ROOT / "scripts/lib/generate_linux_license_catalog.py"
QT_LICENSE_EXTRACTOR = SOURCE_ROOT / "scripts/lib/extract_qt_license_archives.py"
UBUNTU_PACKAGE_SET = SOURCE_ROOT / "scripts/ci/ubuntu-22.04-packages.txt"
MAIN_CPP = SOURCE_ROOT / "app/main.cpp"


def load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(path.parent))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.pop(0)
    return module


class LinuxPackagingContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = WORKFLOW.read_text(encoding="utf-8")
        self.ubuntu_packages = UBUNTU_PACKAGE_SET.read_text(encoding="utf-8").splitlines()

    def test_workflow_validates_job_kind_before_setup(self) -> None:
        self.assertIn('case "$JOB_KIND" in', self.workflow)
        self.assertIn("artifacts|qttest|sunshine", self.workflow)
        self.assertIn("unsupported job_kind", self.workflow)

    def test_runtime_inputs_enter_shell_only_through_environment(self) -> None:
        self.assertIn("APPIMAGE_RUNTIME_URL: ${{ inputs.appimage_runtime_url }}", self.workflow)
        self.assertIn("APPIMAGE_RUNTIME_SHA256: ${{ inputs.appimage_runtime_sha256 }}", self.workflow)
        self.assertNotIn("'${{ inputs.appimage_runtime_url }}'", self.workflow)
        self.assertNotIn("'${{ inputs.appimage_runtime_sha256 }}'", self.workflow)
        self.assertRegex(self.workflow, r"\[\[ \"\$APPIMAGE_RUNTIME_SHA256\" =~ \^\[0-9a-f\]\{64\}\$ \]\]")

    def test_ci_build_inputs_and_native_wayland_assertion_are_present(self) -> None:
        self.assertIn("attr", self.ubuntu_packages)
        self.assertIn("libwayland-dev", self.ubuntu_packages)
        self.assertIn("kwin-wayland-backend-virtual", self.ubuntu_packages)
        self.assertRegex(self.workflow, r"configure[^\n]*--enable-x11[^\n]*--enable-wayland[^\n]*--enable-drm")
        self.assertNotIn("libshaderc-dev", self.workflow)
        self.assertIn("-Dglslang=enabled -Dshaderc=disabled", self.workflow)
        package_common = (SOURCE_ROOT / "scripts/lib/package_linux_common.sh").read_text(encoding="utf-8")
        self.assertIn("perigee_assert_native_wayland", package_common)
        self.assertIn("libwayland-client.so", package_common)
        self.assertIn("libva-wayland.so", package_common)
        self.assertIn(
            "PKG_CONFIG_PATH=%s/lib/pkgconfig:%s/lib/x86_64-linux-gnu/pkgconfig:%s/share/pkgconfig",
            self.workflow,
        )

    def test_version_query_bypasses_gui_initialization(self) -> None:
        main_cpp = MAIN_CPP.read_text(encoding="utf-8")
        self.assertIn(
            'QString::fromLocal8Bit(argv[i]) == QStringLiteral("--version")',
            main_cpp,
        )
        self.assertLess(
            main_cpp.index('QString::fromLocal8Bit(argv[i]) == QStringLiteral("--version")'),
            main_cpp.index("QGuiApplication app(argc, argv);"),
        )

    def test_ci_exposes_sdl2_compat_under_the_qmake_package_name(self) -> None:
        sdl2_block = re.search(
            r"- name: Build sdl2-compat(?P<body>.*?)(?=\n\s+- name:)",
            self.workflow,
            re.S,
        )
        self.assertIsNotNone(sdl2_block)
        assert sdl2_block is not None
        self.assertIn('"$DEP_ROOT/lib/pkgconfig/sdl2-compat.pc"', sdl2_block.group("body"))
        self.assertIn('"$DEP_ROOT/lib/pkgconfig/sdl2.pc"', sdl2_block.group("body"))
        self.assertRegex(
            sdl2_block.group("body"),
            r'install -m 0644 "\$DEP_ROOT/lib/pkgconfig/sdl2-compat\.pc" '
            r'\\\s*"\$DEP_ROOT/lib/pkgconfig/sdl2\.pc"',
        )

    def test_ci_installs_and_gates_a_pinned_supported_qt(self) -> None:
        self.assertIn("runs-on: ubuntu-22.04", self.workflow)
        verifier_workflow = (SOURCE_ROOT / ".github/workflows/perigee-ci.yml").read_text(encoding="utf-8")
        self.assertIn("runs-on: ubuntu-22.04", verifier_workflow)
        self.assertIn("aqtinstall==3.3.0", self.workflow)
        self.assertIn("QT_VERSION: 6.8.3", self.workflow)
        self.assertIn("aqt install-qt linux desktop 6.8.3 linux_gcc_64", self.workflow)
        self.assertIn("-m qtvirtualkeyboard", self.workflow)
        self.assertIn('test -f "$QT_DIR/plugins/platforms/libqwayland-generic.so"', self.workflow)
        self.assertIn('test -f "$QT_DIR/plugins/platforms/libqwayland-egl.so"', self.workflow)
        self.assertNotIn("6.8.3 gcc_64 \\", self.workflow)
        self.assertIn("QT_ROOT", self.workflow)
        self.assertRegex(self.workflow, r"qmake6[^\n]*-query QT_VERSION")
        self.assertIn("Perigee requires Qt 6.7 or newer", self.workflow)
        self.assertIn('command -v qmake6', self.workflow)
        self.assertIn('"$QT_DIR/bin/qmake6"', self.workflow)
        self.assertLess(self.workflow.index("Validate Qt version"), self.workflow.index("Cache immutable dependencies"))

    def test_steam_link_build_keeps_its_supported_qt5_path(self) -> None:
        app_pro = (SOURCE_ROOT / "app/app.pro").read_text(encoding="utf-8")
        self.assertIn("!config_SL:!versionAtLeast(QT_VERSION, 6.7.0)", app_pro)
        main_cpp = (SOURCE_ROOT / "app/main.cpp").read_text(encoding="utf-8")
        graphics_index = main_cpp.index("QQuickWindow::setGraphicsApi")
        linux_guard = main_cpp.rfind("#ifdef Q_OS_LINUX", 0, graphics_index)
        graphics_api = main_cpp[linux_guard:graphics_index]
        self.assertIn("#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)", graphics_api)
        renderer = (SOURCE_ROOT / "app/perigee/deck/decksurfacerenderer.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("quickWindow->setRenderTarget(framebuffer.get());", renderer)
        self.assertIn("renderControl->initialize(context.get());", renderer)
        self.assertRegex(
            renderer,
            r"#if QT_VERSION >= QT_VERSION_CHECK\(6, 0, 0\)\s+"
            r"m_Impl->renderControl->beginFrame\(\);",
        )
        self.assertRegex(
            renderer,
            r"#if QT_VERSION >= QT_VERSION_CHECK\(6, 0, 0\)\s+"
            r"m_Impl->renderControl->endFrame\(\);",
        )
        self.assertIn("format.setInternalTextureFormat(GL_RGBA);", renderer)
        bindings = (SOURCE_ROOT / "app/perigee/input/deckbindings.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "#if defined(Q_OS_LINUX) && !defined(STEAM_LINK)\n"
            "#include <linux/input-event-codes.h>",
            bindings,
        )
        adapter = (SOURCE_ROOT / "app/perigee/polaris/polarisadapter.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)", adapter)
        self.assertIn("QStringConverter::Flag::Stateless", adapter)
        self.assertIn("#include <QTextCodec>", adapter)
        self.assertIn("QTextCodec::ConverterState", adapter)
        self.assertIn("state.invalidChars == 0 && state.remainingChars == 0", adapter)
        api_client = (SOURCE_ROOT / "app/perigee/polaris/polarisapiclient.cpp").read_text(
            encoding="utf-8"
        )
        self.assertRegex(
            api_client,
            r"#if QT_VERSION >= QT_VERSION_CHECK\(5, 15, 0\)\s+"
            r"request\.setAttribute\(QNetworkRequest::Http2AllowedAttribute, false\);\s+"
            r"#else\s+"
            r"request\.setAttribute\(QNetworkRequest::HTTP2AllowedAttribute, false\);\s+"
            r"#endif",
        )

    def test_ci_installs_pinned_qt_source_license_texts(self) -> None:
        for module, digest in (
            ("qtbase", "56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80"),
            ("qtsvg", "35eb516460f00f264eb504baa253432384351cf23fb9980a5857190e8deef438"),
            ("qtdeclarative", "1f03a2b8f5588b4face7da87926e9b2c1372b3a32157c52df07a75067a9db1af"),
            ("qtwayland", "20fe385887d21190165a3180c17dcfc8b9a0e1da4ec76865b6334bdc709994b0"),
            ("qtvirtualkeyboard", "8111061261ed8d88ec40b79083f8ed025650eb1807a05528615265d36213bb1d"),
        ):
            self.assertIn(
                f"{module.upper()}_SOURCE_LICENSE_URL: https://download.qt.io/official_releases/qt/6.8/6.8.3/submodules/{module}-everywhere-src-6.8.3.tar.xz",
                self.workflow,
            )
            self.assertIn(f"{module.upper()}_SOURCE_LICENSE_SHA256: {digest}", self.workflow)
        self.assertIn("Install pinned Qt source license texts", self.workflow)
        self.assertIn("extract_qt_license_archives.py", self.workflow)
        self.assertIn('rm -rf -- "$QT_DIR/LICENSES"', self.workflow)
        self.assertIn('mv -- "$qt_license_stage" "$QT_DIR/LICENSES"', self.workflow)
        self.assertIn("sha256sum --check --strict", self.workflow)
        self.assertIn('test -f "$QT_DIR/LICENSES/qtbase/LicenseRef-Qt-Commercial.txt"', self.workflow)
        self.assertIn("qt source license archives", self.workflow)
        self.assertLess(
            self.workflow.index("Install pinned Qt source license texts"),
            self.workflow.index("Validate pinned Meson version"),
        )

    def test_qt_license_extractor_rejects_unsafe_members_and_preserves_module_texts(self) -> None:
        extractor = load("extract_qt_license_archives", QT_LICENSE_EXTRACTOR)

        def make_archive(path: pathlib.Path, root: str, members: list[tuple[str, bytes, str]]) -> None:
            import tarfile

            with tarfile.open(path, "w:xz") as handle:
                for name, content, kind in members:
                    info = tarfile.TarInfo(f"{root}/LICENSES/{name}")
                    if kind == "file":
                        info.size = len(content)
                        handle.addfile(info, __import__("io").BytesIO(content))
                    else:
                        info.type = tarfile.SYMTYPE
                        info.linkname = "../../outside"
                        handle.addfile(info)

        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            base = root / "qtbase-everywhere-src-6.8.3.tar.xz"
            virtual = root / "qtvirtualkeyboard-everywhere-src-6.8.3.tar.xz"
            make_archive(base, "qtbase-everywhere-src-6.8.3", [("Module.txt", b"base\n", "file")])
            make_archive(
                virtual,
                "qtvirtualkeyboard-everywhere-src-6.8.3",
                [("Module.txt", b"virtual keyboard\n", "file")],
            )
            destination = root / "licenses"
            extractor.extract_archives([base, virtual], destination)
            self.assertEqual((destination / "qtbase/Module.txt").read_bytes(), b"base\n")
            self.assertEqual(
                (destination / "qtvirtualkeyboard/Module.txt").read_bytes(),
                b"virtual keyboard\n",
            )
            with self.assertRaises(extractor.LicenseArchiveError):
                extractor.extract_archives([base], destination)

            unsafe = root / "qtwayland-everywhere-src-6.8.3.tar.xz"
            make_archive(unsafe, "qtwayland-everywhere-src-6.8.3", [("escape", b"", "symlink")])
            with self.assertRaises(extractor.LicenseArchiveError):
                extractor.extract_archives([unsafe], root / "unsafe")

    def test_qt_license_origins_keep_module_identity(self) -> None:
        stage = load("stage_linux_payload", STAGE_PATH)
        prefix = pathlib.Path("/opt/qt")
        cases = {
            "lib/libQt6VirtualKeyboard.so.6": "qtvirtualkeyboard",
            "plugins/platforms/libqwayland-generic.so": "qtwayland",
            "lib/libQt6WlShellIntegration.so.6": "qtwayland",
            "plugins/imageformats/libqsvg.so": "qtsvg",
            "qml/QtCore/libqtqmlcoreplugin.so": "qtdeclarative",
            "qml/QtQuick/Controls/libqtquickcontrols2plugin.so": "qtdeclarative",
            "lib/libQt6Core.so.6": "qtbase",
            "lib/libicudata.so.73": "qtbase",
            "plugins/platforms/libqxcb.so": "qtbase",
            "plugins/tls/libqopensslbackend.so": "qtbase",
        }
        for relative, module in cases.items():
            with self.subTest(relative=relative):
                self.assertEqual(stage.qt_module_for_source(prefix / relative, prefix), module)
        self.assertIsNone(stage.qt_module_for_source(pathlib.Path("/usr/lib/libQt6Core.so"), prefix))
        self.assertIsNone(stage.qt_module_for_source(prefix / "lib/libQt6Unexpected.so.6", prefix))
        self.assertIsNone(stage.qt_module_for_source(prefix / "plugins/unexpected/libplugin.so", prefix))
        self.assertIsNone(stage.qt_module_for_source(prefix / "qml/Unknown/libplugin.so", prefix))

    def test_ci_exports_architecture_specific_runtime_libraries_for_spawned_app(self) -> None:
        self.assertIn(
            "LD_LIBRARY_PATH=%s/lib:%s/lib/x86_64-linux-gnu:%s/lib",
            self.workflow,
        )

    def test_ci_stages_dlopen_runtime_libraries_for_clean_launch(self) -> None:
        self.assertIn(
            "PERIGEE_EXTRA_RUNTIME_LIBRARIES: ${{ github.workspace }}/dep_root/lib/libSDL3.so.0:/usr/lib/x86_64-linux-gnu/libssl.so.3:/usr/lib/x86_64-linux-gnu/libcrypto.so.3",
            self.workflow,
        )
        package_common = (SOURCE_ROOT / "scripts/lib/package_linux_common.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("PERIGEE_EXTRA_RUNTIME_LIBRARIES", package_common)
        self.assertIn("--extra-library", package_common)

    def test_reusable_workflow_job_env_avoids_runner_context(self) -> None:
        job_env = re.search(r"(?ms)^    env:\n(?P<body>.*?)(?=^\s{4}steps:)", self.workflow)
        self.assertIsNotNone(job_env)
        assert job_env is not None
        self.assertNotIn("${{ runner.", job_env.group("body"))
        for variable in ("AQT_VENV", "PYTHON_WHEEL_DIR", "QT_DIR", "QT_ROOT"):
            self.assertRegex(
                job_env.group("body"),
                re.compile(rf"^      {variable}: \$\{{\{{ github\.workspace \}}\}}/", re.MULTILINE),
            )

    def test_ci_installs_and_gates_a_pinned_supported_meson(self) -> None:
        self.assertIn("MESON_VERSION: 1.6.1", self.workflow)
        self.assertIn('meson=="$MESON_VERSION"', self.workflow)
        self.assertIn('test "$(command -v meson)" = "$AQT_VENV/bin/meson"', self.workflow)
        self.assertIn('actual_meson_version="$(meson --version)"', self.workflow)
        self.assertIn('test "$actual_meson_version" = "$MESON_VERSION"', self.workflow)
        self.assertLess(
            self.workflow.index("Validate pinned Meson version"),
            self.workflow.index("Fingerprint dependency toolchain"),
        )

        apt_block = re.search(
            r"- name: Install build prerequisites(?P<body>.*?)(?=\n\s+- name:)",
            self.workflow,
            re.S,
        )
        self.assertIsNotNone(apt_block)
        assert apt_block is not None
        apt_install = apt_block.group("body").split("python3 -m venv", 1)[0]
        self.assertNotRegex(apt_install, r"\bmeson\b")

    def test_ci_uses_reviewed_vulkan_headers_for_video_decode(self) -> None:
        self.assertIn("repository: KhronosGroup/Vulkan-Headers", self.workflow)
        self.assertIn("ref: 409c16be502e39fe70dd6fe2d9ad4842ef2c9a53", self.workflow)
        self.assertRegex(
            self.workflow,
            r"for source in[^\n]*Vulkan-Headers",
        )
        self.assertIn("CPATH: ${{ github.workspace }}/dep_root/include", self.workflow)
        self.assertIn("Install pinned Vulkan headers", self.workflow)
        self.assertIn('cmake --install build', self.workflow)
        self.assertIn("Validate pinned Vulkan headers", self.workflow)
        self.assertIn("VK_HEADER_VERSION", self.workflow)
        self.assertIn("VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME", self.workflow)
        self.assertIn('test "$actual_header_version" = 313', self.workflow)
        self.assertLess(
            self.workflow.index("Validate pinned Vulkan headers"),
            self.workflow.index("Build libplacebo"),
        )
        self.assertLess(
            self.workflow.index("Validate pinned Vulkan headers"),
            self.workflow.index("Build FFmpeg"),
        )
        self.assertIn("libvulkan-dev", self.ubuntu_packages)
        self.assertNotIn("packages.lunarg.com", self.workflow)
        self.assertNotIn("vulkan-sdk", self.workflow.lower())

    def test_ci_qttest_uses_source_root_compatible_build_layout(self) -> None:
        self.assertIn("mkdir -p build-tests", self.workflow)
        self.assertIn("cd build-tests", self.workflow)
        self.assertIn("qmake6 ../moonlight-qt.pro", self.workflow)
        self.assertIn("./build-tests/tests/perigee-tests -silent", self.workflow)
        self.assertNotIn("build/ci-tests", self.workflow)

    def test_cache_contains_only_immutable_dependency_outputs(self) -> None:
        self.assertIn("uses: actions/cache@", self.workflow)
        self.assertRegex(self.workflow, r"(?m)^\s+path: dep_root$")
        self.assertIn("steps.toolchain.outputs.fingerprint", self.workflow)
        self.assertIn("steps.sources.outputs.fingerprint", self.workflow)
        self.assertIn("hashFiles('.github/workflows/build-appimage.yml'", self.workflow)
        for pin in (
            "c9ad296376ed6091e0fdec9844461fdf09af7845",
            "a883e490e30fb44a5336ea3dcb990c6982c5216f",
            "2d0979fb54e025e904c7372666fffbf5dae40f66",
        ):
            self.assertIn(pin, self.workflow)
        self.assertIn("https://libsdl.org/release/sdl2-compat-2.32.70.tar.gz", self.workflow)
        self.assertIn("998fa62557eb46ffe7e5c3e2c123bc332f7df9d9f593b3ceed88ed1158428a44", self.workflow)
        self.assertIn("sha256sum --check --strict", self.workflow)
        self.assertNotIn("repository: libsdl-org/sdl2-compat", self.workflow)
        for repository in ("intel/libva", "FFmpeg/FFmpeg"):
            block = re.search(
                rf"repository: {re.escape(repository)}\n\s+ref: (?P<ref>[^\n]+)",
                self.workflow,
            )
            self.assertIsNotNone(block)
            assert block is not None
            self.assertRegex(block.group("ref"), r"^[0-9a-f]{40}$")
        self.assertIn("710eb465c6277ee2cac3e6948767b01eebe7e77a", self.workflow)
        self.assertIn("239f2c733de417201d7ad3b3b8b0d9b63285b2b1", self.workflow)
        self.assertRegex(self.workflow, r"DAV1D_COMMIT: [0-9a-f]{40}")
        self.assertIn("54706fc6bc0cdecab7e9593974a4039cc038fca7", self.workflow)
        self.assertRegex(self.workflow, r'git(?: -C [^\n]+)? checkout --detach "\$DAV1D_COMMIT"')
        self.assertLess(self.workflow.index("Fingerprint dependency sources"), self.workflow.index("Cache immutable dependencies"))
        cache_block = re.search(r"- name: Cache immutable dependencies(?P<body>.*?)(?=\n\s+- name:)", self.workflow, re.S)
        self.assertIsNotNone(cache_block)
        assert cache_block is not None
        self.assertNotRegex(cache_block.group("body"), r"HOME|XDG|config|profile|certificate|clipboard|host")

        runtime_block = re.search(
            r"- name: Install pinned AppImage runtime(?P<body>.*?)(?=\n\s+- name:)",
            self.workflow,
            re.S,
        )
        self.assertIsNotNone(runtime_block)
        assert runtime_block is not None
        self.assertNotIn("working-directory: dep_root", runtime_block.group("body"))
        self.assertIn("$RUNNER_TEMP/runtime-x86_64", runtime_block.group("body"))

        for tool in (
            "aqt version",
            "gcc --version",
            "g++ --version",
            "ld --version",
            "nasm -v",
            "cmake --version",
            "meson --version",
            "ninja --version",
            "pkg-config --version",
            "pkg-config --modversion vulkan",
            "pkg-config --modversion glslang",
        ):
            self.assertIn(tool, self.workflow)
        self.assertIn("qtvirtualkeyboard (base archives: qtbase qtsvg qtdeclarative qtwayland)", self.workflow)

    def test_dependency_cache_fingerprints_all_installed_packages_and_pip_tools(self) -> None:
        self.assertIn(
            "dpkg-query -W -f='${Package}=${Version}\\n' | LC_ALL=C sort",
            self.workflow,
        )
        self.assertIn('"$AQT_VENV/bin/pip" freeze --all | LC_ALL=C sort', self.workflow)
        self.assertIn("--only-binary=:all: --dest", self.workflow)
        self.assertIn('--no-index --find-links "$PYTHON_WHEEL_DIR"', self.workflow)
        self.assertIn("sha256sum -- * | LC_ALL=C sort", self.workflow)
        self.assertNotIn('find "$AQT_VENV" -type f', self.workflow)
        self.assertIn("steps.toolchain.outputs.fingerprint", self.workflow)
        self.assertIn("steps.sources.outputs.fingerprint", self.workflow)
        self.assertIn("hashFiles('.github/workflows/build-appimage.yml'", self.workflow)

    def test_build_and_packaging_jobs_install_the_same_sorted_ubuntu_package_set(self) -> None:
        verifier_workflow = (
            SOURCE_ROOT / ".github/workflows/perigee-ci.yml"
        ).read_text(encoding="utf-8")
        package_file = "scripts/ci/ubuntu-22.04-packages.txt"
        self.assertEqual(self.ubuntu_packages, sorted(set(self.ubuntu_packages)))
        self.assertIn(package_file, self.workflow)
        self.assertIn(package_file, verifier_workflow)
        self.assertIn("xargs -r sudo apt-get install --yes", self.workflow)
        self.assertIn("xargs -r sudo apt-get install --yes", verifier_workflow)

    def test_artifact_producer_verifies_both_candidates_before_upload(self) -> None:
        first_upload = self.workflow.index("- name: Upload candidate A AppImage")
        for candidate in ("a", "b"):
            command = (
                f'PERIGEE_ARTIFACT_DIR="$GITHUB_WORKSPACE/build/artifacts-{candidate}" '
                "scripts/verify-linux-artifacts.sh"
            )
            self.assertIn(command, self.workflow)
            self.assertLess(self.workflow.index(command), first_upload)

    def test_ci_uploads_and_matches_producer_dpkg_package_provenance(self) -> None:
        verifier_workflow = PIPELINE.read_text(encoding="utf-8")
        for candidate in ("a", "b"):
            for kind in ("AppImage", "Tar"):
                self.assertIn(
                    f"Perigee-Linux{kind}-{candidate}-dpkg-${{{{ env.CI_VERSION }}}}",
                    self.workflow,
                )
                self.assertIn(
                    f"Perigee-Linux{kind}-{candidate}-dpkg-${{{{ github.sha }}}}",
                    verifier_workflow,
                )
        self.assertIn(
            "reference=ci-provenance/appimage-a/perigee-dpkg-packages.txt",
            verifier_workflow,
        )
        self.assertIn('cmp --silent "$reference" "$producer"', verifier_workflow)
        self.assertIn("producer package provenance differs", verifier_workflow)
        self.assertNotIn("verifier-dpkg-packages.txt", verifier_workflow)

    def test_ci_runs_packaging_tests_before_artifact_production(self) -> None:
        pipeline = PIPELINE.read_text(encoding="utf-8")
        self.assertIn("packaging-tests:", pipeline)
        self.assertIn(
            "PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest discover \\\n"
            "            -s scripts/tests -p 'test_*.py'",
            pipeline,
        )
        self.assertRegex(
            pipeline,
            r"(?s)linux-artifacts:.*?needs: packaging-tests.*?job_kind: artifacts",
        )

    def test_ci_builds_compares_and_verifies_two_isolated_candidate_sets(self) -> None:
        pipeline = PIPELINE.read_text(encoding="utf-8")
        self.assertIn("for candidate in a b; do", self.workflow)
        self.assertIn('PERIGEE_BUILD_DIR="$build_dir"', self.workflow)
        self.assertIn('PERIGEE_STAGE_DIR="$tar_stage"', self.workflow)
        self.assertIn('PERIGEE_STAGE_DIR="$appimage_stage"', self.workflow)
        self.assertIn('PERIGEE_BINARY="$build_dir/app/perigee"', self.workflow)
        self.assertIn("scripts/build-linux-tar.sh", self.workflow)
        self.assertIn("scripts/build-appimage.sh", self.workflow)
        self.assertRegex(
            self.workflow,
            r"cmp --silent build/artifacts-a/Perigee-\$version-x86_64\.AppImage \\\n"
            r"\s+build/artifacts-b/Perigee-\$version-x86_64\.AppImage",
        )
        self.assertRegex(
            self.workflow,
            r"cmp --silent build/artifacts-a/Perigee-\$version-linux-x86_64\.tar\.zst \\\n"
            r"\s+build/artifacts-b/Perigee-\$version-linux-x86_64\.tar\.zst",
        )
        for candidate in ("a", "b"):
            for kind in ("AppImage", "Tar"):
                self.assertIn(
                    f"Perigee-Linux{kind}-{candidate}-${{{{ env.CI_VERSION }}}}",
                    self.workflow,
                )
                self.assertIn(
                    f"Perigee-Linux{kind}-{candidate}-${{{{ github.sha }}}}",
                    pipeline,
                )
            self.assertIn(
                f'PERIGEE_ARTIFACT_DIR="$GITHUB_WORKSPACE/build/artifacts-{candidate}" '
                "scripts/verify-linux-artifacts.sh",
                self.workflow,
            )
        self.assertIn("Compare downloaded candidate bytes", pipeline)

    def test_reviewed_license_catalog_is_external_and_deterministic(self) -> None:
        self.assertTrue(LICENSE_CATALOG_PATH.is_file())
        catalog = LICENSE_CATALOG_PATH.read_text(encoding="utf-8")
        entries = [line for line in catalog.splitlines() if line and not line.startswith("#")]
        self.assertTrue(entries)
        self.assertEqual(entries, sorted(entries))
        self.assertTrue(LICENSE_CATALOG_GENERATOR.is_file())
        generator = LICENSE_CATALOG_GENERATOR.read_text(encoding="utf-8")
        verifier = VERIFY_PATH.read_text(encoding="utf-8")
        self.assertIn("reviewed_linux_license_digests.tsv", generator)
        self.assertIn("reviewed_linux_license_digests.tsv", verifier)
        self.assertIn("reviewed license catalog", verifier)

    def test_rpm_license_discovery_never_walks_runtime_dependencies(self) -> None:
        stager = STAGE_PATH.read_text(encoding="utf-8")
        self.assertIn('"-ql", package', stager)
        self.assertIn('"%{SOURCERPM}"', stager)
        self.assertNotIn('"--requires"', stager)
        self.assertNotIn("rpm_declared_license_files", stager)

    def test_source_build_licenses_never_fall_back_to_system_packages(self) -> None:
        stager = load("stage_source_license_test", STAGE_PATH)
        source = inspect.getsource(stager.existing_source_licenses)
        self.assertNotIn("/usr/share", source)
        with self.assertRaisesRegex(stager.PackagingError, "missing source-build"):
            stager.existing_source_licenses(pathlib.Path("/does/not/exist"), "ffmpeg")

    def test_dpkg_ownership_uses_the_discovered_merged_usr_path(self) -> None:
        stager = load("stage_merged_usr_test", STAGE_PATH)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            real_library = root / "usr/lib/libfixture.so.1"
            real_library.parent.mkdir(parents=True)
            real_library.write_bytes(b"fixture library\n")
            (root / "lib").symlink_to("usr/lib", target_is_directory=True)
            discovered_library = root / "lib/libfixture.so.1"
            payload_library = root / "payload/lib/libfixture.so.1"
            copyright_file = root / "usr/share/doc/fixture/copyright"
            copyright_file.parent.mkdir(parents=True)
            copyright_file.write_text("Fixture license\n", encoding="utf-8")

            def query(command: list[str], **_kwargs: object) -> mock.Mock:
                if command == ["dpkg-query", "-S", str(discovered_library.resolve())]:
                    return mock.Mock(
                        returncode=0,
                        stdout=f"fixture:amd64: {discovered_library.resolve()}\n",
                        stderr="",
                    )
                if command == ["dpkg-query", "-S", str(discovered_library)]:
                    return mock.Mock(
                        returncode=0,
                        stdout=f"fixture:amd64: {discovered_library}\n",
                        stderr="",
                    )
                if command == ["dpkg-query", "-L", "fixture:amd64"]:
                    return mock.Mock(
                        returncode=0,
                        stdout=f"{discovered_library.resolve()}\n{copyright_file}\n",
                        stderr="",
                    )
                if command == [
                    "dpkg-query",
                    "-W",
                    "-f=${Version}",
                    "fixture:amd64",
                ]:
                    return mock.Mock(returncode=0, stdout="1.0-1", stderr="")
                raise AssertionError(command)

            def which(command: str) -> str | None:
                return "/usr/bin/dpkg-query" if command == "dpkg-query" else None

            stager.COPIED_SOURCES.clear()
            stager.PACKAGE_LICENSE_CACHE.clear()
            with mock.patch.object(stager.shutil, "which", side_effect=which), mock.patch.object(
                stager.subprocess, "run", side_effect=query
            ):
                stager.copy_regular(discovered_library, payload_library)
                recorded_source = stager.COPIED_SOURCES[payload_library.resolve()]
                package = stager.package_license_files(recorded_source)

            self.assertIsNotNone(package)
            assert package is not None
            self.assertEqual(package.package, "fixture:amd64")
            self.assertEqual(package.library, discovered_library.resolve())
            self.assertEqual(package.licenses, (copyright_file,))

    def test_dpkg_ownership_preserves_legacy_lib_path(self) -> None:
        stager = load("stage_legacy_lib_test", STAGE_PATH)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            real_library = root / "usr/lib/libfixture.so.1"
            real_library.parent.mkdir(parents=True)
            real_library.write_bytes(b"fixture library\n")
            (root / "lib").symlink_to("usr/lib", target_is_directory=True)
            discovered_library = root / "lib/libfixture.so.1"
            payload_library = root / "payload/lib/libfixture.so.1"
            copyright_file = root / "usr/share/doc/fixture/copyright"
            copyright_file.parent.mkdir(parents=True)
            copyright_file.write_text("Fixture license\n", encoding="utf-8")

            def query(command: list[str], **_kwargs: object) -> mock.Mock:
                if command == ["dpkg-query", "-S", str(discovered_library.resolve())]:
                    return mock.Mock(returncode=1, stdout="", stderr="not found")
                if command == ["dpkg-query", "-S", str(discovered_library)]:
                    return mock.Mock(
                        returncode=0,
                        stdout=f"fixture:amd64: {discovered_library}\n",
                        stderr="",
                    )
                if command == ["dpkg-query", "-L", "fixture:amd64"]:
                    return mock.Mock(
                        returncode=0,
                        stdout=f"{discovered_library}\n{copyright_file}\n",
                        stderr="",
                    )
                if command == [
                    "dpkg-query",
                    "-W",
                    "-f=${Version}",
                    "fixture:amd64",
                ]:
                    return mock.Mock(returncode=0, stdout="1.0-1", stderr="")
                raise AssertionError(command)

            def which(command: str) -> str | None:
                return "/usr/bin/dpkg-query" if command == "dpkg-query" else None

            stager.COPIED_SOURCES.clear()
            stager.PACKAGE_LICENSE_CACHE.clear()
            with mock.patch.object(stager.shutil, "which", side_effect=which), mock.patch.object(
                stager.subprocess, "run", side_effect=query
            ):
                stager.copy_regular(discovered_library, payload_library)
                recorded_source = stager.COPIED_SOURCES[payload_library.resolve()]
                package = stager.package_license_files(recorded_source)

            self.assertIsNotNone(package)
            assert package is not None
            self.assertEqual(package.package, "fixture:amd64")
            self.assertEqual(package.library, discovered_library)
            self.assertEqual(package.licenses, (copyright_file,))

    def test_copy_regular_propagates_original_source_identity(self) -> None:
        stager = load("stage_copy_origin_test", STAGE_PATH)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            original = root / "system/libfixture.so.1"
            original.parent.mkdir(parents=True)
            original.write_bytes(b"fixture library\n")
            first = root / "payload/lib/libfixture.so.1"
            second = root / "payload/lib/libfixture-copy.so.1"

            stager.COPIED_SOURCES.clear()
            stager.copy_regular(original, first)
            stager.copy_regular(first, second)

            self.assertEqual(stager.COPIED_SOURCES[first.resolve()], original)
            self.assertEqual(stager.COPIED_SOURCES[second.resolve()], original)

    def test_extra_runtime_libraries_are_staged_before_dependency_walk(self) -> None:
        stager = load("stage_extra_runtime_library_test", STAGE_PATH)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "dep_root/lib/libSDL3.so.0"
            source.parent.mkdir(parents=True)
            source.write_bytes(b"SDL3 fixture\n")
            prefix = root / "stage/usr"

            stager.COPIED_SOURCES.clear()
            stager.collect_extra_libraries(prefix, [source])

            staged = prefix / "lib/libSDL3.so.0"
            self.assertEqual(staged.read_bytes(), source.read_bytes())
            self.assertEqual(stager.COPIED_SOURCES[staged.resolve()], source)

    def test_elf_path_scrubber_preserves_length_and_removes_host_prefixes(self) -> None:
        stager = load("stage_linux_payload_path_scrubber", STAGE_PATH)
        payload = b"prefix /home/runner/work/perigee /tmp/perigee-build suffix"
        scrubbed = stager.scrub_absolute_build_paths(payload)
        self.assertEqual(len(scrubbed), len(payload))
        self.assertNotIn(b"/home/", scrubbed)
        self.assertIn(b"/src_/runner/work/perigee", scrubbed)
        self.assertIn(b"/tmp/perigee-build", scrubbed)

    def test_dpkg_license_discovery_uses_same_source_sibling(self) -> None:
        stager = load("stage_dpkg_source_license_test", STAGE_PATH)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            library = root / "usr/lib/libfixture.so.1"
            library.parent.mkdir(parents=True)
            library.write_bytes(b"fixture library\n")
            copyright_file = root / "usr/share/doc/fixture/copyright"
            copyright_file.parent.mkdir(parents=True)
            copyright_file.write_text("Fixture license\n", encoding="utf-8")

            def query(command: list[str], **_kwargs: object) -> mock.Mock:
                if command == ["dpkg-query", "-S", str(library)]:
                    return mock.Mock(
                        returncode=0,
                        stdout=f"libfixture:amd64: {library}\n",
                        stderr="",
                    )
                if command == ["dpkg-query", "-L", "libfixture:amd64"]:
                    return mock.Mock(returncode=0, stdout=f"{library}\n", stderr="")
                if command == [
                    "dpkg-query",
                    "-W",
                    "-f=${source:Package}\t${source:Version}",
                    "libfixture:amd64",
                ]:
                    return mock.Mock(returncode=0, stdout="fixture-source\t1.0-1", stderr="")
                if command == [
                    "dpkg-query",
                    "-W",
                    "-f=${binary:Package}\t${source:Package}\t${source:Version}\n",
                ]:
                    return mock.Mock(
                        returncode=0,
                        stdout=(
                            "libfixture:amd64\tfixture-source\t1.0-1\n"
                            "fixture-license:all\tfixture-source\t1.0-1\n"
                        ),
                        stderr="",
                    )
                if command == ["dpkg-query", "-L", "fixture-license:all"]:
                    return mock.Mock(returncode=0, stdout=f"{copyright_file}\n", stderr="")
                if command == [
                    "dpkg-query",
                    "-W",
                    "-f=${Version}",
                    "libfixture:amd64",
                ]:
                    return mock.Mock(returncode=0, stdout="1.0-1", stderr="")
                raise AssertionError(command)

            def which(command: str) -> str | None:
                return "/usr/bin/dpkg-query" if command == "dpkg-query" else None

            stager.PACKAGE_LICENSE_CACHE.clear()
            stager.DPKG_LICENSE_CACHE.clear()
            stager.DPKG_SOURCE_CACHE.clear()
            with mock.patch.object(stager.shutil, "which", side_effect=which), mock.patch.object(
                stager.subprocess, "run", side_effect=query
            ):
                package = stager.package_license_files(library)

            self.assertIsNotNone(package)
            assert package is not None
            self.assertEqual(package.package, "libfixture:amd64")
            self.assertEqual(package.licenses, (copyright_file,))

    def test_stager_and_verifier_share_the_graphics_policy(self) -> None:
        stage = load("stage_policy_test", STAGE_PATH)
        verify = load("verify_policy_test", VERIFY_PATH)
        self.assertEqual(stage.HOST_GRAPHICS_AND_WAYLAND_PREFIXES, verify.HOST_GRAPHICS_AND_WAYLAND_PREFIXES)
        expected = {
            "libwayland-client.so",
            "libwayland-egl.so",
            "libwayland-cursor.so",
            "libEGL.so",
            "libGL.so",
            "libGLES",
            "libGLX.so",
            "libOpenGL.so",
            "libGLdispatch.so",
            "libgbm.so",
            "libEGL_mesa.so",
            "libGLX_mesa.so",
            "libvulkan.so",
            "libva.so",
            "libdrm.so",
            "libX11.so",
            "libX11-xcb.so",
            "libXext.so",
            "libXau.so",
            "libXdmcp.so",
            "libxcb",
            "libstdc++.so",
            "libgcc_s.so",
        }
        self.assertTrue(expected.issubset(set(stage.HOST_GRAPHICS_AND_WAYLAND_PREFIXES)))

    def test_source_component_license_set_is_complete(self) -> None:
        stage = load("stage_license_test", STAGE_PATH)
        expected = {
            "moonlight-common",
            "enet",
            "nanors",
            "qmdnsengine",
            "h264bitstream",
            "sdl-controller-db",
        }
        self.assertTrue(expected.issubset(set(stage.SOURCE_COMPONENT_LICENSES)))

    def test_verifier_targets_the_whole_appimage(self) -> None:
        wrapper = (SOURCE_ROOT / "scripts/verify-linux-artifacts.sh").read_text(encoding="utf-8")
        self.assertIn('verify-version "$APPIMAGE"', wrapper)
        self.assertRegex(wrapper, r'run_launch_gate \\\n\s+"\$APPIMAGE"')
        self.assertIn('python3 -B "$PYTHON_HELPER" launch-gate', wrapper)
        self.assertIn('"$APPIMAGE_ROOT/usr/bin/perigee"', wrapper)
        self.assertIn('"$TAR_ROOT/bin/perigee" "$VERIFY_ROOT/launch-tar" "$TAR_ROOT/bin/perigee"', wrapper)
        self.assertIn('tail -n 80 -- "$work_root/launch.log"', wrapper)
        self.assertNotIn('verify-version "$APPIMAGE_ROOT/AppRun"', wrapper)
        self.assertNotIn('launch-gate "$APPIMAGE_ROOT/AppRun"', wrapper)

    def test_packaging_python_entrypoints_disable_bytecode_writes(self) -> None:
        for relative in (
            "scripts/build-appimage.sh",
            "scripts/lib/package_linux_common.sh",
            "scripts/verify-linux-artifacts.sh",
        ):
            with self.subTest(path=relative):
                script = (SOURCE_ROOT / relative).read_text(encoding="utf-8")
                python_invocations = [
                    line.strip()
                    for line in script.splitlines()
                    if line.strip().startswith("python3 ")
                ]
                self.assertTrue(python_invocations)
                self.assertTrue(
                    all(line.startswith("python3 -B ") for line in python_invocations),
                    python_invocations,
                )


if __name__ == "__main__":
    unittest.main(verbosity=2)

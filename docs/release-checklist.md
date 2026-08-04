# Linux release checklist

Use this checklist for an x86_64 Linux release candidate. Stop the release when a required gate fails.

## 1. Confirm the source

- Use a clean, reviewed commit.
- Confirm that `app/version.txt` contains the intended version.
- Confirm that the submodules match the reviewed commit.
- Record the commit ID and `SOURCE_DATE_EPOCH` in the release record.

```sh
git status --short
git submodule status --recursive
git rev-parse HEAD
git log -1 --format=%ct
qmake6 -query QT_VERSION
```

Perigee requires Qt 6.7 or newer. CI uses Qt 6.8.3 from the pinned aqt module set. Stop if `qmake6` resolves to another Qt installation.

CI uses Meson 1.6.1 from its pinned Python environment. Do not allow the Ubuntu 22.04 system Meson to satisfy the build. The pinned libplacebo source requires Meson 0.63 or newer. Its CI build uses glslang and disables shaderc.

CI uses Khronos Vulkan-Headers from commit `409c16be502e39fe70dd6fe2d9ad4842ef2c9a53`. The compiler must report `VK_HEADER_VERSION` 313 and must define `VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME` before libplacebo or FFmpeg builds. Ubuntu `libvulkan-dev` supplies the loader integration, not the reviewed video-decode headers.

Confirm that the reviewed license catalog covers the exact producer profile. The Ubuntu 22.04 plus aqt Qt 6.8.3 profile is open. Stop an Ubuntu or CI release until its actual cold-produced dependency license set is reviewed and added.

The Ubuntu CI producer uploads its complete `dpkg-query` package/version manifest with each AppImage and Linux tar artifact. The verifier job installs the same sorted package manifest and compares its own manifest with both producer manifests before it verifies the artifacts. A package-set mismatch is a release failure.

The stager records package-managed library provenance in `system-provenance.tsv`. The verifier checks the library package and license-owner package independently. It requires exact package versions, package listings, source paths, source bytes, and license bytes. A notice from another package is accepted only when both packages have the exact same source identity (source RPM on RPM systems, or source package and version on dpkg systems). The stager does not walk runtime dependencies and does not borrow a notice from a package with a different source identity.

The current Nobara development host cannot make a release candidate. Its installed `libdrm.x86_64`, `numactl-libs.x86_64`, `xcb-util-keysyms.x86_64`, and `zlib-ng-compat.x86_64` packages have no package-owned or same-source-RPM license notice. The build must fail closed on those libraries. Do not use a generic license template or an unrelated package notice to bypass this gate.

Source-built dependency notices must come from the exact pinned source checkout. Do not use a system package notice when a required `deps/...` license file is absent.

To make a deterministic catalog candidate from an extracted payload, run:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -B \
  scripts/lib/generate_linux_license_catalog.py \
  /path/to/extracted-payload --layout tar \
  --profile 'exact producer description'
```

Review every changed component identity, file, and legal text. Do not approve a catalog only because its digest manifest is internally consistent.

## 2. Run the automated tests

Run the complete QtTest suite. For version 0.1.0, the expected count is 612 passes and zero failures.

```sh
xvfb-run -a -s '-screen 0 1280x720x24' \
  env LIBGL_ALWAYS_SOFTWARE=1 QT_QPA_PLATFORM=xcb SDL_VIDEODRIVER=dummy \
  ./build-tests/tests/perigee-tests -silent
```

Run the standard Sunshine fixture suite. Do not connect this gate to a live host.

```sh
env QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  QML_DISABLE_DISK_CACHE=1 SDL_VIDEODRIVER=dummy \
  ./build-tests/tests/perigee-tests PolarisAdapterTest -silent
```

Record the pass, failure, and skip counts.

## 3. Build two candidates

Use the reviewed AppImage runtime. Set `PERIGEE_APPIMAGE_RUNTIME` to its local path.

Build candidate A. The tar build creates the release binary. The AppImage build uses the same binary.

```sh
PERIGEE_BUILD_DIR="$PWD/build/release-a" \
PERIGEE_STAGE_DIR="$PWD/build/stage-tar-a" \
PERIGEE_OUTPUT_DIR="$PWD/build/artifacts-a" \
scripts/build-linux-tar.sh

PERIGEE_APPIMAGE_RUNTIME=/path/to/reviewed/runtime-x86_64 \
PERIGEE_BINARY="$PWD/build/release-a/app/perigee" \
PERIGEE_BUILD_DIR="$PWD/build/release-a" \
PERIGEE_STAGE_DIR="$PWD/build/stage-appimage-a" \
PERIGEE_OUTPUT_DIR="$PWD/build/artifacts-a" \
scripts/build-appimage.sh
```

Build candidate B from new build and stage directories.

```sh
PERIGEE_BUILD_DIR="$PWD/build/release-b" \
PERIGEE_STAGE_DIR="$PWD/build/stage-tar-b" \
PERIGEE_OUTPUT_DIR="$PWD/build/artifacts-b" \
scripts/build-linux-tar.sh

PERIGEE_APPIMAGE_RUNTIME=/path/to/reviewed/runtime-x86_64 \
PERIGEE_BINARY="$PWD/build/release-b/app/perigee" \
PERIGEE_BUILD_DIR="$PWD/build/release-b" \
PERIGEE_STAGE_DIR="$PWD/build/stage-appimage-b" \
PERIGEE_OUTPUT_DIR="$PWD/build/artifacts-b" \
scripts/build-appimage.sh
```

## 4. Prove reproducibility

Use these exact artifact names for version 0.1.0:

- `Perigee-0.1.0-x86_64.AppImage`
- `Perigee-0.1.0-linux-x86_64.tar.zst`

Record the size and SHA-256 value for each candidate. Require the candidate A and B files to be byte-identical.

```sh
sha256sum build/artifacts-a/Perigee-0.1.0-x86_64.AppImage
sha256sum build/artifacts-b/Perigee-0.1.0-x86_64.AppImage
sha256sum build/artifacts-a/Perigee-0.1.0-linux-x86_64.tar.zst
sha256sum build/artifacts-b/Perigee-0.1.0-linux-x86_64.tar.zst
cmp build/artifacts-a/Perigee-0.1.0-x86_64.AppImage \
  build/artifacts-b/Perigee-0.1.0-x86_64.AppImage
cmp build/artifacts-a/Perigee-0.1.0-linux-x86_64.tar.zst \
  build/artifacts-b/Perigee-0.1.0-linux-x86_64.tar.zst
```

Release record:

| Item | Candidate A | Candidate B |
| --- | --- | --- |
| AppImage size | Not recorded | Not recorded |
| AppImage SHA-256 | Not recorded | Not recorded |
| Linux tar size | Not recorded | Not recorded |
| Linux tar SHA-256 | Not recorded | Not recorded |

## 5. Verify the artifacts

Run the verifier once for each candidate directory. Do not set `PERIGEE_SKIP_WAYLAND_LAUNCH` for a release gate.

```sh
PERIGEE_ARTIFACT_DIR="$PWD/build/artifacts-a" scripts/verify-linux-artifacts.sh
PERIGEE_ARTIFACT_DIR="$PWD/build/artifacts-b" scripts/verify-linux-artifacts.sh
```

The verifier must complete these gates:

- secure archive extraction;
- exact layout, numeric root ownership, mode, metadata, and icon checks;
- exact `Perigee 0.1.0` version output from the whole AppImage and tar entry point;
- complete ELF dependency closure and the host-runtime allowlist;
- the exact deterministic license mapping set for every packaged ELF and statically linked source component;
- exact license-tree digest coverage with no unused or ambiguous component;
- exact dependency license bytes against the external reviewed component/path catalog;
- private-library and host graphics and Wayland checks;
- rejection of every payload symlink and any non-exact AppImage entry point;
- private-key, host-path, build-path, and test-fixture scans;
- rejection of unexpected PAX keys, GNU sparse metadata, hidden owner names, and SquashFS xattrs;
- isolated KWin Wayland startup with temporary profile directories;
- liveness after the startup delay and after socket inspection; and
- a process-owned TCP listening-socket check.

The environment must permit private D-Bus and Wayland Unix sockets. Treat a blocked compositor or socket gate as a failure, not as a pass.

Record the verifier output. Also record `desktop-file-validate`, `appstreamcli`, and `xmllint` results from the verifier.

## 6. Confirm the platform limits

- The current Linux artifacts support x86_64 only.
- The artifacts use host Wayland and host graphics libraries.
- The artifacts do not include a second Mesa stack.
- Native Windows and macOS package qualification is separate.
- A local borrowed AppImage runtime is not a release input.

CI uses the official AppImage type-2 runtime URL. The reviewed build commit is `75849dc`. The pinned SHA-256 value is `1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf`.

The digest is the trust anchor because the `continuous` URL can change.

To update the runtime pin:

1. Open the official AppImage type-2 runtime release page.
2. Record the published build commit and SHA-256 value.
3. Update both defaults in `.github/workflows/build-appimage.yml`.
4. Review the change as a supply-chain change.
5. Repeat the two-build and artifact-verification gates.

## 7. Keep Task 20 acceptance open

Do not mark these items complete in Task 19:

- [ ] Live Sunshine connection and stream acceptance
- [ ] Live Polaris capability and display acceptance
- [ ] Multi-monitor switch acceptance
- [ ] Controller hardware navigation acceptance
- [ ] Floating-menu in-stream acceptance
- [ ] Public artifact publication

Record the host type, server version, display topology, controller type, and result during Task 20. Do not copy a live profile into test data.

Task 20 scaffolding is in `scripts/acceptance/`. Environment collection is local and read-only by default. Deck cycle testing is a dry run by default and requires both `--live` and `PERIGEE_ACCEPT_LIVE_TESTS=YES` before it invokes an audited driver. Use `docs/testing/live-acceptance-2026-08.md` as the redacted acceptance ledger. The ledger records a passed Task 19 artifact gate and a partial Polaris control-plane probe; the unchecked live stream, input, display, controller, and publication gates remain open.

Use the Task 20 harness and publishable ledger:

```sh
scripts/acceptance/collect-environment.sh
scripts/acceptance/deck-cycle-test.sh
```

The default commands are read-only and do not contact a host. Live Deck cycles require both `--live` and `PERIGEE_ACCEPT_LIVE_TESTS=YES`. A paired permission summary requires both `--include-paired-summary` and `PERIGEE_ACCEPT_SENSITIVE_COLLECTION=YES`. Keep exact 10G endpoint, route, host identity, and rollback evidence in a local mode-0600 file. Publish only the redacted rows in [`testing/live-acceptance-2026-08.md`](testing/live-acceptance-2026-08.md).

## 8. Roll back or remove a candidate

If a gate fails, stop publication. Keep the failed hashes in the release record and mark them as rejected.

Remove only the task-owned local candidates:

```sh
rm -- build/artifacts-a/Perigee-0.1.0-x86_64.AppImage
rm -- build/artifacts-a/Perigee-0.1.0-linux-x86_64.tar.zst
rm -- build/artifacts-b/Perigee-0.1.0-x86_64.AppImage
rm -- build/artifacts-b/Perigee-0.1.0-linux-x86_64.tar.zst
```

If Task 20 later publishes a rejected candidate, remove its release assets. Do not reuse the rejected hashes or tag for a replacement build.

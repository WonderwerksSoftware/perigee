# Upstream pins

This repository keeps the Perigee client and its companion Polaris host on
explicit, reviewable upstream commits. Update a pin only after reviewing the
intervening history and repeating the baseline checks below.

## Pinned revisions

| Component | Fork / local checkout | Upstream | Pinned revision | Version |
| --- | --- | --- | --- | --- |
| Perigee client | `WonderwerksSoftware/perigee` | `moonlight-stream/moonlight-qt` | `546cb72e32e5ac04bbc7e0b3a254176e5696685a` | Moonlight `6.1.0` |
| Polaris host | `WonderwerksSoftware/polaris`; sibling checkout `/home/wcfox/Documents/Codex/perigee-polaris` | `papi-ux/polaris` | `593754652dae557c3642590be2ffdef6f83b8647` | Polaris `1.3.4.5937546` |

The Moonlight revision entered Perigee through merge commit
`1cfaca6774134290cc3b3a9e0bddf02d683b23be`, preserving both histories. The
Polaris checkout uses branch `perigee-client-control` directly at its pinned
revision.

## Remote roles

- Perigee `origin`: authenticated HTTPS write remote for the WonderWerks fork.
- Perigee `moonlight-upstream`: fetch-only-by-policy source of Moonlight Qt
  updates. Never push project work to it.
- Polaris `origin`: authenticated HTTPS write remote for the WonderWerks fork.
- Polaris `polaris-upstream`: fetch-only-by-policy source of Polaris updates.
  Never push project work to it.

HTTPS is used because this workstation does not have a GitHub-authorized SSH
key. GitHub CLI's credential helper supplies fork authentication without
embedding a token in either remote URL.

## Intentional upstream synchronization

Review and merge an explicit commit rather than silently following a moving
branch:

```bash
git fetch moonlight-upstream master --tags
git log --oneline 546cb72e32e5ac04bbc7e0b3a254176e5696685a..moonlight-upstream/master
git merge --no-ff <reviewed-moonlight-commit>
git submodule update --init --recursive
```

For the companion host:

```bash
git -C /home/wcfox/Documents/Codex/perigee-polaris fetch polaris-upstream master --tags
git -C /home/wcfox/Documents/Codex/perigee-polaris log --oneline 593754652dae557c3642590be2ffdef6f83b8647..polaris-upstream/master
git -C /home/wcfox/Documents/Codex/perigee-polaris merge --no-ff <reviewed-polaris-commit>
git -C /home/wcfox/Documents/Codex/perigee-polaris submodule update --init --recursive
```

After either update, record the new full commit ID and rerun the applicable
build and tests before committing the pin change.

## Baseline verification (2026-08-02)

Moonlight Qt was configured with qmake `3.1` / Qt `6.11.1` and built in debug
mode:

```bash
mkdir -p build-baseline
cd build-baseline
qmake6 ../moonlight-qt.pro CONFIG+=debug
CCACHE_DIR="$PWD/.ccache" make -j"$(nproc)" debug
QT_QPA_PLATFORM=offscreen ./app/moonlight --version
```

Result: build passed and the version command printed `Moonlight 6.1.0`. The
build-local cache avoids this sandbox's read-only user ccache directory. The
offscreen Qt backend is needed because the baseline shell cannot acquire a DRM
device; a traced version invocation made no `bind()` or `listen()` calls.

Polaris was configured without CUDA on this AMD host, then the requested fast
test target was built and filtered:

```bash
CCACHE_DIR=/home/wcfox/Documents/Codex/perigee-polaris/build-tests/.ccache \
  cmake -S /home/wcfox/Documents/Codex/perigee-polaris \
  -B /home/wcfox/Documents/Codex/perigee-polaris/build-tests \
  -DBUILD_TESTS=ON -DBUILD_FULL_TESTS=OFF -DCMAKE_BUILD_TYPE=Debug \
  -DPOLARIS_ENABLE_CUDA=OFF
CCACHE_DIR=/home/wcfox/Documents/Codex/perigee-polaris/build-tests/.ccache \
  cmake --build /home/wcfox/Documents/Codex/perigee-polaris/build-tests \
  --target test_polaris -j2
/home/wcfox/Documents/Codex/perigee-polaris/build-tests/tests/test_polaris \
  --gtest_filter=ClientSettingsAdvertisementTests.*
```

Result: Polaris configured as `1.3.4.5937546`, `test_polaris` built, and both
filtered tests passed (`2/2`).

## GPL and attribution rules

- Keep the preserved upstream histories, GPLv3 license text, copyright notices,
  author notices, and no-warranty notices intact.
- Mark distributed modifications prominently, including their dates, and do not
  represent Perigee or its Polaris companion changes as upstream releases.
- When distributing binaries or object code, provide the complete corresponding
  source under GPLv3 using a license-compliant method, including the scripts and
  source needed to build and install the covered work.
- Preserve the separate licenses and notices of bundled dependencies and
  submodules; GPLv3 coverage does not erase their attribution requirements.
- Attribution must not imply endorsement by Moonlight, Polaris, or their
  contributors. Branding and trademark permission are separate from the source
  license.

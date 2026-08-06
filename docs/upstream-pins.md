# Upstream pins

This file records the upstream revisions that Perigee uses for code and contract reviews.

Perigee does not require a modified host. The Polaris revision is a source reference, not a client dependency.

## Pinned revisions

| Component | Upstream | Revision | Purpose |
|---|---|---|---|
| Perigee client base | `moonlight-stream/moonlight-qt` | `546cb72e32e5ac04bbc7e0b3a254176e5696685a` | Moonlight 6.1.0 streaming base |
| Polaris contract review | `papi-ux/polaris` | `73014f91dc64b5510fec84f2f7eaf9f67065dc94` | Review of official advertised client controls |

Moonlight entered Perigee through merge commit `1cfaca6774134290cc3b3a9e0bddf02d683b23be`. The merge keeps both Git histories.

The Polaris revision does not enter the Perigee source tree. It records the official source that was used for the API contract review.

## Remote roles

- Perigee `origin` is the write remote for the WonderWerksSoftware fork.
- Perigee `moonlight-upstream` is the review source for Moonlight Qt updates.
- Do not push Perigee work to `moonlight-upstream`.

The Perigee remote uses authenticated HTTPS. The remote URL does not contain a token.

## Moonlight update procedure

Review one explicit Moonlight commit. Do not follow a moving branch without review.

```bash
git fetch moonlight-upstream master --tags
git log --oneline 546cb72e32e5ac04bbc7e0b3a254176e5696685a..moonlight-upstream/master
git merge --no-ff <reviewed-moonlight-commit>
git submodule update --init --recursive
```

After the merge, record the new full commit ID. Run the complete build and test gates before you commit the pin change.

## Polaris contract review

Perigee uses only official capabilities that a connected Polaris host advertises. The client does not select behavior from a Polaris version string.

The review at `73014f91dc64b5510fec84f2f7eaf9f67065dc94` covered these client contracts:

- Capability discovery
- Paired-client permissions
- Session status
- Client settings
- Live session bitrate
- Adaptive bitrate and AI Auto Quality control
- Quality state and tuning readback
- Named commands
- Text clipboard transfer
- Host-session stop

Physical display selection is not part of the Polaris API contract. Perigee sends the standard GameStream keyboard shortcut through the stream input channel.

Before you change a Polaris request, review the current official source. Update the review revision and the contract fixtures in the same change.

## Moonlight baseline verification

The baseline used qmake 3.1 and Qt 6.11.1. The debug build passed.

```bash
mkdir -p build-baseline
cd build-baseline
qmake6 ../moonlight-qt.pro CONFIG+=debug
CCACHE_DIR="$PWD/.ccache" make -j"$(nproc)" debug
QT_QPA_PLATFORM=offscreen ./app/moonlight --version
```

The version command printed `Moonlight 6.1.0`. The build-local cache prevents writes to the user cache.

The offscreen Qt backend is necessary when the shell cannot use a Direct Rendering Manager (DRM) device.

## GPL and attribution rules

- Keep the upstream Git history, GPLv3 license text, copyright notices, author notices, and no-warranty notices.
- Identify distributed Perigee modifications and their dates. Do not identify Perigee as an upstream Moonlight release.
- When you distribute binaries, provide the complete corresponding source with a GPLv3-compliant method.
- Include the scripts and source that are necessary to build and install the covered work.
- Keep the separate licenses and notices for dependencies and submodules.
- Do not imply endorsement by Moonlight, Polaris, or their contributors.

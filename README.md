# Perigee

<img src="app/res/perigee.svg" alt="Perigee orbital logo" width="128">

Moonlight, brought closer.

Perigee is a remote gaming and workstation client for Linux. It uses the current Moonlight Qt streaming core.

Perigee adds an in-stream control menu named Perigee Deck. Open Deck with one shortcut or one controller chord.

Version 0.1.0 is under development. It is not a production release.

## Purpose

Perigee reduces the number of shortcuts that users must remember during a stream. Deck provides one searchable menu for session controls.

Polaris is the primary host for enhanced controls. Standard Sunshine hosts keep the normal GameStream-compatible streaming path.

## Implemented features

- Open Deck from a keyboard or controller. Use a keyboard, mouse, or controller inside Deck.
- Search actions in the Display, Input, Clipboard, Stats, Window, and Session categories.
- Navigate all core actions without text input.
- Show current values, progress, disabled reasons, confirmations, and verified results.
- Keep remote input neutral while Deck owns keyboard and controller input.
- Restore the intended input-capture state when Deck closes.
- Control mouse capture, keyboard capture, statistics, and window mode locally.
- Disconnect the client or quit Perigee without ending the host session.
- Use authenticated Polaris actions for named commands, text clipboard transfer, host-session control, and display selection.
- Verify Polaris display changes with state readback and a decoded frame.
- Attempt one rollback when a display change fails after mutation.
- Show controller glyphs for Xbox, PlayStation, Nintendo, and Steam Deck layouts.
- Preserve ordinary Moonlight shortcuts and controller input while Deck is closed.

Automated tests cover these features with local fixtures and a fake Polaris service. The cold Linux artifact gate is green. The Standard Sunshine path skips Polaris-only discovery, and Deck pointer targets retain clicks during list scrolling and model updates. A bounded Standard Sunshine stream smoke passed on the authorized staging path, including H.264 decode and first video/audio packets. Live keyboard, mouse, controller, display, recovery, and full Polaris stream acceptance remain open.

See the [live acceptance ledger](docs/testing/live-acceptance-2026-08.md) for the current release gate. It contains redacted results only. Exact host evidence stays in an ignored local file.

## Current limitations

- Perigee can stream only one active host display at a time.
- Perigee does not open concurrent displays in separate client windows.
- Named commands are server-advertised actions. Perigee does not provide an arbitrary shell.
- Clipboard transfer supports UTF-8 text only. The absolute client limit is 1 mebibyte (MiB).
- Polaris display and command controls require the companion paired-client control endpoints.
- The automatic Moonlight update feed is disabled. Perigee 0.1.0 has no replacement update feed.
- Perigee release artifacts do not exist yet. Build the current source for development use.
- Live Polaris, controller, multi-display, and KDE Wayland acceptance is pending. Standard Sunshine launch and bounded video/audio smoke have been verified; live Deck input acceptance is still pending.
- Windows and macOS packaging inputs use the Perigee identity. Native release qualification is pending.
- Flatpak packaging is not part of version 0.1.0.
- Existing translation catalogs have not received a complete Perigee terminology update.

## Architecture and compatibility

The Moonlight Qt core remains responsible for video, audio, pairing, transport, decoding, rendering, and remote input.

Perigee adds these focused layers:

- `ActionRegistry` supplies stable actions, capability checks, permissions, confirmation rules, and result state.
- `GameStreamAdapter` supplies local actions for standard Sunshine and GameStream-compatible sessions.
- `PolarisAdapter` adds authenticated capability discovery and enhanced host actions.
- Deck renders the user interface with Qt's QML declarative language and the existing stream overlay path.
- The input router neutralizes remote input and gives Deck temporary local ownership.

The first release target is Nobara Linux with KDE Plasma and Wayland. Perigee requires Qt 6.7 or newer.

The client keeps the inherited H.264, HEVC, AV1, high dynamic range (HDR), audio, gamepad, and remote-desktop paths.

Host and hardware support still apply.

## Installation

Perigee does not have a verified release package. Use a local development build from this repository.

Do not use Moonlight release packages as Perigee packages. Those packages have a different identity and settings namespace.

On first applicable start, Perigee can import recognized Moonlight Qt preferences and paired hosts. The default answer is **No**.

An accepted import copies only recognized streaming preferences and complete paired-host records. It also copies the required pairing identity for imported hosts.

The import does not copy Deck bindings, logs, crash data, cached artwork, temporary state, or unknown keys.

## Build

Install a C++17 compiler, Qt 6.7 or newer, qmake, and the required development libraries.

For Fedora or Nobara, use these upstream-derived package names:

```text
openssl-devel SDL2-devel SDL2_ttf-devel ffmpeg-devel libva-devel
libvdpau-devel opus-devel pulseaudio-libs-devel alsa-lib-devel
libdrm-devel qt6-qtsvg-devel qt6-qtdeclarative-devel
```

The FFmpeg development package can require RPM Fusion on Fedora-family systems.

For Debian or Ubuntu, use these upstream-derived package names:

```text
libegl1-mesa-dev libgl1-mesa-dev libopus-dev libsdl2-dev
libsdl2-ttf-dev libssl-dev libavcodec-dev libavformat-dev
libswscale-dev libva-dev libvdpau-dev libxkbcommon-dev
wayland-protocols libdrm-dev qt6-base-dev qt6-declarative-dev
libqt6svg6-dev qt6-wayland qml6-module-qtquick-controls
qml6-module-qtquick-templates qml6-module-qtquick-layouts
qml6-module-qtqml-workerscript qml6-module-qtquick-window
qml6-module-qtquick
```

Clone the source and its submodules:

```bash
git clone --recurse-submodules https://github.com/WonderwerksSoftware/perigee.git
cd perigee
```

If you already have the source, update its submodules:

```bash
git submodule update --init --recursive
```

Configure an out-of-source debug build:

```bash
mkdir -p build
cd build
qmake6 ../moonlight-qt.pro CONFIG+=debug
```

Build Perigee:

```bash
make -j"$(nproc)" debug
```

Check the built identity:

```bash
./app/perigee --version
```

The expected output is `Perigee 0.1.0`.

## Test

Install X virtual framebuffer (Xvfb) before you run the complete graphical suite.

Configure the test target from the build directory:

```bash
qmake6 ../moonlight-qt.pro CONFIG+=debug CONFIG+=perigee-tests
make -j"$(nproc)" debug
```

Run the focused identity and migration tests without a display:

```bash
QT_QPA_PLATFORM=offscreen SDL_VIDEODRIVER=dummy \
  ./tests/perigee-tests BrandingTest -silent
```

Run the complete suite with software rendering:

```bash
xvfb-run -a -s "-screen 0 1280x720x24" \
  env LIBGL_ALWAYS_SOFTWARE=1 QT_QPA_PLATFORM=xcb SDL_VIDEODRIVER=dummy \
  ./tests/perigee-tests -silent
```

Some integration tests create private loopback services. They do not require a live Polaris or Sunshine host.

## Keyboard and controller use

The default Deck keyboard shortcut is `Ctrl+Alt+Shift+Space`.

The default Deck controller chord is `LB+RB+Back+Start`. Change both bindings in the Perigee settings.

Use these controls while Deck is open:

- Use the arrow keys, directional pad, or left stick to move.
- Press `Enter` or the controller confirm button to activate an item.
- Press `Escape` or the controller back button to go back or close Deck.
- Press `LB` or `RB` to change categories.
- Press `Y` to focus search when the platform supports text input.

The direct statistics chord remains `LB+RB+Back+X`.

The **Legacy direct disconnect** setting restores the original controller disconnect behavior. The keyboard shortcut still opens Deck.

## Polaris behavior

Perigee uses the paired Moonlight client identity for Polaris Hypertext Transfer Protocol Secure (HTTPS) requests.

The client pins each HTTPS request to the paired host certificate.

The client uses authenticated capability, permission, endpoint, and session data. It does not infer support from a version string.

Unsupported or denied actions remain disabled with a reason. Perigee does not send guessed requests.

Named-command execution accepts only advertised identifiers and structured values. The client has no free-form command field.

The companion implementation is maintained in the [WonderWerks Polaris fork](https://github.com/WonderwerksSoftware/polaris). Live qualification is pending.

## Standard Sunshine behavior

Perigee keeps the inherited GameStream-compatible streaming path for standard [Sunshine](https://github.com/LizardByte/Sunshine) hosts.

Local Deck actions remain available without Polaris. Polaris-only actions are hidden or disabled when the host does not advertise them.

Automated regression tests cover the fallback boundary. A bounded live Standard Sunshine launch and video/audio smoke passed. The client does not probe Polaris endpoints on an ordinary Sunshine host. Live keyboard, mouse, controller, Deck, statistics, and disconnect/quit acceptance remain open.

## Contributing

Keep Perigee changes separate from the inherited streaming core when practical. Preserve upstream names when they identify source, protocols, or libraries.

Add a failing test before each behavior change. Run the focused tests after each small change.

Before you submit a change, run the applicable complete suite. Then run this check:

```bash
git diff --check
```

Do not add credentials, pairing keys, certificates, clipboard contents, or private host data to tests or logs.

See [upstream-pins.md](docs/upstream-pins.md) for the reviewed Moonlight and Polaris baselines.

## License

Perigee is distributed under the GNU General Public License, version 3 or later. See [LICENSE](LICENSE).

Keep all source notices and dependency licenses when you redistribute the software. See [NOTICE.md](NOTICE.md) for project attribution.

## Upstream attribution

Perigee is a modified work based on [Moonlight Qt](https://github.com/moonlight-stream/moonlight-qt).

The streaming protocol implementation uses [Moonlight Common C](https://github.com/moonlight-stream/moonlight-common-c).

[Polaris](https://github.com/papi-ux/polaris) is the enhanced host integration target. [Artemis](https://github.com/wjbeckett/artemis) supplied user-experience inspiration only.

[Nova](https://github.com/papi-ux/nova) was a Polaris protocol and behavior reference only.

These projects do not sponsor or endorse Perigee. Their names identify upstream or reference work only.

## Documentation style

The documentation uses guidance from [ASD-STE100 Issue 9](https://www.asd-ste100.org/assets/files/ASD-STE100_ISSUE9.pdf), dated 2025-01-15. It is not certified as ASD-STE100 compliant.

The [official current-issue page](https://www.asd-ste100.org/STE_downloads.html) identifies the current standard.

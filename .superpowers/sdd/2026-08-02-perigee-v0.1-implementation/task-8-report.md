# Task 8 Report: Make Deck Bindings Configurable

Status: implementation complete; live display qualification deferred because the
current desktop/Xwayland environment is unresponsive

Commit: `feat: make Deck bindings configurable` (this commit)

Starting HEAD: `a67744e8553a95e4e456af1fad76589cdfc40883`

## Summary

- Added one `DeckBindings` value authority for defaults, validation, fallback,
  persistence, display formatting, and controller-chord capture.
- Persisted the keyboard binding as Qt modifier bits plus one SDL scancode and
  the controller binding as an SDL controller-button bitmask. Invalid keyboard
  and controller fields independently fall back to safe defaults.
- Added the exact `StreamingPreferences` properties and durable `QSettings`
  keys required by the Task 8 contract.
- Fed a validated binding snapshot into the existing `DeckInputRouter`; there
  is no second in-stream chord detector. Configured physical chords preserve
  the existing ownership, release-tail, stats, and neutralization behavior.
- Added a controller-navigable **Perigee Deck** settings section with keyboard
  and raw-controller capture, reset, explicit cancel/Escape, single-B controller
  cancellation, legacy direct disconnect, and inline stats-chord conflict text.
- Captured controller events are handled before face-button swapping or Qt
  navigation and are swallowed, including events from non-owning controllers.

## Changed files

- `app/perigee/input/deckbindings.{h,cpp}`
- `app/perigee/input/deckinputrouter.{h,cpp}`
- `app/gui/perigee/DeckBindingSettings.qml`
- `app/gui/sdlgamepadkeynavigation.{h,cpp}`
- `app/gui/SettingsView.qml`
- `app/settings/streamingpreferences.{h,cpp}`
- `app/streaming/session.cpp`
- `app/app.pro`
- `app/qml.qrc`
- `tests/test_deckbindings.cpp`
- `tests/test_deckbindingsqml.cpp`
- `tests/test_sdlgamepadkeynavigation.cpp`
- `tests/test_deckinputrouter.cpp`
- `tests/test_inputintegration.cpp`
- `tests/perigee-tests.pro`

Task 9 actions, Polaris adapters, and host integration are untouched.

## Binding authority and validation

`DeckBindings` owns the exact defaults:

- keyboard: `Ctrl+Alt+Shift+Space`
- controller: `LB+RB+Back+Start`
- legacy direct disconnect: disabled

Keyboard bindings require at least one supported Qt modifier and one supported,
non-modifier SDL scancode. Controller bindings require at least two supported
SDL controller buttons. Empty masks, unsupported high bits, single-button
chords, and the exact `LB+RB+Back+X` performance-statistics chord are rejected.
Construction and `QSettings` loading retain the safe default for each invalid
field rather than allowing a malformed persisted binding into the router.

The durable keys are:

- `deckKeyModifiers`
- `deckKeyScancode`
- `deckControllerButtons`
- `legacyGamepadDisconnect`

The router receives one normalized `DeckBindings` snapshot when a Session is
initialized. With legacy mode disabled, the configured controller chord toggles
Deck. With legacy mode enabled, controller Deck interception is disabled so
Moonlight's original `LB+RB+Back+Start` path remains responsible for direct
disconnect; the configured keyboard chord still opens Deck.

## TDD evidence

### Initial RED

The new test sources and manifests were registered before the implementation:

`qmake6 .. CONFIG+=debug CONFIG+=perigee-tests`

Result: exit 0.

`make -j2 sub-tests`

Result: exit 2 at the expected missing production seam:

`No rule to make target '../app/perigee/input/deckbindings.cpp'`

### Edge RED/GREEN

Extended physical function-key mapping and controller-capture cancellation were
added as tests first. The build failed because
`controllerBindingCaptureCancelled` did not yet exist. After implementing the
signal and capture path, the tests passed.

A mixed-controller SDL queue test initially captured mask 13 instead of mask 9.
Systematic isolation showed that `SDL_PushEvent` rewrites an unregistered fake
controller instance ID to `-1`, so the event queue cannot deterministically
model two distinct physical controllers. A direct test of the production raw
button seam was then added first; it failed to compile until the seam existed.
The final suite keeps a real SDL queue test for raw-before-swap/no-leak behavior
and uses the narrow seam only for deterministic controller ownership.

An audit test also exposed the false assumption that SDL F1 through F24
scancodes are contiguous. The mapper now handles F1-F12 and F13-F24 as their
two real SDL ranges.

## Focused GREEN results

Each focused class was run in a fresh process with:

`QT_QPA_PLATFORM=offscreen SDL_VIDEODRIVER=dummy ./build-tests/tests/perigee-tests <class> -silent`

- `DeckBindingsTest`: 10 passed, 0 failed.
- `DeckBindingsQmlTest`: 5 passed, 0 failed.
- `DeckInputRouterTest`: 15 passed, 0 failed.
- `SdlGamepadKeyNavigationTest`: 5 passed, 0 failed.
- `InputIntegrationTest`: 18 passed, 0 failed.

These focused gates cover exact defaults, isolated save/reload, legacy
persistence, invalid persisted values, the stats conflict, malformed masks,
F13/F24 mapping, reset, controller capture completion/cancellation/ownership,
raw capture before face swapping, non-default router chords, legacy passthrough,
and all existing input-integration behavior.

## Build, stress, and source verification

- Debug application:
  `CCACHE_DIR="$PWD/build-tests/.ccache" make -C build-tests/app qmake_all`
  followed by
  `CCACHE_DIR="$PWD/build-tests/.ccache" make -C build-tests/app -f Makefile.Debug -j1`
  — both exit 0; `moonlight` linked.
- Fresh-process stress: 25 independent invocations of each of
  `DeckBindingsTest`, `DeckBindingsQmlTest`, `DeckInputRouterTest`, and
  `SdlGamepadKeyNavigationTest` — 100 processes, 875 pass records, 0 failures.
- `git diff --check` — clean.
- No temporary comparison worktree, test process, Xvfb process, build process,
  or diagnostic logging remains after the environment investigation.

## Full-suite platform qualification

The required full-suite commands were attempted, bounded, and not reported as
passes:

- The restricted canonical surfaceless command could not create required OpenGL
  contexts or SDL windows. All focused Task 8 classes in that run were green.
- The normal-device canonical command completed Deck QML 11/0, Deck surface
  renderer 6/0, Deck bindings 10/0, binding QML 5/0, router 15/0, delivery 3/0,
  and neutralization 5/0, then stalled in the existing InputIntegration SDL
  window creation path.
- The exact isolated InputIntegration case stalled inside `SDL_CreateWindow()`
  before Task 8 logic. A clean detached Task 7 baseline at starting HEAD
  `a67744e8` reproduced the same stall with the same command.
- The live Wayland full-suite invocation produced no runner output within its
  60-second bound and was terminated.
- A dummy-SDL fallback stack trace showed Qt's offscreen GL path blocked in
  X11 connection setup (`xcb_connect` / `XOpenDisplay`). Unsetting `DISPLAY`
  changed the symptom to the expected immediate GL-context failure.

The current compositor/Xwayland display path is therefore the remaining
qualification blocker. Per the Task 8 execution instruction, no further retries
were made against that unchanged environment.

## Residual concerns

- Re-run both full-suite commands when the Wayland/Xwayland desktop is
  responsive; they are not claimed as passing in this report.
- The raw controller capture is covered through SDL events plus a deterministic
  physical-controller ownership seam, but final hardware acceptance should
  exercise the intended controllers on a live settings page.
- Bidirectional Polaris/10G host testing belongs to the later host-integration
  task and was not started here.

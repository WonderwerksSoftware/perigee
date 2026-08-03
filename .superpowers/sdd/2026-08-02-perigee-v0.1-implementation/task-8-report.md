# Task 8 Report: Make Deck Bindings Configurable

Status: implementation and two hardening rounds complete. Live display and
physical-controller qualification remain deferred because the current
headless environment cannot create the required OpenGL or SDL windows.

Starting HEAD: `a67744e8553a95e4e456af1fad76589cdfc40883`

Task 8 commits:

- `86eb605615b6d39d09540577a94844cee928dde9` — configurable Deck bindings
- `6d2e4c7c` — first input and capture hardening round
- `fix: harden configurable Deck input` — second hardening round (this commit)

## Implemented behavior

- `DeckBindings` is the single authority for defaults, validation, fallback,
  persistence, display formatting, and controller-chord capture.
- Keyboard bindings persist Qt shortcut modifiers plus one physical SDL
  scancode. Controller bindings persist an SDL controller-button bitmask.
- The settings page provides keyboard and controller capture, explicit cancel,
  reset, inline conflict feedback, controller navigation, and the legacy
  direct-disconnect toggle.
- Wayland keyboard capture uses Qt's native scan code, converts the Wayland
  `key + 8` value back to evdev, and maps evdev to SDL. Unsupported platforms
  fail closed without changing the saved binding.
- Classification-only Qt modifier flags, including `KeypadModifier`, are
  removed before validation and persistence. `Ctrl+KP Enter` is accepted as a
  physical shortcut; keypad-only input still fails the modifier requirement.
- Controller capture records the largest simultaneously held chord, not a
  rolled union. Focus loss, disable, owner removal, hide, and destruction all
  cancel capture without leaking input.
- The Deck router tracks viable configured-Deck, performance-statistics, and
  reserved-quit targets per physical controller. There is no timer and no
  second detector outside the router.
- With legacy direct disconnect disabled, the original
  `LB+RB+Back+Start` chord is suppressed even when the configured Deck chord is
  different. Exact completion cannot reach Moonlight's old `SDL_QUIT` path.
- With legacy direct disconnect enabled, the original quit sequence reaches
  Moonlight unchanged. Controller Deck capture is disabled; the keyboard
  shortcut remains available.
- Closed-Deck statistics input is order independent. On the final down event,
  the router consumes that physical event and immediately replays the complete
  four-event sequence once to the real Moonlight handler. Later physical
  releases pass through and remain balanced in gamepad-mouse mode.
- Open-Deck statistics input remains local and side-effect-free for both face
  swap states and both face-first and shared-button-first orders.
- Candidate aborts replay the prefix before routing the current event. The
  current event is represented explicitly and is not applied to held-input
  state until replay finishes. Session routes it once against the resulting
  Deck state without recursive event dispatch.
- External Deck open and close transitions discard pending candidates, retain
  the needed release tails, and leave the next fresh keyboard or controller
  tap available to the new target.

## Validation rules

The exact defaults remain:

- keyboard: `Ctrl+Alt+Shift+Space`
- controller: `LB+RB+Back+Start`
- legacy direct disconnect: disabled

Keyboard bindings require at least one of Shift, Control, Alt, or Meta and one
supported non-modifier SDL scancode. Controller bindings require at least two
supported buttons and reject:

- unsupported bits or empty/single-button masks,
- the statistics chord and its prefixes or supersets for physical X and Y,
- strict supersets of the original quit chord,
- opposite D-pad directions that cannot be pressed on a normal controller.

The durable settings keys are:

- `deckKeyModifiers`
- `deckKeyScancode`
- `deckControllerButtons`
- `legacyGamepadDisconnect`

Invalid stored keyboard and controller values fall back independently to safe
defaults.

## TDD evidence

### Initial implementation RED

The new test sources and manifests were registered before production code.
The first build stopped at the intended missing seam:

`No rule to make target '../app/perigee/input/deckbindings.cpp'`

Additional test-first failures covered F13-F24's non-contiguous SDL ranges,
controller capture cancellation, and deterministic physical-controller
ownership.

### Hardening round 1

Tests first exposed permissive controller validation, logical-key capture on
Wayland, capture lifecycle leaks, rolled controller unions, face-swap
navigation drift, and replay that did not reach the real Moonlight handler.

Fresh GREEN counts after that round were:

- `DeckBindingsTest`: 14 passed, 0 failed.
- `DeckBindingsQmlTest`: 7 passed, 0 failed.
- `DeckInputRouterTest`: 24 passed, 0 failed.
- `SdlGamepadKeyNavigationTest`: 9 passed, 0 failed.
- `StreamingPreferencesTest`: 5 passed, 0 failed.
- focused router-to-real-handler integration: 5 passed, 0 failed.

### Hardening round 2 RED

The second review cases were added before production changes. The observed RED
results matched the intended missing branches:

- router: 6 targeted failures — reserved quit leaked with a custom chord,
  closed stats completed incorrectly, external sync retained keyboard and
  controller buffers, and controller/keyboard aborts returned stale actions;
- QML: 2 targeted failures — `KeypadModifier` was persisted and keypad-only
  input satisfied the modifier check;
- real-handler integration: 3 targeted failures — closed stats and reserved
  quit events reached the wrong destination.

The monolithic run then exposed one additional isolation failure: Qt had cached
the default `QSettings` path before the preferences class changed
`XDG_CONFIG_HOME`. The save/reload test now runs in a fresh child process with
the temporary config root present before `QGuiApplication` starts. The parent
test snapshots and restores organization name, application name, default
format, and `XDG_CONFIG_HOME`; a following test class verifies the restoration
in monolithic order.

## Current GREEN results

Fresh focused runs:

- `DeckBindingsTest`: 14 passed, 0 failed.
- `DeckBindingsQmlTest`: 9 passed, 0 failed.
- `DeckInputRouterTest`: 30 passed, 0 failed.
- `SdlGamepadKeyNavigationTest`: 9 passed, 0 failed.
- `StreamingPreferencesTest`: 5 passed, 0 failed.
- `StreamingPreferencesIsolationTest`: 3 passed, 0 failed in monolithic order.
- `InputIntegrationTest` with dummy SDL: 23 passed, 0 failed.
- `InputIntegrationTest` with offscreen Qt but no dummy SDL: 21 passed; the two
  SDL hidden-window cases fail before Task 8 logic because the environment
  cannot create a window.

The real-handler coverage includes both face-swap states, shared-first and
face-first statistics input, exact reserved-quit suppression, legacy
`SDL_QUIT`, aborted candidate replay, and replayed Back followed by one
balanced host face-button press/release.

## Build and stress

- Full Debug application compile and link:
  `CCACHE_DIR=build-tests/.ccache make -C build-tests -j2` — exit 0.
- Round 1 fresh-process stress: 100 router runs, 100 router-to-handler runs,
  100 binding runs, and 100 capture runs — no failures.
- Round 2 fresh-process stress: 100 full router runs, 100 focused real-handler
  runs, 50 full binding-QML runs, and 50 real preferences runs — no failures.
- `git diff --check` — clean before commit.

## Full-suite environment result

The canonical headless run used offscreen Qt and dummy SDL. Every non-renderer
class passed, including all 23 real input-integration records and the global
settings-isolation sentinel. The only failures were:

- `DeckQmlTest`: 8 OpenGL-context creation failures;
- `DeckSurfaceRendererTest`: 3 OpenGL-context creation failures.

Without dummy SDL, the two existing hidden-window integration cases also fail
at `SDL_CreateWindow()` before Task 8 routing. These are environment
limitations and are not claimed as passing.

## SettingsView seam

The test executable constructs the real resource component at
`qrc:/gui/perigee/DeckBindingSettings.qml`, and the linked application embeds
both that component and `SettingsView.qml`. A full no-display construction of
`SettingsView.qml` is not feasible in this test target without replacing real
application dependencies: `StreamingPreferences`, `ComputerManager`,
`SdlGamepadKeyNavigation`, and `SystemProperties` are registered as singleton
QML modules only in `app/main.cpp`; the test executable does not link the full
`ComputerManager` or `SystemProperties` graphs. Direct `qmllint` reports those
four missing module registrations. No fake singleton graph was added merely to
make the component instantiate.

## Source verification

The native-scancode implementation was checked against current Qt 6 source:

- Qt Wayland assigns `code = key + 8` and forwards it as the native scan code:
  <https://codebrowser.dev/qt6/qtbase/src/plugins/platforms/wayland/qwaylandinputdevice.cpp.html#1342>
- Qt Quick documents that `KeyEvent.nativeScanCode` is passed through unchanged:
  <https://codebrowser.dev/qt6/qtdeclarative/src/quick/items/qquickevents.cpp.html#69>

## Residual concerns

- Re-run the renderer and live SettingsView cases on a responsive Wayland
  desktop with real OpenGL.
- Exercise physical keyboard and controller capture in the live settings page.
- Bidirectional Polaris acceptance over the ww-DevBox 10 Gb path belongs to the
  later host-integration task and was not started in Task 8.
- Task 9 actions and Polaris adapters remain untouched.

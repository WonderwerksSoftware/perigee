# Task 7 Report: Add the Input Ownership and Neutralization State Machine

Status: complete

Commit: `feat: give Deck safe keyboard and controller ownership` (this commit)

Starting HEAD: `732735ebfca30d7323e86bb6fd52a135540af477`

## Summary

- Added a pure `DeckInputRouter` state machine in front of Moonlight's SDL
  handlers. Closed input remains passthrough; the open Deck consumes keyboard,
  mouse, touch, and controller input locally.
- Added the default keyboard shortcut (`Ctrl+Alt+Shift+Space`), controller
  open/close chord (`LB+RB+Back+Start`), and preserved direct stats chord
  (`LB+RB+Back+X`) with press-order tolerance, repeat protection, and release
  tails.
- Added exclusive opening-controller ownership, non-owner suppression,
  hysteretic stick navigation, controller removal recovery, mouse viewport
  mapping, real Qt key/pointer/wheel delivery, and `SDL_TEXTINPUT` commit
  delivery.
- Added explicit remote key, mouse-button, and allocated-controller tracking.
  Opening and closing Deck now neutralize host state idempotently, snapshot and
  restore capture policy, and cancel or gate asynchronous touch/gamepad-mouse
  callbacks that could otherwise recreate remote input after neutralization.
- Added a dirty-only Deck renderer pump to the SDL loop. It delivers only queued
  meta-call work, starts/stops SDL text input with search focus, and does not
  call unrestricted `QCoreApplication::processEvents()`.

## Changed files

- `app/perigee/input/deckinputrouter.{h,cpp}`
- `app/perigee/deck/deckcontroller.{h,cpp}`
- `app/perigee/deck/decksurfacerenderer.{h,cpp}`
- `app/streaming/input/input.{h,cpp}`
- `app/streaming/input/remoteinputstate.{h,cpp}`
- `app/streaming/input/{keyboard,gamepad,mouse,abstouch,reltouch}.cpp`
- `app/streaming/session.{h,cpp}`
- `app/app.pro`
- `tests/test_deckinputrouter.cpp`
- `tests/test_inputneutralization.cpp`
- `tests/test_{deckcontroller,deckqml,decksurfacerenderer}.cpp`
- `tests/perigee-tests.pro`

`abstouch.cpp` and `reltouch.cpp` are included because those paths also emit
remote mouse buttons, including from SDL timer callbacks. Routing them through
the tracker is required for complete and race-safe neutralization.

## TDD evidence

### Initial state-machine RED

After registering the two focused test sources before adding production code:

`make -C build-tests qmake_all && make -C build-tests/tests -j2`

Result: exit 2 at the expected missing seam:

`No rule to make target '../app/perigee/input/deckinputrouter.cpp'`

The first implemented router run then had 7 passes and 2 expected behavioral
failures: a keyboard shortcut latch stayed armed across release, and the
controller test had not released the opener chord. The latch reset and fixture
were corrected, producing 9 passes and 0 failures at that stage.

### Renderer/text-input RED

Real input/dirty tests were added against missing
`DeckController::textInputRequested` and `DeckSurfaceRenderer::isDirty` seams.
The expected compilation failure was followed by controller search-focus text
lifecycle, dirty tracking, real key/pointer/wheel delivery, and
`QInputMethodEvent` commit support.

### Touch ownership RED/GREEN

Focused RED command:

`QT_QPA_PLATFORM=offscreen ./build-tests/tests/perigee-tests DeckInputRouterTest -v1`

Result: 11 passed and 1 failed because `SDL_FINGERDOWN` still passed through
while Deck was open. Touch events are now consumed while open, and asynchronous
touch/gamepad-mouse callbacks are cancelled or gated at the same ownership
boundary.

GREEN result for the expanded router suite: 13 passed, 0 failed.

### Half-open viewport RED/GREEN

The final mouse audit added an assertion that the first pixel beyond the SDL
stream viewport is rejected and an out-of-bounds release clamps to the final
valid pixel.

RED result: 2 passed, 1 failed because `x == viewport.x + viewport.width` was
accepted. The mapping now treats SDL rectangles as half-open.

GREEN result: 13 passed, 0 failed for `DeckInputRouterTest`.

## Ownership and neutralization contract

`beginLocalOverlayInput()` snapshots mouse/system-key capture, atomically gates
timer-driven remote input, sends one neutral state, cancels outstanding touch
gesture timers, and releases capture. `endLocalOverlayInput()` neutralizes
again, restores the captured policy unless `keepReleased` is requested, then
re-enables ordinary remote input.

`RemoteInputState::takeNeutralInput()` returns every currently down key, every
currently down mouse button, and every allocated controller slot. It clears
only discrete key/button state: a second call sends no duplicate releases but
still safely zeros every attached controller. Physical `GamepadState` axes,
triggers, buttons, and emulated clickpad state are cleared with the packet.

Controller removal and battery/lifecycle events continue to reach Moonlight
housekeeping. Removal clears Deck ownership, after which the next controller
button press can safely claim navigation ownership.

## Focused results

- `DeckInputRouterTest`: 13 passed, 0 failed.
- `InputNeutralizationTest`: 5 passed, 0 failed.
- `DeckControllerTest::textInputRequestTracksOpenSearchFocus`: 3 passed,
  0 failed.
- Live Wayland real pointer plus text input: 4 passed, 0 failed.
- Live Wayland dirty renderer: 3 passed, 0 failed.

The behavioral coverage includes closed passthrough, both open/close chords,
alternate press order, repeated key/button events, every specified controller
mapping, axis press/release thresholds, non-owner suppression, owner removal,
mouse move/click/wheel and half-open hit testing, touch suppression, text commit,
stats preservation, release tails, and idempotent double neutralization.

## Build, full suite, platform, and stress results

- Debug application build:
  `CCACHE_DIR="$PWD/build-tests/.ccache" make -C build-tests/app -f Makefile.Debug -j1`
  — exit 0 and linked `moonlight`.
- Full live Wayland suite:
  `QT_QPA_PLATFORM=wayland ./build-tests/tests/perigee-tests -v1`
  — 99 passed, 0 failed.
- Full offscreen/surfaceless suite:
  `QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=opengl QT_OPENGL=desktop LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless ./build-tests/tests/perigee-tests -silent`
  — 99 passed, 0 failed. Qt emitted one non-fatal scene-graph backend warning
  during the first QML case; every render and input assertion passed.
- Stress gate: 100 fresh-process repetitions each of `DeckInputRouterTest` and
  `InputNeutralizationTest` — exit 0. Every router repetition also executes 100
  rapid open/close cycles, for 10,000 verified cycles total.
- `git diff --check` — clean.

## Review notes

- The existing renderer cannot create its OpenGL context with plain
  `QT_QPA_PLATFORM=offscreen` in this environment. The verified surfaceless
  OpenGL environment above supplies the required offscreen gate; live Wayland
  provides the display-capable integration gate.
- A parallel rebuild briefly linked while an unrelated generated Qt meta-object
  file was being refreshed and reported missing `AppModel` symbols. The
  generated object contained the expected symbols, and the deterministic
  serial application rebuild immediately linked cleanly. This did not involve
  Task 7 source behavior.

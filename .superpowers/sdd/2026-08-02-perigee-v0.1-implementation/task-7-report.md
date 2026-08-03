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
- Added a dirty-only Deck renderer pump to the SDL loop. It drains only the
  Deck controller's completion inbox, starts/stops SDL text input with search
  focus, and does not call unrestricted `QCoreApplication::processEvents()`.

## Changed files

- `app/perigee/input/deckinputrouter.{h,cpp}`
- `app/perigee/input/deckinputdelivery.{h,cpp}`
- `app/perigee/deck/deckcontroller.{h,cpp}`
- `app/perigee/deck/deckuipump.{h,cpp}`
- `app/perigee/deck/decksurfacerenderer.{h,cpp}`
- `app/streaming/input/input.{h,cpp}`
- `app/streaming/input/remoteinputstate.{h,cpp}`
- `app/streaming/sdleventcodes.h`
- `app/streaming/input/{keyboard,gamepad,mouse,abstouch,reltouch}.cpp`
- `app/streaming/session.{h,cpp}`
- `app/streaming/video/decoder.h`
- `app/app.pro`
- `tests/test_deckinputrouter.cpp`
- `tests/test_{deckinputdelivery,deckuipump,inputintegration}.cpp`
- `tests/input_integration_stubs.{h,cpp}`
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
  `CCACHE_DIR="$PWD/build-tests/.ccache" make -C build-tests/app qmake_all && CCACHE_DIR="$PWD/build-tests/.ccache" make -C build-tests/app -f Makefile.Debug -j1`
  — exit 0 and linked `moonlight`.
- Full live Wayland suite:
  `QT_QPA_PLATFORM=wayland SDL_VIDEODRIVER=wayland ./build-tests/tests/perigee-tests`
  — 121 passed, 0 failed.
- Full offscreen/surfaceless suite:
  `QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=opengl QT_OPENGL=desktop LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless ./build-tests/tests/perigee-tests -v1`
  — 121 passed, 0 failed. This is the broad headless gate; no SDL video-driver
  override is applied, so the renderer and SDL-window integration cases run in
  the same process.
- Stress gate: 25 fresh-process repetitions of the four ownership/callback
  concurrency cases plus 25 fresh-process repetitions of Deck-scoped
  completion draining — all exited 0.
- `git diff --check` — clean.

## Review notes

- The exact surfaceless command above requires normal Mesa/SDL device access.
  A filesystem/device-sandboxed invocation could not create the OpenGL context
  or SDL windows; the unrestricted canonical gate passed all 121 tests. Live
  Wayland independently supplies the display-capable integration gate.
- A parallel rebuild briefly linked while an unrelated generated Qt meta-object
  file was being refreshed and reported missing `AppModel` symbols. The
  generated object contained the expected symbols, and the deterministic
  serial application rebuild immediately linked cleanly. This did not involve
  Task 7 source behavior.

## Independent review fix round 1

Review starting HEAD: `517b9cd154edf53e29c401955a2fb9ecf4128a2f`

The first independent review found one critical and seven important issues.
This fix round closes all eight:

1. Relative-touch delayed releases now pass the owning `SdlInputHandler` to
   both SDL timer callbacks. The production scheduling test failed with
   SIGSEGV/exit 139 at 101 ms when either callback received `nullptr`; it now
   passes and observes press followed by release.
2. `RemoteInputState` tracks native touch IDs and pen activity. Deck takeover
   sends `CANCEL_ALL` and pen `CANCEL` before releasing capture, and later
   native UP packets remain gated. The original RED observed only one touch
   record where two were required.
3. The ownership gate, remote-state bookkeeping, controller state mutations,
   timer callbacks, sends, and neutral packet share one recursive transaction
   mutex. Main-thread controller axis/button paths now participate in the same
   happens-before relationship as gamepad-mouse callbacks. Timer removal drops
   the lock before waiting, then reacquires and rechecks the ownership gate, so
   tracked sends and targeted neutralization can re-enter without deadlock.
   The deterministic RED showed an axis mutation returning while a blocked
   mouse callback still owned the send transaction; GREEN waits for it.
4. Removed the broad `sendPostedEvents(nullptr, QEvent::MetaCall)` drain.
   `DeckController` now owns an atomic completion inbox and
   `pumpPendingWork()` drains only Deck completions. A mutation restoring the
   broad drain ran an unrelated queued meta-call and failed the scoped test.
5. Both startup capture requests (`SDL_WINDOWEVENT_ENTER` and post-decoder
   recreation) are deferred while Deck owns input. Closing Deck applies the
   final deferred capture request. Removing the guard reproduced the RED.
6. The stats chord neutralizes only its opening physical controller. In merged
   single-controller mode, unrelated physical controller state remains in the
   slot-0 packet; keys, mouse buttons, and other controller slots are untouched.
   A mutation restoring global neutralization cleared the unrelated A button
   and failed.
7. `DeckUiPump` keeps the first open frame immediate, retains dirty state, and
   limits subsequent renders to one per 16 ms. The unpaced mutation failed on
   the second same-tick render. `Session::pumpDeckUi()` directly uses this
   tested policy.
8. The test target now compiles the real `input.cpp`, `gamepad.cpp`,
   `abstouch.cpp`, `reltouch.cpp`, keyboard, mouse, and Deck input delivery
   sources, stubbing only external Moonlight/session dependencies. Coverage
   includes actual SDL text-input Start/Stop application, native cancel,
   capture ordering, timer barriers, controller removal and battery
   housekeeping, router-before-handler order, real Qt wheel construction, and
   combined key plus `SDL_TEXTINPUT` delivery without duplicate commits.

### Fix-round RED/GREEN evidence

- SDL text application RED: `DeckUiPump::applyTextInput()` was temporarily a
  no-op; `appliesTextInputTransitionsToSdl` failed because
  `SDL_IsTextInputActive()` remained false. GREEN: 3 passed, 0 failed for the
  focused invocation and 5 passed, 0 failed for `DeckUiPumpTest`.
- Input delivery mutation RED: a duplicate text commit plus omitted wheel made
  `deliversWheelAndCommitsTextOnlyOnce` report `textEvents == 2` instead of 1.
  GREEN: `DeckInputDeliveryTest` 3 passed, 0 failed.
- Housekeeping mutation RED: consuming unknown SDL events while Deck was open
  changed the battery route disposition from passthrough to consumed. GREEN:
  the actual battery send and controller removal path both run after the router.
- Capture-order mutation RED: swapping physical uncapture ahead of neutral
  produced `capture-off` as the first record instead of
  `remote-mouse-release`. GREEN preserves neutral-first ordering.
- Scoped completion mutation RED, startup-capture RED, targeted-stats RED, and
  16 ms pacing RED were each reproduced independently before restoring GREEN.
- Focused final results: `InputIntegrationTest` 13/0,
  `DeckInputRouterTest` 13/0, `InputNeutralizationTest` 5/0,
  `DeckControllerTest` 16/0, `DeckUiPumpTest` 5/0, and
  `DeckInputDeliveryTest` 3/0.

### Lock and lifetime audit

- The recursive mutex remains for same-thread tracked-send composition and
  ownership neutralization. SDL timer callbacks no longer enter that mutex or
  read handler state; they only enqueue an opaque token.
- No timer callback parameter points to `SdlInputHandler` or `GamepadState`.
  Therefore `SDL_RemoveTimer()` does not need to provide callback joining for
  handler or controller storage to remain safe.
- The token-to-action `QHash`, touch state, gamepad axes, and remote sends are
  read or mutated only from the SDL event owner. The timer thread touches only
  SDL's thread-safe event queue and a by-value 32-bit token.
- Cancellation and device removal erase the token before timer removal. Deck
  takeover and release rotate active repeating tokens, and handler replacement
  uses the process-global token sequence, so already-queued stale events are
  ignored by the current handler.

## Independent review fix round 2

Review starting HEAD: `286512a7d72bffc61114e98f9ecdadec18a812a0`

The second independent review found a teardown use-after-free boundary, an
unsynchronized drag callback, a closed-Deck stats-chord release-tail bug, and a
stale report claim. This round closes all four and audits the surrounding timer
and SDL-event lifecycle:

1. Every touch and gamepad-mouse timer callback now does exactly one operation:
   enqueue app-owned `SDL_USEREVENT` code 106 with an opaque 32-bit token.
   Callbacks never dereference the handler, controller state, or touch state and
   never send remote input. A blocked real `SDL_AddTimer` callback can remain in
   `SDL_PushEvent` while the handler destructor completes safely.
2. The live handler owns the token-to-typed-action table. One-shot dispatch,
   cancellation, controller removal, destruction, Deck takeover, and Deck
   release erase or rotate tokens before removal. Old tokens are rejected after
   takeover, after release, and after complete handler replacement. A filtered
   or full SDL event queue makes a one-shot timer retry instead of silently
   losing a required release.
3. Gamepad-mouse axis selection and movement calculation now run on the SDL
   event thread from named polling, multiplier, and deadzone constants. Active
   mode survives a successful Deck token rotation without false UI toggles.
   `SDL_AddTimer` failure clears the token and does not advertise mouse mode;
   restart failure removes the previously active notification.
4. The stats chord is Deck-local only while Deck is open. With Deck closed, all
   four button-down events and their button-up tails pass through the ordinary
   `SdlInputHandler`, balancing X1, X2, and middle mouse presses while preserving
   an unrelated held physical right mouse button.
5. App-owned SDL user-event codes are centralized in
   `streaming/sdleventcodes.h`: frame-ready remains 0, existing session codes
   remain 100 through 105, and the input timer uses 106. SDL2 and sdl2-compat
   therefore share one explicit, collision-free code namespace.
6. The report summary now accurately says the UI pump drains only the Deck
   completion inbox. It does not claim delivery of broad queued MetaCall work.

### Fix-round 2 RED/GREEN evidence

- Timer callback boundary RED: the production callback test blocked in a
  remote send (`RemoteSend`) instead of the required SDL event enqueue
  (`InputEventPush`). The teardown variant reproduced the same forbidden side
  effect. GREEN: every timer callback blocks only at `SDL_PushEvent` and records
  no mouse send or movement.
- Closed stats chord RED: the router consumed the final X down and its release
  tail while closed. GREEN: closed down/up events all pass through; open stats
  remains local and consumed.
- Focused final results: `InputIntegrationTest` 17 passed, 0 failed and
  `DeckInputRouterTest` 13 passed, 0 failed.

### Fix-round 2 audit and verification

- Token map ownership: every `QHash` insertion, lookup, and erasure runs on the
  SDL owner/destruction path. Timer threads only call `SDL_PushEvent`; there is
  no cross-thread Qt container access.
- Lifetime: the Session event loop dispatches input timer events only while its
  `m_InputHandler` exists. Tokens are nonzero, process-global, and monotonically
  generated; wrap skips zero and any token active in the current table. The
  remaining 32-bit wrap assumption is that one queued SDL event cannot survive
  more than 2^32 later timer generations, which is far beyond the queue's
  operational lifetime.
- Debug application link:
  `CCACHE_DIR="$PWD/build-tests/.ccache" make -C build-tests/app -f Makefile.Debug -j4`
  — exit 0 and linked `moonlight` after the final source change.
- Canonical surfaceless full suite:
  `QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=opengl QT_OPENGL=desktop LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless ./build-tests/tests/perigee-tests -silent`
  — 125 passed, 0 failed.
- Live Wayland full suite:
  `QT_QPA_PLATFORM=wayland SDL_VIDEODRIVER=wayland ./build-tests/tests/perigee-tests -silent`
  — 125 passed, 0 failed.
- Focused stress: 25 fresh-process repetitions of callback isolation, real
  callback teardown, ownership ordering, takeover/release token rotation,
  main-thread gamepad-mouse dispatch, timer-add failure, handler replacement,
  and closed stats release balance — 250 passed, 0 failed.
- `git diff --check` — clean.

## Independent review fix round 3

Review starting HEAD: `658070883432d8208cf2e1f7b3674f8040b49fa1`

The third independent review found one Important full-range axis bug and no
Critical or Minor findings. The gamepad-mouse stick-strength comparison passed
four `short` values directly to `qAbs()`. SDL axes may legitimately be
`-32768`, whose positive magnitude is not representable in a signed short. Qt
debug builds assert on that input; unchecked builds can overflow and select the
weaker stick.

The fix promotes each of the left-X, left-Y, right-X, and right-Y terms to
`int` before `std::abs()`. This matches the existing promotion-safe pattern in
`gamepad.cpp` and changes only stick-strength selection at the signed-short
minimum; the movement curve, deadzone, sign handling, and emitted deltas remain
unchanged.

### Fix-round 3 RED/GREEN evidence

- Focused RED command on the review starting HEAD:
  `QT_QPA_PLATFORM=offscreen SDL_VIDEODRIVER=dummy ./build-tests/tests/perigee-tests InputIntegrationTest gamepadMouseSelectsFullNegativeStickWithoutOverflow -v1`
  — 2 passed, 1 failed. The parent test isolated real timer dispatch in a child
  process; the child hit Qt's signed-minimum assertion and returned `CrashExit`
  instead of `NormalExit`.
- The regression drives the real controller-axis handler with left stick
  strength 65534 and right stick input `(-32768, -32768)`. After Y-axis
  normalization, it verifies state `LS=(32767,-32767)` and
  `RS=(-32768,32767)`, then dispatches the real gamepad-mouse timer action and
  requires one negative-X/negative-Y movement. This proves the right stick wins
  the one-unit strength margin without overflow.
- Focused GREEN: the same command passed 3 tests with 0 failures.
- Full focused class:
  `QT_QPA_PLATFORM=offscreen SDL_VIDEODRIVER=dummy ./build-tests/tests/perigee-tests InputIntegrationTest -silent`
  — 18 passed, 0 failed.

### Fix-round 3 verification

- Debug application link:
  `CCACHE_DIR="$PWD/build-tests/.ccache" make -C build-tests/app -f Makefile.Debug -j4`
  — exit 0 and linked `moonlight`.
- Canonical surfaceless full suite:
  `QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=opengl QT_OPENGL=desktop LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless ./build-tests/tests/perigee-tests -silent`
  — 126 passed, 0 failed.
- Live Wayland full suite:
  `QT_QPA_PLATFORM=wayland SDL_VIDEODRIVER=wayland ./build-tests/tests/perigee-tests -silent`
  — 126 passed, 0 failed.
- Focused fresh-process stress: 25 repetitions of takeover token rotation,
  ordinary main-thread gamepad-mouse dispatch, and the subprocess-isolated
  full-negative-axis selection — 125 parent-process tests passed, 0 failed.
- `git diff --check` — clean.

# Task 9 Report: Expose Truthful Local Moonlight Actions

Status: implementation and local verification are complete. Live stream
qualification remains deferred to the Task 20 ww-DevBox 10G acceptance pass.

Starting HEAD: `a651671c93e21859d4144e9b536c09cb9ea9cce7`

## Implemented behavior

- Added a narrow `SessionFacade` and a `GameStreamAdapter` backed by the live
  Moonlight `Session`. The adapter does not keep the session alive. It fails
  closed when the session authority is absent or destroyed.
- Registered the seven required local action IDs:
  - `input.mouse-capture`
  - `input.keyboard-capture`
  - `input.release-captured`
  - `stats.overlay`
  - `window.fullscreen`
  - `session.disconnect-client`
  - `session.quit-perigee`
- All toggle actions read the resulting Session state after the request. A
  mismatch returns `state_mismatch` with no success evidence.
- Mouse and keyboard capture actions change the saved restore intent while
  Deck owns input. They do not re-grab the streamed application behind Deck.
  Release captured input clears both restore intents.
- Existing keyboard and controller shortcuts now call the same Session
  methods as Deck. Existing key release, controller neutralization, statistics
  balance, and legacy disconnect routing remain in the real input handler.
- Disconnect and quit require explicit confirmation. The confirmation text is
  specific to each action. Deck passes a `confirmed` parameter only after the
  user accepts the confirmation card.
- Added `SessionExitIntent`, the state object used by the production cleanup
  task. Client disconnect keeps Perigee and the host session running. Quit
  Perigee exits the client and keeps the host session running. The legacy
  quit-host-and-exit path still forces host shutdown and Perigee exit.
- The local action registry is available for ordinary Sunshine/GameStream
  sessions. It has no Polaris reachability dependency.

## Architecture decisions

`Session` remains a normal `QObject` with non-virtual public action methods.
`GameStreamSessionFacade` is a small bridge owned by `Session`; it delegates to
those methods and provides a `QObject` lifetime authority for `QPointer`.
This avoids a second state store and preserves the existing Session test seam.

Capture readback separates configured keyboard-capture intent from the current
physical grab. Deck saves the configured intent, releases the physical grab,
and restores only the final saved intent when it closes.

`SessionExitIntent` replaced raw exit booleans. The production deferred cleanup
task reads this exact object to decide whether to quit the host and whether to
exit Perigee. This makes the three exit paths testable without constructing a
networked, decoder-backed Session.

`OverlayManager::isOverlayEnabled()` is now `const`. Its implementation is only
an atomic load from the existing enabled flag. The change adds const-correct
readback without changing its synchronization or state.

## TDD evidence

### GameStream adapter RED

The adapter tests and build manifest were added before production files.
The test build stopped with exit 2 because `gamestreamadapter.{h,cpp}` and
`sessionfacade.h` did not exist.

The first test set covered exact IDs and metadata, state evidence, mismatch
handling, capture release, confirmation, distinct disconnect and quit effects,
and absent or destroyed session authority.

### Confirmation RED

The Deck controller confirmation test was added before the new property. The
test build stopped because `DeckController::confirmationMessage` did not
exist. The production change then carried action-specific text from the
descriptor to the QML confirmation card and sent `confirmed=true` only after
acceptance.

### Input integration RED

Shortcut and open-Deck capture tests were added before the shared Session and
input-handler methods. The build stopped with exit 2 on the missing Session
action methods and keyboard-capture intent APIs.

The final real-handler run is:

`QT_QPA_PLATFORM=offscreen SDL_VIDEODRIVER=dummy ./perigee-tests InputIntegrationTest -v1`

Result: 31 passed, 0 failed.

### Production exit-state RED and GREEN

`SessionExitIntentTest` and its manifest entry were added first. QMake warned
that `app/streaming/sessionexitintent.h` was missing, and compilation stopped
at the missing include with exit 2.

After production integration:

`QT_QPA_PLATFORM=offscreen SDL_VIDEODRIVER=dummy ./perigee-tests SessionExitIntentTest -v1`

Result: 7 passed, 0 failed. The cases prove client-only disconnect, Perigee
quit with host preservation, legacy forced host quit, normal preference-based
exit, and unexpected-termination suppression.

## Focused GREEN results

- `GameStreamAdapterTest`: 17 passed, 0 failed.
- `SessionExitIntentTest`: 7 passed, 0 failed.
- `InputIntegrationTest`: 31 passed, 0 failed.
- `ActionRegistryTest`: 20 passed, 0 failed.
- `DeckControllerTest`: 16 passed, 0 failed.
- `DeckInputRouterTest`: 35 passed, 0 failed.
- `InputNeutralizationTest`: 5 passed, 0 failed.
- `DeckBindingsTest`: 14 passed, 0 failed.

The established filtered non-display suite also passed:

- `DeckInputDeliveryTest`: 3 passed.
- `StreamingPreferencesTest`: 5 passed.
- `StreamingPreferencesIsolationTest`: 3 passed in monolithic order.
- `PerigeeSmokeTest`: 3 passed.
- `SdlGamepadKeyNavigationTest`: 9 passed.
- `DeckBindingsQmlTest`: 9 passed.
- `OverlayLayoutTest`: 26 passed.
- `DeckUiPumpTest`: 5 passed.

## Build, lint, and stress

- Debug application qmake, compile, and link: exit 0. The result is
  `build-baseline/app/moonlight`.
- Test compile and link: exit 0.
- `/usr/lib64/qt6/bin/qmllint app/gui/perigee/*.qml`: exit 0. It reports the
  existing unqualified-access warnings but no QML error.
- Fresh-process stress: 20 full adapter runs and 10 full input-integration
  capture/restoration runs; 30 of 30 exited 0.
- `git diff --check`: clean before the report was written.

The writable ccache path used for builds was
`build-tests/.ccache`. The default home cache is read-only in this sandbox.

## Full-suite environment result

The canonical offscreen and dummy-SDL run produced 214 passes and the exact 11
established renderer failures:

- `DeckQmlTest`: 8 failures at `Deck OpenGL context creation failed`.
- `DeckSurfaceRendererTest`: 3 failures at the same environment gate.

All non-renderer classes passed. A virtual X server attempt aborted before it
could improve the OpenGL gate, so this report does not claim a rendered-QML
integration pass in the current sandbox.

## Live-smoke status and residual concerns

No ww-DevBox, homelab key, Polaris endpoint, Sunshine host, or 10G interface
was accessed during Task 9. Live Perigee, Polaris, Sunshine, controller, and
multi-display qualification is deliberately consolidated into the authorized
Task 20 ww-DevBox 10G acceptance pass.

The remaining qualification gap is the live stream and rendered overlay on a
real display-capable client. The adapter, production exit state, input handler,
build, and all non-renderer regressions are locally verified.

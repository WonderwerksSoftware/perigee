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
- Existing keyboard and controller shortcuts use the same Session request
  operations as Deck, with an explicit policy that preserves their legacy host
  quit preference. Existing key release, controller neutralization, statistics
  balance, and legacy disconnect routing remain in the real input handler.
- Disconnect and quit require explicit confirmation. The confirmation text is
  specific to each action. Confirmation authority is held by `ActionRegistry`,
  is bound to one action, and is consumed by the first acceptance attempt.
- Added `SessionExitIntent`, the state object used by the production cleanup
  task. Deck client disconnect keeps Perigee and the host session running.
  Quit Perigee exits the client and keeps the host session running. Legacy
  keyboard and controller disconnect still honor `quitAppAfter`, and the
  quit-host-and-exit path still forces host shutdown and Perigee exit.
- The local action registry is available for ordinary Sunshine/GameStream
  sessions. It has no Polaris reachability dependency.

## Architecture decisions

`Session` remains a normal `QObject` with non-virtual public action methods.
`SessionFacade` is an enforceable `QObject` base, and
`GameStreamSessionFacade` is a small bridge owned by `Session`. The adapter
holds one `QPointer<SessionFacade>`, so the same object provides behavior and
lifetime authority. This avoids a second state store and preserves the
existing Session test seam.

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
descriptor to the QML confirmation card. The hardening pass replaced the
caller-controlled parameter with a registry-created, action-bound invocation.

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

The canonical offscreen and dummy-SDL run produced 230 passes and the exact 11
established renderer failures:

- `DeckQmlTest`: 8 failures at `Deck OpenGL context creation failed`.
- `DeckSurfaceRendererTest`: 3 failures at the same environment gate.

All non-renderer classes passed. The live `ActionRow` QML component test also
passed without requiring the OpenGL renderer, so succeeded evidence is covered
in this sandbox. This report does not claim full rendered-QML integration in
the current environment.

## Fix round 1: session-action hardening

Base commit: `05f610654ab00e975a59d533ef0f911c4641b31a`

The review findings were reproduced before each production fix:

- A forged `confirmed=true` parameter reached the destructive adapter path:
  2 passed, 1 failed. The registry now creates the only confirmed invocation.
- A direct registry caller could forge the same parameter: 2 passed, 1 failed.
- A pending confirmation survived execution of another action: 2 passed,
  1 failed. Any execution now invalidates it.
- A wrong-action acceptance left the original grant reusable: 2 passed,
  1 failed. Any acceptance attempt now consumes the grant.
- `SessionFacade` was not itself a `QObject`: 2 passed, 1 failed. It is now the
  single lifetime and behavior identity held by the adapter's `QPointer`.
- Destructive Session requests did not report SDL enqueue acceptance. The RED
  build stopped on the old `void` facade contract. Request helpers now mutate
  exit intent only after `SDL_PushEvent()` accepts the event.
- The succeeded evidence string was not present in the live `ActionRow`: 2
  passed, 1 failed. The row now renders category and observed evidence with a
  distinct succeeded state.
- The physical keyboard-grab shortcut lacked a handler-owned toggle seam. The
  RED build stopped on the missing method. The shortcut now toggles from the
  current physical grab state while Deck still owns configured restore intent.

Final focused results:

- `GameStreamAdapterTest`: 22 passed, 0 failed.
- `SessionExitIntentTest`: 10 passed, 0 failed.
- `InputIntegrationTest`: 34 passed, 0 failed.
- `ActionRegistryTest`: 23 passed, 0 failed.
- `DeckControllerTest`: 17 passed, 0 failed.
- `DeckInputRouterTest`: 35 passed, 0 failed.
- `DeckQmlTest::actionRowRendersSucceededEvidence`: 3 passed, 0 failed.

Final build and stability results:

- Debug application compile and link: exit 0.
- Debug test compile and link: exit 0.
- QML lint: exit 0 with the established unqualified-access warnings only.
- Fresh-process stress: 20 of 20 adapter runs and 10 of 10 input-integration
  runs exited 0.
- Monolithic offscreen run: 230 passed and the exact 11 established
  environment-only OpenGL context failures. All non-display classes passed.
- No Xvfb, ww-DevBox, live network, Polaris, Sunshine, or 10G path was used in
  this fix round.

## Live-smoke status and residual concerns

No ww-DevBox, homelab key, Polaris endpoint, Sunshine host, or 10G interface
was accessed during Task 9. Live Perigee, Polaris, Sunshine, controller, and
multi-display qualification is deliberately consolidated into the authorized
Task 20 ww-DevBox 10G acceptance pass.

The remaining qualification gap is the live stream and rendered overlay on a
real display-capable client. The adapter, production exit state, input handler,
build, and all non-renderer regressions are locally verified.

# Stock-Host Physical Display Switching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the fork-only display-target path with a controller-accessible Deck action that sends stock Polaris physical-display shortcuts and verifies that video resumes.

**Architecture:** A focused `PhysicalDisplayController` owns one in-stream display request, its fresh-frame deadline, and one bounded restoration attempt. `GameStreamAdapter` exposes 13 static physical-display actions and uses the session facade for asynchronous completion; `Session` sends a fixed GameStream keyboard sequence through `SdlInputHandler` while Deck continues to suppress all ordinary remote input. Polaris discovery remains responsible only for official advertised `/polaris/v1` capabilities.

**Tech Stack:** C++17, Qt 6.7+, QML, SDL2, Moonlight Common C, qmake, Qt Test

## Global Constraints

- Perigee maintains only the Perigee client repository.
- Perigee does not require changes to Polaris or Sunshine.
- Physical display selection uses the GameStream input path.
- The supported display-number range is 1 through 13. The default visible count is 3.
- Deck continues to own local keyboard, pointer, and controller input during a display action.
- A fresh decoded frame proves resumed video only. It does not prove the physical identity of the frame.
- The user interface uses **Last requested**, never an authoritative active-display claim.
- No automatic display scan or unbounded retry is allowed.
- A restoration attempt is allowed only after a previously successful request in the same stream.
- Live previews and client-local display aliases are outside this implementation.
- Use ASD-STE100 Issue 9 guidance for new user-facing documentation and copy.
- Add a failing test before each production behavior change.

## File Structure

- Create `app/perigee/display/physicaldisplaycontroller.h`: state-machine interface, phases, fixed limits, sender and completion contracts.
- Create `app/perigee/display/physicaldisplaycontroller.cpp`: validation, single-flight behavior, fresh-frame verification, deadline, and bounded restoration.
- Create `tests/test_physicaldisplaycontroller.cpp`: deterministic state-machine tests with a fake clock and shortcut sender.
- Modify `app/streaming/input/input.h` and `app/streaming/input/input.cpp`: one fixed, overlay-safe physical-display shortcut sender.
- Modify `tests/input_integration_stubs.h`, `tests/input_integration_stubs.cpp`, and `tests/test_inputintegration.cpp`: record keyboard packets and test their exact order and failure cleanup.
- Modify `app/settings/streamingpreferences.h`, `app/settings/streamingpreferences.cpp`, `app/gui/perigee/DeckBindingSettings.qml`, `tests/test_streamingpreferences.cpp`, and `tests/test_deckbindingsqml.cpp`: store and edit a 1-to-13 Deck display count.
- Modify `app/perigee/actions/sessionfacade.h`, `app/perigee/actions/gamestreamadapter.h`, `app/perigee/actions/gamestreamadapter.cpp`, and `tests/test_gamestreamadapter.cpp`: expose generic display actions, status, and asynchronous results.
- Modify `app/streaming/session.h`, `app/streaming/session.cpp`, `app/streaming/sdleventcodes.h`, and `tests/test_sessiondisplaytransition.cpp`: connect Deck actions to the SDL input path and decoded-frame evidence.
- Modify `app/perigee/actions/actionregistry.cpp`: remove fork-only dynamic physical-target resolution while retaining named-command resolution until its separate upstream-contract task.
- Modify `app/perigee/polaris/polarisadapter.h`, `app/perigee/polaris/polarisadapter.cpp`, `app/perigee/polaris/polarismodels.h`, and `app/perigee/polaris/polarismodels.cpp`: remove `display_targets_v1`, target POST, and physical-display readback assumptions.
- Delete `app/perigee/display/displaytransaction.h`, `app/perigee/display/displaytransaction.cpp`, `app/perigee/display/sessiontransitioncoordinator.h`, and `app/perigee/display/sessiontransitioncoordinator.cpp`: the old server-mutation and reconnect design.
- Modify `app/main.cpp` and `app/gui/StreamSegue.qml`: remove the global display-transition object and launcher recovery surface.
- Delete `tests/test_displaytransaction.cpp` and `tests/test_sessiontransitioncoordinator.cpp`; rewrite display-related Polaris and StreamSegue tests for the stock-host boundary.
- Modify `app/app.pro` and `tests/perigee-tests.pro`: replace deleted sources and tests with the new controller.
- Modify `README.md`, `docs/testing/live-acceptance-2026-08.md`, and `docs/upstream-pins.md`: describe the stock-host behavior and record acceptance evidence.

---

### Task 1: Add the deterministic physical-display controller

**Files:**
- Create: `app/perigee/display/physicaldisplaycontroller.h`
- Create: `app/perigee/display/physicaldisplaycontroller.cpp`
- Create: `tests/test_physicaldisplaycontroller.cpp`
- Modify: `app/app.pro`
- Modify: `tests/perigee-tests.pro`

**Interfaces:**
- Consumes: a monotonic `Clock`, a `ShortcutSender`, and `HostAdapter::Completion`.
- Produces: `PhysicalDisplayController::request(int, Completion)`, `notifyAcceptedFrame(Enqueue)`, `observeFreshFrame(quint64)`, `checkDeadline()`, `cancel()`, `lastRequestedDisplay()`, `pendingDisplay()`, `active()`, and `phase()`.
- Produces: sender signature `bool(int displayNumber)` for Session integration.

- [ ] **Step 1: Write the failing state-machine tests**

Add a registered Qt test with deterministic fakes:

```cpp
class PhysicalDisplayControllerTest final : public QObject
{
    Q_OBJECT
private slots:
    void rejectsNumbersOutsideOneThroughThirteen();
    void sendsOneRequestAndCompletesOnMatchingFreshFrame();
    void rejectsConcurrentRequests();
    void ignoresStaleFrameEvidence();
    void frameEnqueueFailureAllowsTheNextAcceptedFrame();
    void timesOutWithoutRestorationWhenInitialDisplayIsUnknown();
    void restoresOneKnownDisplayAfterTimeout();
    void reportsFailureWhenTheBoundedRestorationTimesOut();
    void cancellationCompletesOnceAndDisarmsTheRequest();
};

qint64 now = 1000;
QVector<int> sends;
PhysicalDisplayController controller(
    [&now] { return now; },
    [&sends](int display) {
        sends.push_back(display);
        return true;
    });
```

Assert these exact terminal codes:

```cpp
QCOMPARE(timeout.errorCode, QStringLiteral("display_verification_timeout"));
QCOMPARE(restored.errorCode, QStringLiteral("display_switch_failed_restored"));
QCOMPARE(restoreFailed.errorCode,
         QStringLiteral("display_switch_and_restore_failed"));
QCOMPARE(cancelled.errorCode, QStringLiteral("display_switch_cancelled"));
```

- [ ] **Step 2: Register and run the test to verify failure**

Add `test_physicaldisplaycontroller.cpp` to `SOURCES` and the new header/source to the matching qmake lists.

Run from the configured build directory:

```bash
make -j"$(nproc)" debug
./tests/perigee-tests PhysicalDisplayControllerTest -silent
```

Expected: compilation fails because `PhysicalDisplayController` does not exist.

- [ ] **Step 3: Add the controller interface**

Use this public contract:

```cpp
class PhysicalDisplayController
{
public:
    static constexpr int MinimumDisplay = 1;
    static constexpr int MaximumDisplay = 13;
    static constexpr qint64 VerificationTimeoutMs = 5000;

    enum class Phase { Idle, WaitingForFrame, Restoring, Succeeded, Failed };
    using Clock = std::function<qint64()>;
    using ShortcutSender = std::function<bool(int)>;
    using Enqueue = std::function<bool(quint64)>;
    using Completion = HostAdapter::Completion;

    PhysicalDisplayController(Clock clock, ShortcutSender sender);
    bool request(int displayNumber, Completion completion);
    bool notifyAcceptedFrame(const Enqueue& enqueue);
    bool observeFreshFrame(quint64 evidenceEpoch);
    bool checkDeadline();
    void cancel();
    int lastRequestedDisplay() const;
    int pendingDisplay() const;
    quint64 evidenceEpoch() const;
    bool active() const;
    Phase phase() const;

private:
    static constexpr quint64 QueuedBit = quint64(1) << 63;
    static constexpr quint64 MaximumEvidenceEpoch = QueuedBit - 1;
    Clock m_Clock;
    ShortcutSender m_Sender;
    Completion m_Completion;
    std::atomic<quint64> m_FrameGate {0};
    quint64 m_NextEvidenceEpoch = 0;
    quint64 m_EvidenceEpoch = 0;
    qint64 m_Deadline = 0;
    int m_RequestedDisplay = 0;
    int m_PreviousDisplay = 0;
    int m_LastRequestedDisplay = 0;
    bool m_RestorationAttempted = false;
    Phase m_Phase = Phase::Idle;

    bool sendAndArm(int displayNumber, Phase phase);
    void disarmFrameGate();
    void finish(ActionResult result);
};
```

- [ ] **Step 4: Implement the minimal transition rules**

Use a strictly increasing nonzero evidence epoch and an atomic one-shot frame gate. Send the request once, arm the gate only after the sender returns success, set `deadline = clock() + 5000`, and complete only on the matching epoch. `notifyAcceptedFrame()` runs on the decoder thread and can only enqueue the epoch once; it must not access the non-atomic transition state. If SDL event enqueue fails, clear only the queued bit and leave the epoch armed so the next accepted frame can retry. On a primary timeout, disarm the old epoch, send the previous successful display once when it is nonzero and different from the requested display, then arm a new epoch. Do not start another restoration after that attempt. Disarm the gate on every terminal path.

Completion results must use this copy:

```cpp
ActionResult success {
    true,
    QStringLiteral("Display %1 requested; video resumed").arg(requested),
    {}, {},
};

ActionResult timeout {
    false, {},
    QStringLiteral("display_verification_timeout"),
    QStringLiteral("Perigee did not receive fresh video after the display request."),
};
```

For restored video, keep the previous number as `lastRequestedDisplay()` and report the requested action as failed.

- [ ] **Step 5: Run the focused tests**

```bash
make -j"$(nproc)" debug
./tests/perigee-tests PhysicalDisplayControllerTest -silent
```

Expected: PASS with one completion per request and no sender loop.

- [ ] **Step 6: Commit the controller**

```bash
git add app/perigee/display/physicaldisplaycontroller.* app/app.pro \
        tests/test_physicaldisplaycontroller.cpp tests/perigee-tests.pro
git commit -m "feat: add physical display request controller"
```

### Task 2: Send the stock host shortcut through the overlay-safe input path

**Files:**
- Modify: `app/streaming/input/input.h`
- Modify: `app/streaming/input/input.cpp`
- Modify: `tests/input_integration_stubs.h`
- Modify: `tests/input_integration_stubs.cpp`
- Modify: `tests/test_inputintegration.cpp`

**Interfaces:**
- Consumes: a physical display number from `PhysicalDisplayController`.
- Produces: `bool SdlInputHandler::sendPhysicalDisplayShortcut(int displayNumber)`.
- Produces test records with `keyCode`, `action`, `modifiers`, and `flags`.

- [ ] **Step 1: Add failing packet-order and gate tests**

Add these slots to `InputIntegrationTest`:

```cpp
void physicalDisplayShortcutUsesBalancedExplicitModifiers();
void physicalDisplayShortcutMapsDisplayThirteenToF13();
void physicalDisplayShortcutBypassesOnlyTheDeckKeyboardGate();
void physicalDisplayShortcutIgnoresCapsLockState();
void invalidPhysicalDisplaySendsNothing();
void packetFailureStillReleasesEveryShortcutKey();
```

The Display 1 expectation is:

```cpp
const QVector<KeyboardRecord> expected {
    {0x8011, KEY_ACTION_DOWN, 0, 0}, // Control
    {0x8012, KEY_ACTION_DOWN, 0, 0}, // Alt
    {0x8010, KEY_ACTION_DOWN, 0, 0}, // Shift
    {0x8070, KEY_ACTION_DOWN, 0, 0}, // F1
    {0x8070, KEY_ACTION_UP,   0, 0},
    {0x8010, KEY_ACTION_UP,   0, 0},
    {0x8012, KEY_ACTION_UP,   0, 0},
    {0x8011, KEY_ACTION_UP,   0, 0},
};
QCOMPARE(InputIntegrationStubs::keyboards(), expected);
```

For Display 13, expect `0x807C` for F13. Call `beginLocalOverlayInput()` before the helper and verify that an ordinary `handleKeyEvent()` remains suppressed while this exact helper sequence is sent. Set `SDL_SetModState(KMOD_CAPS)` for the Caps Lock test and verify that the recorded sequence is unchanged, then restore `KMOD_NONE`.

- [ ] **Step 2: Run the focused test to verify failure**

```bash
make -j"$(nproc)" debug
./tests/perigee-tests InputIntegrationTest -silent
```

Expected: compilation fails because the sender and keyboard records do not exist.

- [ ] **Step 3: Add keyboard packet recording to the test stubs**

Add this type and API:

```cpp
struct KeyboardRecord {
    int keyCode;
    int action;
    int modifiers;
    int flags;
    bool operator==(const KeyboardRecord& other) const {
        return keyCode == other.keyCode && action == other.action &&
            modifiers == other.modifiers && flags == other.flags;
    }
};

QVector<KeyboardRecord> keyboards();
void failKeyboardSendAt(int zeroBasedPacketIndex);
```

Make `LiSendKeyboardEvent2()` append every attempted packet and return `-1` at the configured index. Reset both the records and failure index in `InputIntegrationStubs::reset()`.

- [ ] **Step 4: Implement the fixed shortcut sender**

Declare the helper public because `Session` is its only production caller:

```cpp
bool sendPhysicalDisplayShortcut(int displayNumber);
```

Implement it under `m_RemoteInputMutex`, but deliberately do not apply the `m_LocalOverlayInputActive` early return. Validate 1 through 13 before sending. Use the eight packets shown in Step 1 with no modifier mask and no local SDL modifier state. Continue through all four key-up packets after any send error and return `false` when any packet fails.

Do not put these synthetic keys into `m_RemoteInputState`; the helper owns and balances its complete fixed sequence.

- [ ] **Step 5: Run input and neutralization tests**

```bash
make -j"$(nproc)" debug
./tests/perigee-tests InputIntegrationTest -silent
./tests/perigee-tests InputNeutralizationTest -silent
```

Expected: PASS. Ordinary input remains suppressed while Deck owns input.

- [ ] **Step 6: Commit the transport**

```bash
git add app/streaming/input/input.* tests/input_integration_stubs.* \
        tests/test_inputintegration.cpp
git commit -m "feat: send stock physical display shortcut"
```

### Task 3: Add the bounded display-count preference

**Files:**
- Modify: `app/settings/streamingpreferences.h`
- Modify: `app/settings/streamingpreferences.cpp`
- Modify: `app/gui/perigee/DeckBindingSettings.qml`
- Modify: `tests/test_streamingpreferences.cpp`
- Modify: `tests/test_deckbindingsqml.cpp`

**Interfaces:**
- Produces: `Q_PROPERTY(int deckPhysicalDisplayCount READ deckPhysicalDisplayCount WRITE setDeckPhysicalDisplayCount NOTIFY deckPhysicalDisplayCountChanged)`.
- Produces: persisted key `deckPhysicalDisplayCount`, clamped to 1 through 13, default 3.

- [ ] **Step 1: Add failing persistence and QML contract tests**

Add a preference round-trip test that writes `0`, `3`, and `14` and expects `1`, `3`, and `13` after reload. Add QML source checks for:

```cpp
QVERIFY(qml.contains("objectName: \"deckPhysicalDisplayCount\""));
QVERIFY(qml.contains("from: 1"));
QVERIFY(qml.contains("to: 13"));
QVERIFY(qml.contains("preferences.setDeckPhysicalDisplayCount(value)"));
```

- [ ] **Step 2: Run tests to verify failure**

```bash
make -j"$(nproc)" debug
./tests/perigee-tests StreamingPreferencesTest -silent
./tests/perigee-tests DeckBindingsQmlTest -silent
```

Expected: FAIL because the preference and control are absent.

- [ ] **Step 3: Implement persistence and clamping**

Add:

```cpp
#define SER_DECK_PHYSICAL_DISPLAY_COUNT "deckPhysicalDisplayCount"

m_DeckPhysicalDisplayCount = qBound(
    1, settings.value(SER_DECK_PHYSICAL_DISPLAY_COUNT, 3).toInt(), 13);

settings.setValue(SER_DECK_PHYSICAL_DISPLAY_COUNT,
                  qBound(1, m_DeckPhysicalDisplayCount, 13));
```

Declare `int deckPhysicalDisplayCount() const`, private member `int m_DeckPhysicalDisplayCount`, the property, and `deckPhysicalDisplayCountChanged()`. Use `Q_INVOKABLE bool setDeckPhysicalDisplayCount(int count)` so QML cannot store an invalid value; emit the signal only when the normalized value changes.

- [ ] **Step 4: Add the accessible settings control**

Place this row in `DeckBindingSettings.qml` above Legacy direct disconnect:

```qml
RowLayout {
    Layout.fillWidth: true
    Label {
        Layout.fillWidth: true
        text: qsTr("Physical displays in Deck")
    }
    SpinBox {
        objectName: "deckPhysicalDisplayCount"
        from: 1
        to: 13
        value: root.preferences ? root.preferences.deckPhysicalDisplayCount : 3
        editable: true
        Accessible.name: qsTr("Physical displays in Perigee Deck")
        onValueModified: {
            if (root.preferences)
                root.preferences.setDeckPhysicalDisplayCount(value)
        }
    }
}
```

- [ ] **Step 5: Run focused tests and commit**

```bash
make -j"$(nproc)" debug
./tests/perigee-tests StreamingPreferencesTest -silent
./tests/perigee-tests DeckBindingsQmlTest -silent
git add app/settings/streamingpreferences.* \
        app/gui/perigee/DeckBindingSettings.qml \
        tests/test_streamingpreferences.cpp tests/test_deckbindingsqml.cpp
git commit -m "feat: configure Deck physical display count"
```

### Task 4: Expose physical displays through the local action adapter

**Files:**
- Modify: `app/perigee/actions/sessionfacade.h`
- Modify: `app/perigee/actions/gamestreamadapter.cpp`
- Modify: `tests/test_gamestreamadapter.cpp`
- Modify: `tests/test_deckcontroller.cpp`
- Modify: `tests/test_polarisadapter.cpp`
- Modify: `tests/test_polarisactions.cpp`
- Modify: `tests/test_polarisintegration.cpp`

**Interfaces:**
- Consumes: `PhysicalDisplayController` state through `SessionFacade`.
- Produces: action IDs `display.physical.1` through `display.physical.13` and `display.physical-status`.
- Produces facade methods `physicalDisplayCount()`, `lastRequestedPhysicalDisplay()`, `physicalDisplaySwitchActive()`, and `requestPhysicalDisplay(int, PhysicalDisplayCompletion)`.

- [ ] **Step 1: Add failing descriptor, snapshot, asynchronous, and navigation tests**

Extend the `SessionFacade` fakes in the four adapter/integration test files with safe physical-display state. The GameStream adapter fake stores completion state:

```cpp
int physicalCount = 3;
int lastPhysicalDisplay = 0;
bool physicalSwitchActive = false;
int requestedPhysicalDisplay = 0;
SessionFacade::PhysicalDisplayCompletion physicalCompletion;
```

Verify:

```cpp
QCOMPARE(descriptors.value("display.physical.1").label,
         QStringLiteral("Display 1"));
QCOMPARE(descriptors.value("display.physical.13").label,
         QStringLiteral("Display 13"));
QCOMPARE(descriptors.value("display.physical.1").resourceKey,
         QStringLiteral("display.physical"));
QVERIFY(snapshot.actionStates.value("display.physical.4").visible == false);
QCOMPARE(snapshot.actionStates.value("display.physical-status").value.toString(),
         QStringLiteral("Unknown"));
```

Execute Display 2, assert the action remains `Working`, invoke the stored completion, and then assert `Succeeded`. Add a controller-only Deck test that selects the Display category, reaches all three enabled display entries, and activates Display 2. Keep Deck open after both a success and a `display_verification_timeout` completion, and verify that the existing Disconnect client action remains reachable.

- [ ] **Step 2: Run adapter and Deck tests to verify failure**

```bash
make -j"$(nproc)" debug
./tests/perigee-tests GameStreamAdapterTest -silent
./tests/perigee-tests DeckControllerTest -silent
```

Expected: compilation fails on the new facade methods and action IDs.

- [ ] **Step 3: Extend the session facade**

Add:

```cpp
using PhysicalDisplayCompletion = std::function<void(const ActionResult&)>;

virtual int physicalDisplayCount() const = 0;
virtual int lastRequestedPhysicalDisplay() const = 0;
virtual bool physicalDisplaySwitchActive() const = 0;
virtual bool requestPhysicalDisplay(
    int displayNumber, PhysicalDisplayCompletion completion) = 0;
```

Include `actiontypes.h` and `<functional>` directly.

- [ ] **Step 4: Add static descriptors and truthful snapshot state**

Append one status descriptor and 13 display descriptors in numeric order. Each display descriptor uses category `Display`, aliases `display`, `monitor`, `screen`, and its number, resource key `display.physical`, and no confirmation.

In `snapshot()`:

```cpp
const int count = qBound(1, authority->physicalDisplayCount(), 13);
const int last = authority->lastRequestedPhysicalDisplay();
status.value = last == 0
    ? QStringLiteral("Unknown")
    : QStringLiteral("Last requested: Display %1").arg(last);

entry.visible = number <= count;
entry.enabled = number <= count && !authority->physicalDisplaySwitchActive();
entry.value = number == last ? QStringLiteral("Last requested") : QString();
```

The status row stays disabled with reason `The host does not report an authoritative active physical display.` It is informative and not activatable.

- [ ] **Step 5: Delegate display execution asynchronously**

Parse only canonical decimal IDs in the range 1 through 13. Call `requestPhysicalDisplay(number, completion)` without completing early. On rejection, complete once with:

```cpp
ActionResult {
    false, {},
    QStringLiteral("request_rejected"),
    QStringLiteral("The physical display request could not be queued."),
};
```

Leave ActionRegistry's existing `resourceKey` single-flight rule in control of concurrent display rows.

- [ ] **Step 6: Run focused tests and commit**

```bash
make -j"$(nproc)" debug
./tests/perigee-tests GameStreamAdapterTest -silent
./tests/perigee-tests DeckControllerTest -silent
git add app/perigee/actions/sessionfacade.h \
        app/perigee/actions/gamestreamadapter.cpp \
        tests/test_gamestreamadapter.cpp tests/test_deckcontroller.cpp \
        tests/test_polarisadapter.cpp tests/test_polarisactions.cpp \
        tests/test_polarisintegration.cpp
git commit -m "feat: expose physical displays in Deck"
```

### Task 5: Wire Session, SDL input, and decoded-frame evidence

**Files:**
- Modify: `app/streaming/session.h`
- Modify: `app/streaming/session.cpp`
- Modify: `app/streaming/sdleventcodes.h`
- Modify: `tests/test_sessiondisplaytransition.cpp`

**Interfaces:**
- Consumes: `SdlInputHandler::sendPhysicalDisplayShortcut()` and `PhysicalDisplayController`.
- Produces: the `GameStreamSessionFacade` physical-display methods.
- Produces: SDL user event `SDL_CODE_PERIGEE_POST_SHORTCUT_FRAME` carrying the Session generation and controller evidence epoch.

- [ ] **Step 1: Replace the old source-inspection test with failing integration assertions**

Rename the test class to `SessionPhysicalDisplayTest` and verify the source contains:

```cpp
QVERIFY(codes.contains("SDL_CODE_PERIGEE_POST_SHORTCUT_FRAME"));
QVERIFY(session.contains("sendPhysicalDisplayShortcut(displayNumber)"));
QVERIFY(session.contains("m_PhysicalDisplayController->notifyAcceptedFrame"));
QVERIFY(session.contains("m_PhysicalDisplayController->observeFreshFrame"));
QVERIFY(session.contains("m_PhysicalDisplayController->checkDeadline()"));
QVERIFY(session.contains("m_PhysicalDisplayController->cancel()"));
QVERIFY(session.contains("sessionEpoch == m_PhysicalDisplaySessionEpoch"));
```

The controller unit test must call `notifyAcceptedFrame()` before a request and assert that it does not enqueue. It must then request a display, call `notifyAcceptedFrame()` twice, and assert that only the first call enqueues the current evidence epoch. This proves the gate is armed only after the shortcut sender returns.

- [ ] **Step 2: Run the source integration test to verify failure**

```bash
make -j"$(nproc)" debug
./tests/perigee-tests SessionPhysicalDisplayTest -silent
```

Expected: FAIL on the new source contract.

- [ ] **Step 3: Implement the facade methods and controller construction**

`GameStreamSessionFacade` delegates:

```cpp
int physicalDisplayCount() const override;
int lastRequestedPhysicalDisplay() const override;
bool physicalDisplaySwitchActive() const override;
bool requestPhysicalDisplay(
    int displayNumber, PhysicalDisplayCompletion completion) override;
```

Construct `PhysicalDisplayController` during `Session::initialize()` with a monotonic SDL clock and this sender:

```cpp
[this](int displayNumber) {
    return m_InputHandler != nullptr &&
        m_InputHandler->sendPhysicalDisplayShortcut(displayNumber);
}
```

`PhysicalDisplayController::request()` arms its internal frame gate only after this sender returns `true`.

- [ ] **Step 4: Route fresh decoded-frame evidence back to the SDL thread**

Rename the event code to `SDL_CODE_PERIGEE_POST_SHORTCUT_FRAME`. Give each Session a nonzero `m_PhysicalDisplaySessionEpoch` from a process-wide atomic counter. After `submitDecodeUnit()` returns `DR_OK`, call the controller's thread-safe `notifyAcceptedFrame()` method and let its armed gate enqueue one event with the Session epoch in `data1` and evidence epoch in `data2`. Handle it in the SDL event loop:

```cpp
case SDL_CODE_PERIGEE_POST_SHORTCUT_FRAME: {
    const quint64 sessionEpoch = static_cast<quint64>(
        reinterpret_cast<uintptr_t>(event.user.data1));
    const quint64 evidenceEpoch = static_cast<quint64>(
        reinterpret_cast<uintptr_t>(event.user.data2));
    if (sessionEpoch == m_PhysicalDisplaySessionEpoch &&
            m_PhysicalDisplayController != nullptr) {
        m_PhysicalDisplayController->observeFreshFrame(
            evidenceEpoch);
    }
    break;
}
```

The decoder callback must not invoke Deck, Qt models, or completions directly. Add a source contract assertion that an event from an earlier Session generation cannot complete a request in a replacement Session.

- [ ] **Step 5: Pump deadlines and cancel on teardown**

Call `checkDeadline()` at the start of `pumpDeckUi()` before any renderer early return. Call `cancel()` before destroying the registry, controller, or input handler. Refresh the Deck controller and mark its surface dirty after a terminal transition so the action message and Last requested status appear without closing Deck.

- [ ] **Step 6: Run Session, adapter, Deck, and input tests**

```bash
make -j"$(nproc)" debug
./tests/perigee-tests SessionPhysicalDisplayTest -silent
./tests/perigee-tests PhysicalDisplayControllerTest -silent
./tests/perigee-tests GameStreamAdapterTest -silent
./tests/perigee-tests InputIntegrationTest -silent
```

Expected: PASS.

- [ ] **Step 7: Commit Session integration**

```bash
git add app/streaming/session.* app/streaming/sdleventcodes.h \
        tests/test_sessiondisplaytransition.cpp
git commit -m "feat: verify display requests with fresh video"
```

### Task 6: Remove the obsolete Polaris display-target and reconnect path

**Files:**
- Delete: `app/perigee/display/displaytransaction.h`
- Delete: `app/perigee/display/displaytransaction.cpp`
- Delete: `app/perigee/display/sessiontransitioncoordinator.h`
- Delete: `app/perigee/display/sessiontransitioncoordinator.cpp`
- Delete: `tests/test_displaytransaction.cpp`
- Delete: `tests/test_sessiontransitioncoordinator.cpp`
- Modify: `app/perigee/actions/actionregistry.cpp`
- Modify: `app/perigee/polaris/polarisadapter.h`
- Modify: `app/perigee/polaris/polarisadapter.cpp`
- Modify: `app/perigee/polaris/polarismodels.h`
- Modify: `app/perigee/polaris/polarismodels.cpp`
- Modify: `app/streaming/session.h`
- Modify: `app/streaming/session.cpp`
- Modify: `app/main.cpp`
- Modify: `app/gui/StreamSegue.qml`
- Modify: `app/app.pro`
- Modify: `tests/perigee-tests.pro`
- Modify: `tests/test_actionregistry.cpp`
- Modify: `tests/test_polarismodels.cpp`
- Modify: `tests/test_polarisadapter.cpp`
- Modify: `tests/test_polarisactions.cpp`
- Modify: `tests/test_polarisintegration.cpp`
- Modify: `tests/test_streamsegueqml.cpp`
- Modify: `tests/fixtures/polaris/capabilities-current.json`
- Modify: `tests/fixtures/polaris/client-settings-current.json`

**Interfaces:**
- Consumes: static `display.physical.N` actions from `GameStreamAdapter`.
- Preserves: official Polaris capability, session-status, client-settings, session-stop, and inherited clipboard transport behavior.
- Removes: `DisplayTarget`, `DisplayTransaction`, `SessionTransitionCoordinator`, `PolarisOperation::DisplaySwitch`, `display_targets_v1`, `display.target.*`, and display-target POST methods.

- [ ] **Step 1: Add failing stock-contract boundary tests**

Change tests to assert:

```cpp
QVERIFY(!capabilities.features.contains(QStringLiteral("display_targets_v1")));
QVERIFY(!adapterSnapshot.actionStates.contains(QStringLiteral("display.switch")));
QVERIFY(adapterSnapshot.actionStates.contains(QStringLiteral("display.physical.1")));
```

Update the current fixtures to match official Polaris fields only. Keep `/polaris/v1/capabilities`, `/polaris/v1/session/status`, and `/polaris/v1/client-settings`. Remove the invented display catalog, selection endpoint, and current physical target.

Replace StreamSegue tests with negative assertions:

```cpp
QVERIFY(!qml.contains("DisplayTransitionCoordinator"));
QVERIFY(!qml.contains("displayRecoverySurface"));
QVERIFY(!qml.contains("createDisplayTransitionReplacement"));
```

- [ ] **Step 2: Run the focused boundary tests to verify failure**

```bash
make -j"$(nproc)" debug
./tests/perigee-tests PolarisModelsTest -silent
./tests/perigee-tests PolarisAdapterTest -silent
./tests/perigee-tests PolarisActionsTest -silent
./tests/perigee-tests PolarisIntegrationTest -silent
./tests/perigee-tests StreamSegueQmlTest -silent
```

Expected: FAIL while the obsolete catalog and transition surface remain.

- [ ] **Step 3: Remove the fork-only model and adapter operations**

Delete the `DisplayTarget` type, the `PolarisOperation::DisplaySwitch` enumerator, both `PolarisAdapter::postDisplayTarget` overloads, the `PolarisAdapter::ActionKind::DisplayTarget` enumerator, and the `PolarisAdapter::m_TransitionCoordinator` member.

In `PolarisAdapter::snapshot()`, begin with `m_LocalAdapter.snapshot()` and do not add a `display.switch` template or `display.target.*` states. In `execute()`, local action detection continues to delegate every `GameStreamAdapter::descriptor()`, including the new physical display rows.

Retain official client-settings parsing for `stream_display_mode`, `display_mode`, bitrate, adaptive state, device capabilities, runtime, and resume timeout. Do not interpret these stream-mode fields as physical monitors.

- [ ] **Step 4: Remove dynamic display resolution from ActionRegistry**

Delete `DisplayTemplateId`, `DisplayPrefix`, `displayActionId()`, `displayMetadata()`, the `displayTemplate` branch in `resolvedDescriptors()`, and its tests. Keep the existing `host.command` dynamic-resolution code unchanged in this task.

- [ ] **Step 5: Remove application-wide reconnect and recovery UI**

Delete the global `SessionTransitionCoordinator` construction and `DisplayTransitionCoordinator` context property from `main.cpp`. Remove display-handoff properties, replacement Session methods, recovery-carrier timers, related connections, and `displayRecoverySurface` from `StreamSegue.qml` and `Session`.

Normal stream completion must again follow the inherited single-Session cleanup path. A failed in-stream physical request stays in Deck and does not replace or restart the streaming Session.

- [ ] **Step 6: Update qmake inputs and delete obsolete tests**

Remove the four obsolete display source/header files and two obsolete test sources from `app/app.pro` and `tests/perigee-tests.pro`. Add `physicaldisplaycontroller.cpp`, `physicaldisplaycontroller.h`, and `test_physicaldisplaycontroller.cpp` to both relevant targets.

- [ ] **Step 7: Run the boundary suite and scan for stale symbols**

```bash
make -j"$(nproc)" debug
./tests/perigee-tests ActionRegistryTest -silent
./tests/perigee-tests PolarisModelsTest -silent
./tests/perigee-tests PolarisAdapterTest -silent
./tests/perigee-tests PolarisActionsTest -silent
./tests/perigee-tests PolarisIntegrationTest -silent
./tests/perigee-tests StreamSegueQmlTest -silent
rg -n "display_targets_v1|DisplayTarget|DisplayTransaction|SessionTransitionCoordinator|display\.target\.|DisplayTransitionCoordinator" app
```

Expected: all tests PASS and `rg` returns no matches.

- [ ] **Step 8: Commit the obsolete-path removal**

```bash
git add -A app tests
git commit -m "refactor: remove fork-only display target path"
```

### Task 7: Correct the README and acceptance documentation

**Files:**
- Modify: `README.md`
- Modify: `docs/testing/live-acceptance-2026-08.md`
- Modify: `docs/upstream-pins.md`

**Interfaces:**
- Consumes: actual implemented behavior and verification output from Tasks 1 through 6.
- Produces: accurate pre-release documentation with no companion-fork dependency.

- [ ] **Step 1: Add a documentation contract check**

Run this before editing and save the matches in the implementation notes:

```bash
rg -n "companion|WonderWerks Polaris|display target|target readback|rollback|display_targets_v1|named commands" \
  README.md docs/testing/live-acceptance-2026-08.md docs/upstream-pins.md
```

Expected: stale fork-only claims are present.

- [ ] **Step 2: Rewrite the README feature and compatibility sections**

Use this factual core copy:

```markdown
- Select a physical host display from Deck with a keyboard, pointer, or controller.
- Send the stock GameStream display shortcut. Perigee does not require a modified Polaris or Sunshine host.
- Mark a display as **Last requested** only after a fresh video frame arrives.
- Keep Deck open when video verification times out. Perigee does not scan displays or claim authoritative host readback.
```

Replace the companion-fork paragraph with:

```markdown
Perigee uses only capabilities that the connected Polaris host advertises. Physical display selection uses the GameStream input channel and does not require a Polaris API extension. The client treats Polaris stream-display modes, virtual displays, and physical monitor indexes as different concepts.
```

Do not describe named commands, clipboard transfer, or other enhanced actions as complete unless their current focused and live acceptance rows are green. Keep their source attribution and future direction separate from the implemented-feature list.

- [ ] **Step 3: Update upstream pins and the live ledger**

Record the official Polaris source commit used for the contract review:

```text
73014f91dc64b5510fec84f2f7eaf9f67065dc94
```

Add separate ledger rows for:

```markdown
| Standard Sunshine stream regression | Pending live run | Core stream and local Deck only |
| Stock Polaris Display 1 | Pending live run | Balanced shortcut plus fresh frame |
| Stock Polaris Display 2 | Pending live run | Balanced shortcut plus fresh frame |
| Stock Polaris Display 3 | Pending live run | Balanced shortcut plus fresh frame |
| Fullscreen controller selection | Pending live run | Deck remains open and responsive |
| Verification timeout | Automated only | No display scan; bounded restoration rules |
```

Change only `Pending live run` rows after a real observation.

- [ ] **Step 4: Verify documentation and commit**

```bash
rg -n "companion|WonderWerks Polaris|display_targets_v1|authoritative active display" \
  README.md docs/testing/live-acceptance-2026-08.md docs/upstream-pins.md
git diff --check
git add README.md docs/testing/live-acceptance-2026-08.md docs/upstream-pins.md
git commit -m "docs: describe stock host display switching"
```

Expected: no obsolete requirement remains; any use of `authoritative` explains that Perigee lacks physical-display authority.

### Task 8: Run the complete automated verification gate

**Files:**
- Modify only when a test exposes a defect in an earlier task.

**Interfaces:**
- Consumes: the complete implementation.
- Produces: a clean build, complete test result, and stale-contract scan.

- [ ] **Step 1: Configure a cold debug test build**

From the repository root:

```bash
mkdir -p build
cd build
qmake6 ../moonlight-qt.pro CONFIG+=debug CONFIG+=perigee-tests
make -j"$(nproc)" debug
```

Expected: the Perigee application and `tests/perigee-tests` build without deleted display-transition objects.

- [ ] **Step 2: Run the focused display and Deck suite**

```bash
./tests/perigee-tests PhysicalDisplayControllerTest -silent
./tests/perigee-tests InputIntegrationTest -silent
./tests/perigee-tests GameStreamAdapterTest -silent
./tests/perigee-tests DeckControllerTest -silent
./tests/perigee-tests SessionPhysicalDisplayTest -silent
./tests/perigee-tests StreamingPreferencesTest -silent
./tests/perigee-tests DeckBindingsQmlTest -silent
```

Expected: PASS.

- [ ] **Step 3: Run the complete graphical suite**

```bash
xvfb-run -a -s "-screen 0 1280x720x24" \
  env LIBGL_ALWAYS_SOFTWARE=1 QT_QPA_PLATFORM=xcb SDL_VIDEODRIVER=dummy \
  ./tests/perigee-tests -silent
```

Expected: PASS with no crash, hang, duplicate completion, or QML warning promoted to failure.

- [ ] **Step 4: Run source, documentation, and diff checks**

```bash
cd ..
rg -n "display_targets_v1|DisplayTarget|DisplayTransaction|SessionTransitionCoordinator|display\.target\.|DisplayTransitionCoordinator|WonderWerksSoftware/polaris" \
  app README.md docs/testing docs/upstream-pins.md
git diff --check
git status --short
```

Expected: the stale-symbol scan returns no active implementation or README matches. Historical design documents can contain a clearly marked superseded statement. `git diff --check` is silent.

- [ ] **Step 5: Route any verification correction back to its owning task**

If verification exposes a defect, return to the task that owns that file, add a regression assertion to that task's focused test, make the smallest correction, rerun the focused and complete gates, and use that task's explicit `git add` list. If no correction is required, do not create an empty commit.

### Task 9: Run the two-host live acceptance matrix

**Files:**
- Modify: `docs/testing/live-acceptance-2026-08.md`

**Interfaces:**
- Consumes: the verified Perigee build, the existing Sunshine dev host, and the existing Polaris workstation host.
- Produces: human-observed acceptance evidence without host modification.

- [ ] **Step 1: Record the exact client and host identities without changing either host**

Record:

```bash
git rev-parse HEAD
./build/app/perigee --version
```

For each host, record its reported software name and version through existing UI or read-only service output. Do not install, update, stop, enable, disable, or reconfigure host software as part of this task.

- [ ] **Step 2: Run the Sunshine regression**

From the Intel Yoga client, connect to the existing Sunshine dev host. Verify video, audio, keyboard, pointer, controller, Deck open/close, fullscreen/windowed, statistics, and client disconnect. Do not claim physical switching on Sunshine unless the observed host actually changes displays.

- [ ] **Step 3: Run the three-display Polaris test**

Connect to the existing Polaris workstation. In windowed mode, use Deck to request Display 1, Display 2, and Display 3 by pointer and controller. Repeat in fullscreen mode. For each request, record:

```text
display number | input method | client mode | fresh frame received | visible monitor content matched | Deck responsive
```

The visible-content observation is human evidence, not API readback.

- [ ] **Step 4: Exercise the bounded failure path**

Select one configured index that is not available on the host. Verify that Deck stays open, the action times out once, Perigee does not scan other indexes, and restoration occurs only when a prior successful display is known.

- [ ] **Step 5: Update the ledger and commit only observed results**

Replace pending rows with PASS or FAIL and include the client commit, host type/version, mode, input method, and evidence summary. Do not include addresses, credentials, certificates, or pairing material.

```bash
git add docs/testing/live-acceptance-2026-08.md
git commit -m "test: record physical display live acceptance"
```

## Completion Gate

The implementation is complete only when:

1. Every focused and complete automated test passes.
2. The obsolete server-mutation display path is absent from active code.
3. The README describes the stock-host path and no longer requires a WonderWerks Polaris fork.
4. The existing Sunshine host still passes the core stream regression.
5. Display 1, Display 2, and Display 3 pass a real Polaris human test in windowed and fullscreen modes.
6. Keyboard, pointer, and controller can operate the display actions.
7. Failure produces no scan loop, stuck key, closed Deck, or false active-display claim.

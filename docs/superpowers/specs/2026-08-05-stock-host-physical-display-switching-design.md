# Stock-Host Physical Display Switching and Polaris Integration Design

**Date:** 2026-08-05

**Status:** Proposed; design direction approved

**Supersedes:** The physical-display design and Polaris-fork requirement in the 2026-08-02 Perigee client design

## 1. Purpose

Perigee must let a user select a physical host display from Deck. The action must work with an unmodified Polaris host. It must also preserve normal streaming with an unmodified Sunshine host.

This design does not add a Perigee server component. It does not require a Polaris fork. Perigee sends the same GameStream keyboard shortcut that stock Polaris already accepts for physical display selection.

The design also records the parts of Nova that can guide later Perigee features. These parts are reference behavior and API contracts. Nova is not a Perigee code dependency.

## 2. Compatibility Boundary

The following rules are mandatory:

1. Perigee maintains only the Perigee client repository.
2. Perigee does not require changes to Polaris or Sunshine.
3. Physical display selection uses the GameStream input path.
4. Polaris API features are optional enhancements. Their absence cannot break the standard streaming path.
5. Perigee does not claim that it knows the active physical display when the host does not supply that information.
6. Future Polaris display metadata can improve the user interface only when the host advertises a stable capability.

## 3. Existing Host Mechanism

Stock Polaris tracks the state of Control, Alt, and Shift. It maps `Ctrl+Alt+Shift+F1` through `Ctrl+Alt+Shift+F13` to physical display indexes 0 through 12.

Perigee must send a balanced keyboard event sequence through the existing GameStream input channel:

1. Control down.
2. Alt down.
3. Shift down.
4. The selected function key down.
5. The selected function key up.
6. Shift up.
7. Alt up.
8. Control up.

Perigee must send explicit modifier events. A function-key event that contains modifier flags is not sufficient because Polaris uses its tracked modifier state to recognize the shortcut.

The action must use the stream input thread and the normal Moonlight keyboard packet path. It must not call platform input injection tools, run a host command, or use an unauthenticated network request.

## 4. Deck Behavior

### 4.1 Display list

Deck shows a physical-display submenu with generic entries:

- Display 1
- Display 2
- Display 3
- additional entries up to the configured limit, with a protocol maximum of 13

The initial default can match the user's known three-display workstation. The setting must remain adjustable because stock Polaris does not expose a paired-client physical-display catalog.

Perigee can store local aliases for each host and index, for example `Center`, `Left`, or `Portrait`. An alias is client-local information. It is not host state.

### 4.2 Current-state labels

Before the first successful request in a stream, Deck shows the current physical display as unknown.

After a successful request, Deck can mark the selection as **Last requested**. It must not mark the entry as an authoritative active display unless a future advertised host capability supplies that state.

### 4.3 Input ownership

Deck owns local keyboard, pointer, and controller input while it is open. The display action is an explicit exception that can send only its fixed host shortcut sequence.

The exception must:

- run on the correct stream input thread;
- send only the selected display shortcut;
- balance all key-down and key-up events;
- ignore the local Caps Lock state;
- preserve Deck input suppression before and after the sequence;
- prevent concurrent display shortcut sequences;
- release the sequence safely if the stream ends during the action.

Ordinary user keystrokes must remain local while Deck is open.

## 5. Action Lifecycle

1. The user opens Deck with the configured keyboard shortcut or controller chord.
2. The user selects a physical display.
3. Perigee rechecks the stream state and the single-flight display-action guard.
4. Perigee sends the balanced host shortcut sequence.
5. Perigee waits for a fresh decoded video frame after the completed shortcut.
6. On a fresh frame, Perigee records the index as the last requested display for this stream and reports that the request completed.
7. On timeout or stream failure, Deck remains open and reports that it could not verify resumed video.

A fresh decoded frame proves that video resumed after the request. It does not prove the physical identity of the frame. The user interface and logs must keep that distinction clear.

The timeout state offers these actions:

- Retry the same display.
- Select another display.
- Return to Deck.
- Disconnect the client.

Perigee must not report success only because it sent the keyboard packets.

## 6. Recovery Limits

Perigee does not know the initial physical display on a stock host. It therefore cannot promise automatic restoration after the first failed selection.

Perigee can attempt one restoration only when the current stream has a previously verified last-requested index. The restoration uses the same shortcut and fresh-frame evidence. If the restoration does not produce a fresh frame, Deck remains open and keeps the error visible.

Perigee must not loop between display indexes. It must not scan all displays to find video. It must not send repeated shortcuts without a new user action or the one bounded restoration attempt.

## 7. Preview Images

Stock Polaris and Sunshine do not provide a paired-client API that returns a live preview for each physical display. Perigee must not switch through the host displays to manufacture a preview grid.

Perigee can later store a local **last-seen preview** after a successful display request. The preview must:

- use a decoded client frame;
- remain local to Perigee;
- show the local alias or generic display index;
- show a capture time;
- use the label **Last seen**, not **Live**;
- expire or become visibly stale according to a documented policy;
- never be treated as proof of the current host display.

Preview storage is not required for the first physical-switch implementation.

## 8. Optional Future Polaris Capability

Perigee can consume a future Polaris capability for physical display metadata when upstream Polaris advertises it. A useful contract would include:

- a stable display identifier;
- a user-facing display name;
- connection and availability state;
- current-selection state with defined authority;
- an optional preview reference with age and privacy rules;
- a capability version.

This capability can also help Nova and other Polaris clients. Perigee must fall back to the stock shortcut flow when the capability is absent. It must not use a version string as proof that the capability exists.

## 9. Nova Reference Analysis

Nova is primarily an Android client built from Artemis and Moonlight Android lineage. It uses Moonlight Common C for the streaming protocol. The current public repository has a new Git history rather than a mergeable Moonlight or Artemis fork history. Perigee must therefore use Nova as a behavior and API reference, not as a source branch.

The reusable Nova patterns are:

### 9.1 Capability discovery

Nova queries `/polaris/v1/capabilities` and enables enhanced controls only when Polaris advertises support. Perigee should retain this capability-driven boundary and avoid host-name or version-string guesses.

### 9.2 Session truth

Nova queries `/polaris/v1/session/status` for session ownership, viewer state, display mode class, capture and encoder state, tuning state, and health information. Perigee can use these fields for a later session panel and stream-health display.

These fields do not currently provide a physical display catalog or authoritative physical display index.

### 9.3 Client settings

Nova uses `/polaris/v1/client-settings` for advertised client settings such as stream display mode, display override, bitrate, adaptive behavior, device capabilities, and resume timeout. Perigee can use the official fields in a later settings surface.

Perigee must remove assumptions about nonstandard fields such as `display_targets_v1` from its production Polaris contract.

### 9.4 Live session controls

Nova uses focused Polaris operations for bitrate, adaptive bitrate, the AI optimizer, cursor state, reports, and optimization profiles. These operations are candidates for later Deck actions after the core local and display actions are stable.

### 9.5 Session lifecycle and resilience

Nova distinguishes the session owner, viewer, watch, resume, disconnect, and end-session cases. After a stream drop, it can query session state before it decides whether to resume. Perigee should use this model for a later recovery feature instead of treating each disconnect as an ended host session.

### 9.6 Event updates

Nova contains a Polaris server-sent event client for session and state updates. Perigee can evaluate this mechanism after it verifies the current upstream authentication and capability contract. Polling remains the safe fallback.

### 9.7 Library and artwork

Nova uses Polaris game, cover, artwork, launch-mode, and per-game setting operations. These operations can support a future Perigee desktop library. They are outside the physical-display task.

### 9.8 Code that Perigee must not reuse directly

Nova's Kotlin user interface, Android input code, MediaCodec path, dual-screen role composer, and companion-device behavior do not map directly to Perigee's C++ and Qt architecture. Nova's dual-screen feature routes Android surfaces; it does not select a physical host monitor. Its artwork previews are not host-display previews.

## 10. Code and Documentation Migration

The implementation must remove the obsolete companion-Polaris-fork direction from active Perigee behavior and documentation.

Required changes include:

- replace `display_targets_v1` production assumptions with the stock shortcut action;
- keep any old fork-only fixtures only when they are clearly marked as historical test data, or remove them;
- remove claims that physical display selection has authoritative Polaris readback;
- remove the WonderWerks Polaris fork as a Perigee requirement;
- update the README in controlled, clear language;
- update the live-acceptance ledger with separate Sunshine and Polaris results;
- preserve the standard Sunshine path and existing local Deck actions.

The implementation must not delete unrelated Polaris features only because physical display selection does not use the Polaris API. Each feature must be checked against the current official Polaris contract before it is kept, corrected, or removed.

## 11. Testing

### 11.1 Unit tests

Tests must verify:

- the exact key-down and key-up order for Display 1 through Display 13;
- no packet is sent for an invalid index;
- every partial sequence has a safe release path;
- the action works while Deck suppresses ordinary host input;
- unrelated keyboard, mouse, and controller input remains suppressed;
- Caps Lock does not change the sequence;
- only one display action can run at a time;
- a fresh post-action frame completes the action;
- a stale frame does not complete the action;
- timeout and disconnect return truthful errors;
- restoration is unavailable before a known successful request;
- one bounded restoration attempt occurs when a known target exists.

### 11.2 Deck tests

Keyboard, mouse, and controller tests must reach and activate every configured display entry. Tests must verify focus, search aliases, pointer activation, scaling, confirmation behavior, and windowed/fullscreen parity.

### 11.3 Live acceptance

The live matrix must include:

1. Perigee to unmodified Sunshine for streaming and local Deck regression.
2. Perigee to unmodified current Polaris for physical display switching.
3. The three physical displays on the reference workstation.
4. Windowed and fullscreen client modes.
5. Keyboard, pointer, and controller selection.
6. Stable video and a controlled low-bandwidth case.
7. An invalid or unavailable display index with no retry loop.
8. Repeated Deck open, switch, and close cycles without stuck input.

The acceptance record must identify the client build, host type, host version, display index, result, and evidence. It must not contain credentials or private host data.

## 12. Out of Scope

This change does not include:

- a Polaris fork or Perigee host daemon;
- physical display names supplied by the host;
- authoritative physical display readback;
- live preview images for all host displays;
- concurrent display streams;
- automatic display scanning;
- Nova's Android dual-screen feature;
- the full Nova library, optimizer, or HUD feature set.

## 13. Acceptance Criteria

The feature is ready for a human acceptance test when:

1. A failing test was added for each new behavior before the implementation.
2. Deck sends the correct balanced stock Polaris shortcut for each configured display.
3. Deck remains responsive and owns local input during the action.
4. The result uses fresh-frame evidence and truthful wording.
5. No Polaris or Sunshine modification is required.
6. The standard Sunshine streaming path still passes its regression tests.
7. The README and live-acceptance documentation describe the implemented behavior without fork-only claims.
8. Focused tests, the complete applicable suite, and `git diff --check` pass.

## 14. Source Projects

- Moonlight Qt: <https://github.com/moonlight-stream/moonlight-qt>
- Polaris: <https://github.com/papi-ux/polaris>
- Nova: <https://github.com/papi-ux/nova>
- Artemis: <https://github.com/ClassicOldSong/moonlight-android>

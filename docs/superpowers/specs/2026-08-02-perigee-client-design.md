# Perigee Client Design

**Date:** 2026-08-02

**Status:** Design approved; written specification awaiting user review

**Working tagline:** Moonlight, brought closer.

## 1. Purpose

Perigee is a Polaris-first remote gaming and workstation client built from the current Moonlight Qt codebase. It adds a Parsec-like in-stream control surface so a user can manage the session through one keyboard shortcut or one controller chord instead of memorizing Moonlight's collection of direct shortcuts.

The first release targets the user's Nobara Linux, KDE Plasma, and Wayland environment with Polaris as the reference server. The streaming path remains compatible with ordinary Sunshine/GameStream hosts so Perigee can retain upstream Moonlight behavior and remain useful to other users.

## 2. Product Principles

1. **Polaris-first, GameStream-compatible.** Polaris drives feature development and release acceptance. Ordinary Sunshine support is a regression boundary, not the product priority.
2. **Current Moonlight foundation.** Fork `moonlight-stream/moonlight-qt` rather than reviving an older client.
3. **Small upstream delta.** Keep the stream engine intact wherever possible and place Perigee behavior behind focused interfaces and QML components.
4. **Capability-driven controls.** The client displays only truthful state and never reports success merely because it sent a request.
5. **One menu, every input method.** Keyboard, mouse, and controller users can reach all core actions.
6. **No hidden remote shell.** Server commands are named, advertised, permission-checked actions rather than free-form command execution.
7. **No premature server fork.** Use upstream Polaris first. Add a narrow Perigee Bridge or Polaris patch only after a concrete missing capability is demonstrated.

## 3. Upstream and Reference Strategy

### Primary foundation

- Fork the current `moonlight-stream/moonlight-qt` default branch.
- Preserve Moonlight's decoding, rendering, audio, input, pairing, transport, and ordinary host behavior.
- Regularly merge upstream Moonlight changes into Perigee. Keep Perigee commits organized by subsystem so conflicts remain localized.

### Polaris integration target

- Treat `papi-ux/polaris` as the primary server implementation.
- Use Polaris's authenticated client API and advertised capability, permission, endpoint, and session metadata.
- Require live Polaris validation before a host-facing v0.1 feature is considered complete.

### Donor and protocol references

- Use `wjbeckett/artemis` only as a UX and feature-inventory donor. Do not inherit its unverified endpoints, simulated success paths, or legacy build structure.
- Use `papi-ux/nova` as a Polaris protocol and behavior reference, not as the Perigee codebase.

## 4. Scope

### v0.1 goals

- Open and close an in-stream Perigee Deck using one configurable keyboard shortcut or controller chord.
- Provide a compact top Search Rail that leaves most of the remote desktop visible.
- Support every useful Artemis quick-menu action through real Moonlight or Polaris operations:
  - disconnect the client while leaving the host session running;
  - quit Perigee;
  - run named server commands advertised and permitted by Polaris;
  - send the local text clipboard to the host;
  - fetch the remote text clipboard;
  - toggle streaming statistics;
  - toggle mouse capture;
  - toggle keyboard capture;
  - toggle fullscreen/windowed mode.
- Enumerate real Polaris display/session targets and switch the active streamed display.
- Report current state, permissions, progress, verified success, and actionable failure reasons.
- Preserve normal Moonlight keyboard shortcuts, gamepad passthrough, force feedback, motion controls, and multi-controller behavior while Deck is closed.
- Produce an x86_64 AppImage and an unpacked Linux tarball.

### Explicit v0.1 non-goals

- Simultaneously streaming multiple remote displays into multiple local windows.
- Rewriting Moonlight's decoder, renderer, transport, or pairing stack.
- A general-purpose remote shell or arbitrary command input.
- A permanent Polaris fork before a missing capability is proven.
- Flatpak packaging before controller, input-capture, and Wayland behavior is proven outside a sandbox.
- Full Windows or macOS release qualification. Platform boundaries must remain clean enough for later ports.
- Exposing Polaris AI optimization, bitrate tuning, or other advanced host settings in the first Deck surface. Those can follow after the session-control foundation is reliable.

## 5. Architecture

### 5.1 Moonlight streaming core

The current Moonlight Qt streaming engine remains the authority for video, audio, client input, pairing, connection lifecycle, and local presentation state. Perigee changes this layer only where an explicit hook is required to suspend input forwarding, surface state, invoke existing local actions, or render Deck with the stream window.

### 5.2 Perigee Deck

Deck is an in-stream QML overlay owned by the streaming window. Keeping it in the same process and window allows it to remain above accelerated video under KDE Wayland and fullscreen modes without relying on a separate always-on-top window.

Its default presentation is the approved **Search Rail**:

- a compact rail centered at the top of the stream;
- a search field focused immediately when opened from the keyboard;
- Display, Input, Clipboard, Stats, Window, and Session categories;
- a small tray below the rail containing only the active category or search results;
- visible focus, current values, disabled reasons, progress, and confirmation state;
- no permanent side panel and no full-screen takeover.

### 5.3 Action registry

Deck renders actions from a registry rather than directly calling Moonlight or Polaris code. Each action declares:

- stable identifier, label, category, and search aliases;
- current state and whether readback is supported;
- required host capability and permission;
- responsible adapter;
- execution and cancellation behavior;
- success evidence;
- confirmation policy;
- user-facing unavailable and failure messages.

This boundary allows QML to remain presentation-focused and lets local, standard-host, and Polaris actions share one interface.

### 5.4 Host adapters

`HostAdapter` is the narrow interface used by the action registry.

- `GameStreamAdapter` supplies the ordinary session operations supported by Moonlight and standard hosts.
- `PolarisAdapter` extends that behavior with paired-client HTTPS API calls, capability discovery, permissions, session state, clipboard actions, named commands, and display/session switching.

Polaris data is authoritative for Polaris-specific state. The adapter consumes advertised endpoints, such as a server-provided stop endpoint, instead of assuming fixed routes when the capability response provides the route.

### 5.5 Optional Perigee Bridge

Bridge is not part of the initial implementation. It may be introduced only when a required v0.1 capability cannot be expressed through upstream Polaris.

If required, Bridge must:

- expose only the missing narrow operations;
- authenticate with the paired client identity or an equivalently strong client certificate mechanism;
- enforce per-client permissions;
- accept structured, allowlisted operations;
- never expose arbitrary shell execution;
- be independently removable when upstream Polaris gains the capability.

## 6. Interaction Design

### 6.1 Opening and closing Deck

- Default keyboard shortcut: `Ctrl+Alt+Shift+Space`.
- Default controller chord: `LB+RB+Back+Start`.
- Both bindings are configurable.
- The controller chord opens Deck by default instead of immediately disconnecting. A **Legacy direct disconnect** setting restores Moonlight's original direct behavior.
- Moonlight's direct statistics chord, `LB+RB+Back+X`, remains available.
- Existing Moonlight keyboard shortcuts remain available and are not removed by Deck.

When Deck opens, Perigee:

1. records current mouse and keyboard capture settings;
2. sends neutral keyboard, mouse-button, and controller states to prevent stuck input;
3. pauses forwarding of new remote input;
4. gives local focus to Deck;
5. refreshes local and host state.

Closing Deck restores the recorded capture settings and resumes forwarding from a clean neutral state. Invoking **Release captured input** deliberately changes the restored state so control remains local after Deck closes.

### 6.2 Keyboard and mouse navigation

- Opening by keyboard focuses search immediately.
- Search matches action names, categories, and aliases such as monitor, screen, and display.
- Arrow keys move through results, `Enter` invokes the focused action, and `Escape` backs out of a confirmation/submenu or closes Deck.
- Mouse users can select categories, actions, toggles, and confirmations directly.

### 6.3 Controller navigation

- The controller that opens Deck owns menu focus until Deck closes.
- D-pad and left stick move focus.
- `A` activates the focused item.
- `B` returns to the previous level or closes Deck.
- `LB` and `RB` move between categories while Deck is open.
- `Y` focuses search and requests the platform text-input interface when one is available.
- Every core action remains reachable through categories without typing.
- Button glyphs follow the detected Xbox, PlayStation, or Nintendo layout.
- Other connected controllers are neutralized and suppressed while Deck is open so local players cannot unintentionally control the remote application behind the menu.

### 6.4 Capability presentation

Core Perigee actions remain visible for discoverability. If an action is supported in principle but unavailable, it is disabled with a plain reason such as:

- Polaris is unreachable;
- the installed Polaris version does not advertise the feature;
- this paired client lacks permission;
- no alternate display is available;
- the current session is transitioning.

Vendor-specific optional actions that have no meaning for the connected host are omitted. Perigee never invents support based only on server identity or version strings.

### 6.5 Confirmation policy

- Local toggles, display selection, clipboard transfer, and statistics changes execute without an extra confirmation.
- Disconnect client, quit Perigee, and end host session require explicit confirmation.
- Named host commands require confirmation when their advertised metadata marks them as disruptive or destructive.
- End-session copy clearly distinguishes stopping the Polaris host session from merely disconnecting the client.

## 7. Multi-Display Behavior

v0.1 switches one active streamed display at a time. Deck lists only targets learned from real Polaris session/capability data and clearly marks the current target.

For an in-place switch, Perigee requests the new target and waits for Polaris state plus a decoded frame from that target before reporting success. If the server requires a stream reconnection, Deck remains locally available, shows the transition, reconnects through the normal Moonlight session path, and waits for the first decoded frame.

Before switching, Perigee records the last working target. If the switch or reconnect fails, it attempts to restore that target. If restoration also fails, Perigee keeps Deck open, preserves diagnostic context, and offers retry, disconnect, or return-to-host actions rather than leaving a blank unresponsive stream.

## 8. Action Lifecycle and Data Flow

1. A hotkey or controller chord opens Deck at the client input boundary.
2. Perigee neutralizes and suspends remote input forwarding.
3. Deck requests a snapshot from the action registry.
4. The registry combines Moonlight stream/window/input state with the active adapter's capabilities, permissions, and session state.
5. Deck renders categories, values, enabled state, and reasons.
6. When the user invokes an action, the registry rechecks its preconditions and sets it to working.
7. The responsible adapter performs the local operation or authenticated host request.
8. The adapter obtains the declared success evidence:
   - local state readback for Moonlight actions;
   - Polaris session/capability state readback for remote state changes;
   - an explicit authenticated server acknowledgement only for operations that provide no safe readback, such as one-way clipboard delivery.
9. The registry reports success or a structured failure and refreshes dependent actions.
10. Closing Deck restores the intended capture state and resumes remote input.

Actions use a single-flight policy per resource. For example, a second display switch cannot begin while the first is unresolved, but an unrelated local statistics toggle may proceed.

## 9. Failure Handling

- If Polaris becomes unreachable, local Moonlight actions continue to work and host actions become disabled with the connection error and retry option.
- If a host lacks Polaris capabilities, the adapter falls back to the standard GameStream action set without sending guessed Polaris requests.
- Authentication and permission failures remain distinct from network failures.
- Malformed or incomplete server responses fail closed and are logged without sensitive payloads.
- Disconnects during an action cancel pending UI work, preserve the actual connection state, and cannot leave Deck referencing destroyed session objects.
- Failed display switches follow the restoration behavior in Section 7.
- Clipboard v0.1 accepts UTF-8 text only. Transfers are limited to the smaller of the host-advertised limit and 1 MiB; when the host advertises no limit, the 1 MiB client limit applies. Empty, oversized, unavailable, and permission-denied states receive distinct messages.
- Named commands have no free-form command or argument field in v0.1. Perigee submits only the identifier and structured values explicitly described by the server's advertised command schema.
- Perigee logs action identifiers, state transitions, timings, and error classes. It does not log clipboard contents, pairing keys, client certificates, bearer material, or sensitive command values.

## 10. Security Boundaries

- Reuse Moonlight/Polaris pairing identity and authenticated TLS rather than introducing a parallel password store.
- Validate the expected paired host identity for Polaris API traffic.
- Treat capabilities and permissions as authorization inputs, not merely presentation hints.
- Recheck authorization at execution time because state may change after Deck opens.
- Do not expose a new public listener in the client.
- Do not silently fall back from an authenticated Polaris action to an unauthenticated mechanism.
- Keep clipboard contents in memory only for the transfer lifetime.
- Redact sensitive fields in normal logs, crash context, and diagnostics exported for bug reports.

## 11. Testing Strategy

### 11.1 Unit tests

- Action registry filtering, aliases, capability and permission evaluation, confirmation policy, single-flight behavior, and structured errors.
- `GameStreamAdapter` and `PolarisAdapter` behavior using fixed capability/session fixtures.
- Input-neutralization and capture-state restoration state machines.
- Controller-layout detection and logical-to-visual button-label mapping.
- Log-redaction rules for clipboard, credentials, certificates, and sensitive command fields.

### 11.2 UI tests

- Search Rail focus order and active-category behavior.
- Full action reachability using keyboard only and controller only.
- D-pad/left-stick navigation, `A`, `B`, `LB/RB`, and `Y` behavior.
- Confirmation dialogs and disabled-reason presentation.
- Dynamic Xbox, PlayStation, and Nintendo glyph changes.
- Repeated open/close cycles without focus loss or stale state.

### 11.3 Polaris integration harness

Use a local authenticated fake Polaris service to cover:

- current, older, partial, and malformed capability responses;
- allowed and denied permissions;
- timeouts, disconnects, TLS/authentication failures, and non-success responses;
- state readback that succeeds, fails, or disagrees with the requested state;
- clipboard limits and acknowledgements;
- display switches that are in-place, require reconnection, fail, or require restoration;
- named command metadata and risk confirmation.

The fake service is a test harness, not a production replacement for Polaris.

### 11.4 Live acceptance matrix

The primary release environment is Nobara Linux with KDE Plasma on Wayland connected to upstream Polaris. Acceptance covers:

- physical, virtual, and headless display targets available in the test environment;
- windowed and fullscreen modes;
- keyboard/mouse, an Xbox-style controller, a PlayStation-style controller, and Steam Deck controls;
- display switching during stable streaming and simulated network degradation;
- controller and input restoration after 100 repeated Deck open/close cycles;
- client disconnect, Perigee quit, and Polaris end-session distinctions;
- a standard non-Polaris Sunshine host for regression coverage.

## 12. Packaging and Release Boundary

v0.1 produces:

- an x86_64 AppImage using current Moonlight-compatible packaging patterns;
- an unpacked Linux tarball suitable for local testing and diagnosis;
- symbols or a separate debug artifact for actionable crash reports;
- release notes that distinguish Polaris-only features from standard-host behavior.

Flatpak follows only after native Wayland, controller, device-access, and input-capture behavior is stable, because sandbox permissions and portals introduce a separate validation surface.

## 13. Delivery Sequence

The implementation plan should preserve this dependency order:

1. Establish the current Moonlight Qt fork, upstream-sync policy, reproducible baseline build, and unchanged-stream regression test.
2. Add the action registry and Deck shell with local Moonlight actions.
3. Add input neutralization, controller ownership/navigation, and legacy-shortcut configuration.
4. Add the authenticated Polaris adapter, capability/session discovery, and truthful disabled states.
5. Add Polaris clipboard and named-command actions.
6. Add display enumeration, switching, reconnection, verification, and restoration; record any capability gaps demonstrated by the live Polaris test.
7. If a required v0.1 capability is missing, implement the smallest justified upstream Polaris patch or removable Perigee Bridge operation.
8. Complete live Nobara/KDE/Wayland acceptance and produce AppImage/tar artifacts.

## 14. Definition of Done for v0.1

Perigee v0.1 is complete when all specified Artemis-derived actions and active-display switching work through the Search Rail against the live Polaris reference environment; every core action is reachable by keyboard, mouse, and controller; failures and permissions are reported truthfully; repeated Deck use does not produce stuck input; ordinary Sunshine streaming remains functional; and the AppImage and tarball pass the release acceptance matrix.

## 15. Source Projects

- Moonlight Qt: <https://github.com/moonlight-stream/moonlight-qt>
- Polaris: <https://github.com/papi-ux/polaris>
- Nova: <https://github.com/papi-ux/nova>
- Artemis donor: <https://github.com/wjbeckett/artemis>

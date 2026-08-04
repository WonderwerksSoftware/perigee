# Perigee v0.1 live acceptance — August 2026

## Status

**PARTIAL. Task 19 is accepted. The Polaris control-plane probe and a bounded Standard Sunshine video/audio smoke passed. Deck pointer and list-reset regressions pass on Wayland. Live input, controller, display, and recovery acceptance is still open.**

This file is the publishable acceptance ledger. Do not put an exact host name, network address, UUID, certificate, token, clipboard content, private-key path, or user-profile path in this file. Keep exact endpoint and identity evidence in an ignored local file with mode `0600`.

Task 19 artifact acceptance is complete. The live work used the authorized staging host and the verified 10G path. The bounded Standard Sunshine smoke started and decoded a stream; it did not send input, transfer clipboard data, change displays, or end the host session. Exact evidence is in the ignored local file with mode `0600`.

## Safety and evidence rules

| Field | Required value | Current value |
|---|---|---|
| Default environment collection | Local and read-only; no host contact | Confirmed by contract test; live collection not run |
| Default Deck cycle mode | Dry run; no driver call; no host contact | Confirmed by contract test; live cycle not run |
| Live-action authorization | `--live` and `PERIGEE_ACCEPT_LIVE_TESTS=YES` | Granted for this acceptance run |
| Sensitive-summary authorization | `--include-paired-summary` and `PERIGEE_ACCEPT_SENSITIVE_COLLECTION=YES` | Not granted |
| Exact host evidence | Ignored local file only; mode `0600` | Recorded locally; no secrets or paired keys |
| Published evidence | Redacted values and pass/fail results only | This ledger contains no live values |
| Staging-host contact | Requires separate user authorization and verified 10G route | Authorized; control-plane probe completed |
| Management-route fallback | Prohibited | Pass; no fallback used |

Before live work, record the rollback procedure and verify that it does not change unrelated network, storage, display, or service state. Stop if the 10G path is unavailable. Do not silently use a management path.

## Harness commands

The environment collector writes a redacted report to standard output. Its default mode does not read a paired profile or contact a host:

```bash
scripts/acceptance/collect-environment.sh
```

Optional GL and display-output queries are local and read-only, but they can touch the current desktop services. Set their gates only during an approved local collection:

```bash
PERIGEE_COLLECT_GLXINFO=YES \
PERIGEE_COLLECT_DISPLAY_OUTPUTS=YES \
scripts/acceptance/collect-environment.sh
```

To include a prepared permission summary, use both gates. The input must be a regular file that is not a symlink and is no larger than 1 MiB:

```bash
PERIGEE_ACCEPT_SENSITIVE_COLLECTION=YES \
scripts/acceptance/collect-environment.sh \
  --include-paired-summary /path/to/prepared-summary.txt
```

Do not pass a live profile to the collector. Prepare a minimal summary that contains permission names and results only.

The Deck harness is a dry run unless both live gates are present:

```bash
scripts/acceptance/deck-cycle-test.sh
```

For an approved automated run, use an audited local driver at an absolute path:

```bash
PERIGEE_ACCEPT_LIVE_TESTS=YES \
PERIGEE_DECK_CYCLE_DRIVER=/absolute/path/to/audited-driver \
scripts/acceptance/deck-cycle-test.sh --live --mode automated --cycles 100
```

For an approved physical-controller run:

```bash
PERIGEE_ACCEPT_LIVE_TESTS=YES \
PERIGEE_DECK_CYCLE_DRIVER=/absolute/path/to/audited-driver \
scripts/acceptance/deck-cycle-test.sh --live --mode physical --cycles 20
```

The driver interface is `snapshot`, `open`, `close`, and `physical-cycle`. The harness stores snapshots in a private temporary directory, compares the initial and final bytes, and deletes the snapshots. It does not print snapshot contents. If an automated close fails, the exit trap makes one more close request.

## Environment record

| Field | Result |
|---|---|
| Collection date and time | 2026-08-04; exact time is local-only |
| Tester | Perigee acceptance harness |
| Perigee commit | `053de742` |
| Polaris commit and version | Official Ubuntu 24.04 release package, version `1.3.4` |
| Standard Sunshine version | Active existing user service; exact version is local-only |
| Operating system | Ubuntu 24.04 |
| KDE Plasma and KWin version | Local-only host evidence |
| Session type | Wayland |
| Qt version | Local-only host evidence |
| SDL version | Local-only host evidence |
| SDL video driver | Local-only host evidence |
| GPU and renderer class | AMD Radeon Pro WX 4100; VAAPI H.264 and HEVC encoders found |
| Physical, virtual, and headless output classes | One physical output; virtual backend unavailable; portal headless mode available |
| Paired permission summary | Pair status and authenticated readback passed; exact permission value is local-only |

Use output classes and counts in this publishable table. Do not record connector UUIDs, EDIDs, network addresses, or certificate fingerprints here.

## 10G route and host-identity gate

Keep exact values in the local evidence file. Copy only the result and a non-sensitive interface label to this table.

| Check | Evidence field | Result |
|---|---|---|
| User authorized the staging host | Authorization reference | PASS |
| Exact staging-host identity matches local records | Local evidence digest only | PASS |
| Exact 10G endpoint belongs to that host | Local evidence digest only | PASS |
| Selected route uses the 10G endpoint | Sanitized route result | PASS |
| Selected interface is the 10G interface | Non-sensitive interface label | PASS |
| Source-address ownership is correct | Pass/fail only | PASS |
| Management path is not used | Pass/fail only | PASS |
| Existing Polaris service state is recorded | Pass/fail and version only | No prior Polaris service; side-by-side state recorded |
| Rollback package, service, or side-by-side process is ready | Pass/fail only | PASS |

If any row fails, stop before deployment or streaming traffic.

## Polaris capability and public-port gate

| Check | Expected result | Actual result |
|---|---|---|
| Paired TLS identity is unchanged | PASS | PASS |
| Required v1 capability flags are present | PASS | PASS |
| Permission summary matches the paired role | PASS | PASS |
| Listener snapshot was recorded before the change | PASS | PASS |
| Listener snapshot was recorded after the change | PASS | PASS |
| New public listening ports | No Sunshine listener changes; alternate Polaris listeners are isolated to this run | PASS |
| Existing public-port exposure increased | No relative to the Sunshine baseline | PASS |
| Rollback restores the prior listener and service state | PASS | PASS; side-by-side service can be stopped without changing Sunshine |

Do not publish port numbers, host addresses, certificates, or capability-response bodies. Record only the reviewed flag names and pass/fail results.

## Bidirectional stream and control matrix

| Direction | Check | Polaris result | Standard Sunshine result |
|---|---|---|---|
| Perigee to host | Pair and authenticate | PASS for the Polaris test identity | PASS for the existing Sunshine identity; no new pairing request was needed |
| Perigee to host | Keyboard and mouse input | NOT RUN | NOT RUN |
| Perigee to host | Controller input and rumble | NOT RUN | NOT RUN |
| Perigee to host | Authenticated named command | NOT RUN | Not applicable; action must be disabled truthfully |
| Perigee to host | UTF-8 clipboard action | NOT RUN | Not applicable; action must be disabled truthfully |
| Perigee to host | Display selection and verified readback | NOT RUN | Not applicable; action must be disabled truthfully |
| Host to Perigee | Video and decoded-frame evidence | FAIL; the client initialized H.264 decode and received the initial test frame, then received no video traffic and reported `No video received from host` | PASS; 1280x720x30, decode test passed, first video packet at 100 ms |
| Host to Perigee | Audio | PARTIAL; the client received the first audio packet and the host PipeWire capture became active; local playback was not recorded | PASS; first audio packet at 400 ms; local playback was not recorded |
| Host to Perigee | Stream state and error readback | PARTIAL; launch and channel startup succeeded, but the client timed out on video and Polaris hung during cleanup before its service restarted | PASS; launch returned HTTP 200 and the bounded run ended without a no-video error |
| Host to Perigee | Clipboard acknowledgement without content in logs | NOT RUN | Not applicable |
| Route | Traffic stayed on the verified 10G path | PASS for the authenticated control-plane probe | NOT RUN |

The read-only mTLS probe used a standard Moonlight client identity to verify Polaris endpoints. The Polaris smoke reused that identity in a temporary profile and started a 1280x720x30 desktop stream, but it did not sustain video and Polaris hung during cleanup. A separate bounded run against the existing Standard Sunshine service used its existing certificate, launched successfully, passed the H.264 decode test, and recorded first video and audio packets at 100 ms and 400 ms. No input event, clipboard transfer, display mutation, or rollback was tested. Sunshine and Polaris service configuration were not changed by this acceptance run.

## Input and Deck acceptance

Run every applicable action through mouse, keyboard, and controller navigation. Keep exact device serial numbers out of the ledger.

| Input path | Navigation | Activation | Back/cancel | Search | Glyphs | Result |
|---|---|---|---|---|---|---|
| Keyboard | NOT RUN | NOT RUN | NOT RUN | NOT RUN | Not applicable | NOT RUN |
| Mouse | NOT RUN | NOT RUN | NOT RUN | NOT RUN | Not applicable | NOT RUN |
| Xbox-style controller | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| PlayStation-style controller | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| Steam Deck controls | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |

| Cycle gate | Required count | Input state restored | Capture state restored | Result |
|---|---:|---|---|---|
| Automated Deck open/close | 100 | NOT RUN | NOT RUN | NOT RUN |
| Physical-controller Deck open/close | 20 | NOT RUN | NOT RUN | NOT RUN |

A pass requires an exact initial/final snapshot match. Do not waive a stuck key, mouse button, gamepad button, or capture state.

## Display and recovery matrix

| Target or failure | Switch request | State readback | Decoded frame | Rollback | User message | Result |
|---|---|---|---|---|---|---|
| Physical display | NOT RUN | NOT RUN | NOT RUN | Not applicable on success | NOT RUN | NOT RUN |
| Virtual display | NOT RUN | NOT RUN | NOT RUN | Not applicable on success | NOT RUN | NOT RUN |
| Headless display | NOT RUN | NOT RUN | NOT RUN | Not applicable on success | NOT RUN | NOT RUN |
| Forced reconnect succeeds | NOT RUN | NOT RUN | NOT RUN | Not applicable | NOT RUN | NOT RUN |
| Switch fails before mutation | NOT RUN | NOT RUN | NOT RUN | No rollback needed | NOT RUN | NOT RUN |
| Switch fails after mutation; rollback succeeds | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| Switch fails after mutation; rollback fails | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |

Do not report display-switch success without target readback and a decoded frame. Attempt no more than one automatic rollback.

## Disconnect, Quit, and End outcomes

These actions must have different copy and different effects.

| Action | Client outcome | Host-session outcome | Authenticated host mutation | Result |
|---|---|---|---|---|
| Disconnect | Stream closes; Perigee remains available | Host session continues | No | NOT RUN |
| Quit Perigee | Client exits | Host session continues | No | NOT RUN |
| End host session | Client reports authenticated stop result | Host session ends | Yes | NOT RUN |

For each row, record the observed outcome. Do not infer success from an accepted request alone.

## Standard Sunshine regression

| Check | Result |
|---|---|
| Pair and launch | PASS; launched through the existing Sunshine service with the corrected temporary test profile |
| Video and audio | PASS; 1280x720x30, H.264 decode test passed, first video packet at 100 ms, first audio packet at 400 ms |
| Polaris discovery on Sunshine host | PASS; no Polaris endpoint probes in the rebuilt client log |
| Deck pointer/list regressions | PASS; 20 Qt Quick tests pass on the Wayland compositor |
| Keyboard, mouse, controller, and rumble | NOT RUN |
| Statistics shortcut | NOT RUN |
| Direct legacy shortcuts | NOT RUN |
| Local Deck actions | NOT RUN |
| Polaris-only actions disabled with a truthful reason | NOT RUN |
| Disconnect and Quit keep the host session running | NOT RUN |

## Final decision

| Gate | Result |
|---|---|
| All automated suites pass at the tested commits | PASS |
| Task 19 cold Ubuntu candidates pass independent verification | PASS |
| Polaris live matrix passes | PARTIAL; control plane and session startup pass, but the latest custom run has no sustained video traffic and cleanup hangs |
| Standard Sunshine regression passes | PARTIAL; launch and bounded video/audio smoke pass; input, controller, statistics, Deck, and disconnect/quit gates remain open |
| Controller and input-neutralization gates pass | NOT RUN |
| Display verification and rollback gates pass | NOT RUN |
| 10G route evidence passes without management fallback | PASS |
| No Sunshine public-port exposure increased | PASS |
| Publishable evidence contains no sensitive value | PASS |

**Release decision: NOT ACCEPTED.**

# Perigee v0.1 live acceptance — August 2026

## Status

**PARTIAL. Task 19 is accepted. The Polaris control-plane probe and a bounded standard Sunshine video and audio test passed. Deck pointer regressions pass on Wayland. Live input, controller, and physical display acceptance is still open.**

This file is the publishable acceptance ledger. Do not put an exact host name, network address, UUID, certificate, token, clipboard content, private-key path, or user-profile path in this file. Keep exact endpoint and identity evidence in an ignored local file with mode `0600`.

Task 19 artifact acceptance is complete. The live work used the authorized staging host and the verified 10G path.

The bounded standard Sunshine test started and decoded a stream. It did not send input, transfer clipboard data, change displays, or end the host session.

Exact evidence is in the ignored local file with mode `0600`.

## Safety and evidence rules

| Field | Required value | Current value |
|---|---|---|
| Default environment collection | Local and read-only. No host contact. | Confirmed by contract test. Live collection not run. |
| Default Deck cycle mode | Dry run. No driver call. No host contact. | Confirmed by contract test. Live cycle not run. |
| Live-action authorization | `--live` and `PERIGEE_ACCEPT_LIVE_TESTS=YES` | Granted for this acceptance run |
| Sensitive-summary authorization | `--include-paired-summary` and `PERIGEE_ACCEPT_SENSITIVE_COLLECTION=YES` | Not granted |
| Exact host evidence | Ignored local file only. Mode `0600`. | Recorded locally. No secrets or paired keys. |
| Published evidence | Redacted values and pass/fail results only | This ledger contains no live values |
| Staging-host contact | Requires separate user authorization and verified 10G route | Authorized. Control-plane probe completed. |
| Management-route fallback | Prohibited | Pass. No fallback used. |

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
| Collection date and time | 2026-08-04. Exact time is local-only. |
| Tester | Perigee acceptance harness |
| Perigee commit | `053de742` |
| Polaris commit and version | Official Ubuntu 24.04 release package, version `1.3.4` |
| Standard Sunshine version | Active existing user service. Exact version is local-only. |
| Operating system | Ubuntu 24.04 |
| KDE Plasma and KWin version | Local-only host evidence |
| Session type | Wayland |
| Qt version | Local-only host evidence |
| SDL version | Local-only host evidence |
| SDL video driver | Local-only host evidence |
| GPU and renderer class | AMD Radeon Pro WX 4100. VAAPI H.264 and HEVC encoders found. |
| Physical, virtual, and headless output classes | One physical output. Virtual backend unavailable. Portal headless mode available. |
| Paired permission summary | Pair status and authenticated readback passed. Exact permission value is local-only. |

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
| Existing Polaris service state is recorded | Pass/fail and version only | No prior Polaris service. Side-by-side state recorded. |
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
| New public listening ports | No Sunshine listener changes. Alternative Polaris listeners are isolated to this run. | PASS |
| Existing public-port exposure increased | No relative to the Sunshine baseline | PASS |
| Rollback restores the prior listener and service state | PASS | PASS. You can stop the side-by-side service without a Sunshine change. |

Do not publish port numbers, host addresses, certificates, or capability-response bodies. Record only the reviewed flag names and pass/fail results.

## Bidirectional stream and control matrix

| Direction | Check | Polaris result | Standard Sunshine result |
|---|---|---|---|
| Perigee to host | Pair and authenticate | PASS for the Polaris test identity | PASS for the existing Sunshine identity. No new pairing request was necessary. |
| Perigee to host | Keyboard and mouse input | NOT RUN | NOT RUN |
| Perigee to host | Controller input and rumble | NOT RUN | NOT RUN |
| Perigee to host | Authenticated named command | NOT RUN | Not applicable. The action must be disabled truthfully. |
| Perigee to host | UTF-8 clipboard action | NOT RUN | Not applicable. The action must be disabled truthfully. |
| Perigee to host | Physical display request and fresh video | NOT RUN | NOT RUN |
| Perigee to host | Manual and Adaptive quality control | NOT RUN | Not applicable. The configured bitrate must remain read-only. |
| Host to Perigee | Video and decoded-frame evidence | FAIL. The client initialized H.264 decode and received the initial test frame. The client then received no video traffic and reported `No video received from host`. | PASS. The 1280x720x30 decode test passed. The first video packet arrived at 100 ms. |
| Host to Perigee | Audio | PARTIAL. The client received the first audio packet. The host PipeWire capture became active. Local playback was not recorded. | PASS. The first audio packet arrived at 400 ms. Local playback was not recorded. |
| Host to Perigee | Stream state and error readback | PARTIAL. Launch and channel startup succeeded. The client timed out on video. Polaris did not respond during cleanup. | PASS. Launch returned HTTP 200. The bounded run ended without a no-video error. |
| Host to Perigee | Clipboard acknowledgement without content in logs | NOT RUN | Not applicable |
| Route | Traffic stayed on the verified 10G path | PASS for the authenticated control-plane probe | NOT RUN |

The read-only mTLS probe used a standard Moonlight client identity to verify Polaris endpoints. The Polaris test reused that identity in a temporary profile and started a 1280x720x30 desktop stream. It did not sustain video. Polaris did not respond during cleanup.

A separate bounded run used the existing standard Sunshine service. The stream started, and the H.264 decode test passed. The first video packet arrived at 100 ms. The first audio packet arrived at 400 ms.

The acceptance run did not send input or clipboard data. It did not request a physical display or stop a host session.

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

## Physical display acceptance

| Check | Result | Scope |
|---|---|---|
| Standard Sunshine stream regression | Pending live run | Core stream and local Deck only |
| Stock Polaris Display 1 | Pending live run | Balanced shortcut plus fresh frame |
| Stock Polaris Display 2 | Pending live run | Balanced shortcut plus fresh frame |
| Stock Polaris Display 3 | Pending live run | Balanced shortcut plus fresh frame |
| Fullscreen controller selection | Pending live run | Deck remains open and responsive |
| Verification timeout | Automated only | No display scan and bounded restoration rules |

Perigee sends one standard GameStream display shortcut. A fresh video frame verifies that video resumed after the request.

Perigee does not receive authoritative physical display state. The **Last requested** value is client evidence, not host readback.

## Disconnect, Quit, and End outcomes

These actions must have different copy and different effects.

| Action | Client outcome | Host-session outcome | Authenticated host mutation | Result |
|---|---|---|---|---|
| Disconnect | Stream closes. Perigee remains available. | Host session continues | No | NOT RUN |
| Quit Perigee | Client exits | Host session continues | No | NOT RUN |
| End host session | Client reports authenticated stop result | Host session ends | Yes | NOT RUN |

For each row, record the observed outcome. Do not infer success from an accepted request alone.

## Standard Sunshine regression

| Check | Result |
|---|---|
| Pair and launch | PASS. The client used the existing Sunshine service and the corrected temporary test profile. |
| Video and audio | PASS. The 1280x720x30 H.264 decode test passed. Video arrived at 100 ms. Audio arrived at 400 ms. |
| Polaris discovery on Sunshine host | PASS. The rebuilt client log contains no Polaris endpoint probes. |
| Deck pointer/list regressions | PASS. Twenty Qt Quick tests pass on the Wayland compositor. |
| Keyboard, mouse, controller, and rumble | NOT RUN |
| Statistics shortcut | NOT RUN |
| Direct legacy shortcuts | NOT RUN |
| Local Deck actions | NOT RUN |
| Physical display request | Pending live run |
| Configured bitrate is read-only | Pending live run |
| Polaris-only actions disabled with a truthful reason | NOT RUN |
| Disconnect and Quit keep the host session running | NOT RUN |

## Final decision

| Gate | Result |
|---|---|
| All automated suites pass at the tested commits | PASS |
| Task 19 cold Ubuntu candidates pass independent verification | PASS |
| Polaris live matrix passes | PARTIAL. Control plane and session startup pass. The latest custom run has no sustained video traffic. |
| Standard Sunshine regression passes | PARTIAL. Launch and the bounded video and audio test pass. Input, controller, statistics, Deck, disconnect, and quit gates remain open. |
| Controller and input-neutralization gates pass | NOT RUN |
| Physical display request and fresh-frame gates pass | NOT RUN |
| 10G route evidence passes without management fallback | PASS |
| No Sunshine public-port exposure increased | PASS |
| Publishable evidence contains no sensitive value | PASS |

**Release decision: NOT ACCEPTED.**

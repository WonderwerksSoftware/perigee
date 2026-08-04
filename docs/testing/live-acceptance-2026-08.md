# Perigee v0.1 live acceptance — August 2026

## Status

**NOT RUN. No host was contacted. No live action was authorized.**

This file is the publishable acceptance ledger. Do not put an exact host name, network address, UUID, certificate, token, clipboard content, private-key path, or user-profile path in this file. Keep exact endpoint and identity evidence in an ignored local file with mode `0600`.

Task 19 artifact acceptance is still open. A local green test does not accept an artifact or a live result.

## Safety and evidence rules

| Field | Required value | Current value |
|---|---|---|
| Default environment collection | Local and read-only; no host contact | Confirmed by contract test; live collection not run |
| Default Deck cycle mode | Dry run; no driver call; no host contact | Confirmed by contract test; live cycle not run |
| Live-action authorization | `--live` and `PERIGEE_ACCEPT_LIVE_TESTS=YES` | Not granted |
| Sensitive-summary authorization | `--include-paired-summary` and `PERIGEE_ACCEPT_SENSITIVE_COLLECTION=YES` | Not granted |
| Exact host evidence | Ignored local file only; mode `0600` | Not created |
| Published evidence | Redacted values and pass/fail results only | This ledger contains no live values |
| Staging-host contact | Requires separate user authorization and verified 10G route | Not contacted |
| Management-route fallback | Prohibited | Not attempted |

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
| Collection date and time | NOT RUN |
| Tester | NOT RUN |
| Perigee commit | NOT RUN |
| Polaris commit and version | NOT RUN |
| Standard Sunshine version | NOT RUN |
| Operating system | NOT RUN |
| KDE Plasma and KWin version | NOT RUN |
| Session type | NOT RUN |
| Qt version | NOT RUN |
| SDL version | NOT RUN |
| SDL video driver | NOT RUN |
| GPU and renderer class | NOT RUN |
| Physical, virtual, and headless output classes | NOT RUN |
| Paired permission summary | NOT RUN |

Use output classes and counts in this publishable table. Do not record connector UUIDs, EDIDs, network addresses, or certificate fingerprints here.

## 10G route and host-identity gate

Keep exact values in the local evidence file. Copy only the result and a non-sensitive interface label to this table.

| Check | Evidence field | Result |
|---|---|---|
| User authorized the staging host | Authorization reference | NOT RUN |
| Exact staging-host identity matches local records | Local evidence digest only | NOT RUN |
| Exact 10G endpoint belongs to that host | Local evidence digest only | NOT RUN |
| Selected route uses the 10G endpoint | Sanitized route result | NOT RUN |
| Selected interface is the 10G interface | Non-sensitive interface label | NOT RUN |
| Source-address ownership is correct | Pass/fail only | NOT RUN |
| Management path is not used | Pass/fail only | NOT RUN |
| Existing Polaris service state is recorded | Pass/fail and version only | NOT RUN |
| Rollback package, service, or side-by-side process is ready | Pass/fail only | NOT RUN |

If any row fails, stop before deployment or streaming traffic.

## Polaris capability and public-port gate

| Check | Expected result | Actual result |
|---|---|---|
| Paired TLS identity is unchanged | PASS | NOT RUN |
| Required v1 capability flags are present | PASS | NOT RUN |
| Permission summary matches the paired role | PASS | NOT RUN |
| Listener snapshot was recorded before the change | PASS | NOT RUN |
| Listener snapshot was recorded after the change | PASS | NOT RUN |
| New public listening ports | 0 | NOT RUN |
| Existing public-port exposure increased | No | NOT RUN |
| Rollback restores the prior listener and service state | PASS | NOT RUN |

Do not publish port numbers, host addresses, certificates, or capability-response bodies. Record only the reviewed flag names and pass/fail results.

## Bidirectional stream and control matrix

| Direction | Check | Polaris result | Standard Sunshine result |
|---|---|---|---|
| Perigee to host | Pair and authenticate | NOT RUN | NOT RUN |
| Perigee to host | Keyboard and mouse input | NOT RUN | NOT RUN |
| Perigee to host | Controller input and rumble | NOT RUN | NOT RUN |
| Perigee to host | Authenticated named command | NOT RUN | Not applicable; action must be disabled truthfully |
| Perigee to host | UTF-8 clipboard action | NOT RUN | Not applicable; action must be disabled truthfully |
| Perigee to host | Display selection and verified readback | NOT RUN | Not applicable; action must be disabled truthfully |
| Host to Perigee | Video and decoded-frame evidence | NOT RUN | NOT RUN |
| Host to Perigee | Audio | NOT RUN | NOT RUN |
| Host to Perigee | Stream state and error readback | NOT RUN | NOT RUN |
| Host to Perigee | Clipboard acknowledgement without content in logs | NOT RUN | Not applicable |
| Route | Traffic stayed on the verified 10G path | NOT RUN | NOT RUN |

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
| Pair and launch | NOT RUN |
| Video and audio | NOT RUN |
| Keyboard, mouse, controller, and rumble | NOT RUN |
| Statistics shortcut | NOT RUN |
| Direct legacy shortcuts | NOT RUN |
| Local Deck actions | NOT RUN |
| Polaris-only actions disabled with a truthful reason | NOT RUN |
| Disconnect and Quit keep the host session running | NOT RUN |

## Final decision

| Gate | Result |
|---|---|
| All automated suites pass at the tested commits | NOT RUN |
| Task 19 cold Ubuntu candidates pass independent verification | NOT RUN |
| Polaris live matrix passes | NOT RUN |
| Standard Sunshine regression passes | NOT RUN |
| Controller and input-neutralization gates pass | NOT RUN |
| Display verification and rollback gates pass | NOT RUN |
| 10G route evidence passes without management fallback | NOT RUN |
| No new public port exists | NOT RUN |
| Publishable evidence contains no sensitive value | NOT RUN |

**Release decision: NOT ACCEPTED.**

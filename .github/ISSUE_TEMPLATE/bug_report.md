---
name: Bug report
about: Report a reproducible Perigee defect
---

## Before you report

Search open and closed issues for the same problem.

The [upstream Moonlight troubleshooting guide](https://github.com/moonlight-stream/moonlight-docs/wiki/Troubleshooting) can help compare inherited Moonlight-core behavior and host setup.
It is an upstream comparison resource. It is not Perigee-specific support or endorsement.

## Describe the bug

Describe what happened and what you expected.

## Steps to reproduce

1.
2.
3.

## Affected applications

List each streamed application that shows the problem.
Also test Steam Big Picture or the desktop to find application-specific behavior.

## Comparison with upstream Moonlight clients (optional)

Does the issue also occur in official Moonlight on iOS or Android?
This comparison can separate inherited Moonlight-core behavior from Perigee behavior.

## Perigee settings

- Which settings differ from the defaults?
- Does the problem occur after you restore the defaults?
- Which display was selected?
- Was Perigee Deck open?

## Controller details (if applicable)

- Controller model:
- Connected to the client or the host:
- Does [HTML5 Gamepad Tester](https://html5gamepad.com/) detect the controller when you stream the desktop?

The [upstream Moonlight setup guide](https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide) can help compare the host setup.
It is an upstream comparison resource. It is not Perigee-specific support or endorsement.

## Client details

- Operating system and version:
- Perigee version:
- GPU:
- Package or build type:

## Server details

- Operating system and version:
- Server software and version, such as Polaris or Sunshine:
- GPU:
- GPU driver:

## Perigee logs

- Windows: Attach `Perigee-*.log` from `%TEMP%`.
- macOS: Attach `Perigee-*.log` from `/tmp`.
- Linux development builds: Start Perigee from a terminal. Attach the terminal output from the run that reproduced the issue.

Remove private host names, addresses, and tokens before you attach logs.

## Screenshots

Attach screenshots when they clearly show the problem.

## Additional context

Add any other details that can help reproduce the problem.

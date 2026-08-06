# Deck Quality Control Implementation Plan

## Goal

Add controller-friendly Manual and Adaptive quality controls to the Perigee Deck. Use the official Polaris live-control contract and preserve standard Sunshine compatibility.

## Task 1: Parse quality state

- Extend `PolarisControls` with `hostTuningAllowed`.
- Extend `PolarisClientSettings` with strict optional quality fields.
- Add bitrate and adaptive quality operations to the availability model.
- Write failing model tests before implementation.

## Task 2: Add the Quality category

- Add `ActionCategory::Quality` after Display.
- Add a read-only local stream-quality action.
- Report the configured GameStream bitrate through `SessionFacade`.
- Update category, controller, model, and QML tests.

## Task 3: Add Polaris quality actions

- Register status, Manual, Adaptive, reduce, and increase actions.
- Gate actions on capabilities, ownership, tuning permission, and valid settings.
- Build exact bounded requests for the official endpoints.
- Write failing action tests before implementation.

## Task 4: Validate responses and refresh

- Validate the official success response for each operation.
- Request a discovery refresh after acceptance.
- Keep the prior authoritative state until fresh readback arrives.
- Test errors, malformed replies, cancellation, and refresh behavior.

## Task 5: Document and verify

- Update the README, upstream pins, and test count.
- Run focused tests.
- Run a cold build and the full graphical suite.
- Deploy only the client build to the authorized test laptop when it is online.
- Test Manual and Adaptive modes against the existing Polaris workstation.
- Verify standard Sunshine shows read-only quality without a fake live action.

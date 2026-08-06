# Deck Quality Control Design

## Purpose

Perigee must let a user inspect and change stream quality from the Deck. The controls must work with a keyboard, mouse, or controller.

## Host boundary

Polaris supplies live bitrate and AI Auto Quality operations. Perigee uses the official Polaris API. This work does not change the Polaris server.

Standard Sunshine does not supply a Moonlight protocol operation for a live bitrate change. Perigee shows the configured stream bitrate, but it disables live changes.

## Deck model

Add a **Quality** category after **Display**. This keeps the main workstation controls close to each other.

The category has these actions:

- **Stream quality** shows the current mode and bitrate.
- **Manual quality** disables Polaris AI Auto Quality.
- **Adaptive quality** enables Polaris AI Auto Quality.
- **Reduce by 5 Mbps** reduces the Polaris bitrate target.
- **Increase by 5 Mbps** increases the Polaris bitrate target.

The step actions use 5 Mbps because they are fast with a controller and precise enough for a live stream. Perigee clamps each request to the limits that Polaris reports.

## Mode semantics

Manual mode keeps the last selected bitrate. The user can change it with the step actions.

Adaptive mode lets Polaris use stream health, packet loss, and round-trip time to change the encoder bitrate.

The current Polaris API links adaptive bitrate and its AI optimizer. Perigee therefore exposes one Adaptive mode. It does not present two independent switches.

A separate Smart mode is deferred until Polaris advertises an independent contract. Perigee must not imply that two linked controls are independent.

## Protocol operations

Perigee reads `/polaris/v1/client-settings` and checks these capability flags:

- `adaptive_bitrate_control`
- `ai_auto_quality_control`
- `client_settings_v1`

It uses these official operations:

- `POST /polaris/v1/session/bitrate` with `bitrate_kbps`
- `POST /polaris/v1/session/adaptive-bitrate` with `enabled`

Perigee permits a mutation only when the client owns the session and `controls.host_tuning_allowed` is true.

## State and evidence

The settings parser reads the adaptive mode, current encoder bitrate, adaptive target, base, minimum, maximum, state, and reason.

After a successful mutation, Perigee starts a discovery refresh. The next settings response becomes the authoritative Deck state.

The request result can report acceptance. It must not claim that the new bitrate is active before readback.

## Failure behavior

The quality mutations are disabled when any required capability, permission, owner state, or valid settings value is absent.

The Deck gives one specific reason. It keeps unrelated local and host actions available.

Requests use one `quality.tuning` resource key. A new quality request cancels an older request for that resource.

## Test boundary

Tests must cover:

- strict parsing of current Polaris quality fields
- capability and permission failures
- standard Sunshine read-only behavior
- exact JSON request bodies and routes
- minimum and maximum clamping
- successful response validation and refresh
- malformed and failed response handling
- Quality category navigation and search

# Perigee Deck Input and Quality Polish Plan

**Goal:** Make the existing Perigee Deck reliable for daily client use without changing any host software or service.

**Scope:** Keep the working physical display switcher intact. Repair reused Polaris TLS request authentication for quality actions. Detect controllers opened by the client, select controller or keyboard hints from that state, preserve D-pad and arrow navigation, ignore Caps Lock in the Deck chord, and keep all category labels inside the overlay.

**Verification:** Add focused regressions for each repaired behavior. Build the full client, run the relevant Qt and packaging gates, pass GitHub Actions, verify the final AppImage, and deploy only the Perigee artifact and desktop launcher on Cometforge with a rollback copy.

**Boundaries:** Do not modify Sunshine, Polaris, networking, displays, or host services. Do not stop a live client. Do not deploy an unverified artifact.

# Task 10 Report: Add an Authenticated Polaris API Transport

Status: local implementation and deterministic verification are complete.
Live paired-host qualification remains deferred to Task 20.

Starting HEAD: `79936ac1d8e0bebf72d32289929adbb5ac06ca28`

## Implemented behavior

- Added a dedicated-thread Polaris HTTPS client. Its network manager, replies,
  and timers live on the worker thread; completed results enter a locked FIFO
  queue and user callbacks run only when the owner calls the bounded poll seam.
- Reused the existing Moonlight client identity from `IdentityManager` on every
  request. HTTP/2 and proxy use are disabled.
- Built requests only from the paired computer's active address and HTTPS port.
  Advertised endpoints must be canonical origin-relative paths under
  `/polaris/v1/`; schemes, authorities, alternate ports, fragments, traversal,
  delimiter ambiguity, backslashes, malformed encoding, and origin changes are
  rejected locally.
- Kept `/actions/clipboard` as one explicit paired-protocol operation. Its
  response is bounded by `min(advertised limit, 1 MiB)`, or 1 MiB when the host
  publishes no limit. The reply is aborted after reading only the limit plus
  one detection byte.
- Pinned authentication to the exact paired server leaf certificate. Null,
  missing, different, mixed-certificate, and unproven handshakes fail closed as
  `tls_identity_mismatch`; ordinary CA validity is not treated as paired
  authentication.
- Added stable monotonic request IDs, caller cancellation, total and idle
  timeouts, one-terminal-completion guards, bounded response retention, JSON
  validation, HTTP error classification, and synchronous worker shutdown.
- Added metadata-only logging and redaction. Logs contain method, redacted
  origin-relative path, status, elapsed time, and error class; tests prove that
  body, clipboard, command, header, certificate, token, key, and private-key
  canaries do not escape.

No Task 11 capability, display, command, action, or production clipboard UI was
added. No Deck/session polling change was required because the transport owns a
narrow poll method.

## TDD evidence

Focused command used for transport cycles:

`QT_QPA_PLATFORM=offscreen SDL_VIDEODRIVER=dummy ./perigee-tests PolarisApiClientTest -silent`

Recorded RED then GREEN transitions:

- The initial broad edge-case pass was 30 passed and 3 failed. An oversized
  reply lost its HTTP status in two cases, and a newline-bearing unsafe method
  reached the fake network. Capturing status before the bounded abort and
  allowing only GET and POST produced 33 passed and 0 failed.
- A 64 KiB fake reply defeated a four-byte generic bound: the RED test observed
  65,540 bytes read instead of five. Reading at most the remaining limit plus
  one byte produced 34 passed and 0 failed.
- A POST reached the fake manager without `application/json`: 34 passed and 1
  failed. Setting the content type before dispatch produced 35 passed and 0
  failed.
- Redaction exposed command and clipboard path canaries: 4 passed and 1 failed.
  Sensitive path/header vocabulary and private-key material are now replaced;
  the suite is 5 passed and 0 failed.
- The final requirements audit exposed session path values and `Session:` and
  `Token:` header-like input: 5 passed and 1 failed. Adding the missing
  sensitive vocabulary produced 6 passed and 0 failed.
- Clipboard fetch with no valid paired HTTPS origin reached the fake network and
  returned `tls_identity_mismatch`: 40 passed and 1 failed. It is now rejected
  before dispatch as `endpoint_policy_rejected`, producing the final 41 passed
  and 0 failed.

The completed transport suite covers paired IPv4, DNS, and bracketed IPv6
origins; endpoint-policy attacks; query preservation and redaction; request
identity and HTTP/2 policy; exact certificate matching; status/body/JSON
handling; timeouts, cancellation, SSL/finish races; bounded queue draining;
callback thread and mutex behavior; FIFO ordering; teardown; manager/reply
lifetime; and exact clipboard bounds.

## Verification evidence

- Debug application compile and link: exit 0.
- Debug test compile and link: exit 0.
- `PolarisApiClientTest`: 41 passed, 0 failed, 0 skipped.
- `RedactionTest`: 6 passed, 0 failed, 0 skipped.
- Fresh-process transport stress: 20 of 20 runs exited 0, for 820 test cases.
  No duplicate callback, leaked reply/manager, cross-thread warning, or
  `QThread` destruction warning was observed.
- Canonical headless run: 310 passed, 11 failed, 0 skipped. The 11 failures are
  the established environment-only `Deck OpenGL context creation failed`
  results: eight in `DeckQmlTest` and three in `DeckSurfaceRendererTest`. Every
  non-renderer class passed.
- `git diff --check`: clean after the final report update.

Builds used `CCACHE_DISABLE=1` because the sandbox's home cache is read-only.
The inherited shell prints malformed module-function and Starship cache
warnings before commands; these do not originate in Perigee and did not affect
exit status.

## ThreadSanitizer status

QMake's named sanitizer configurations emitted no sanitizer flags, so they were
not accepted as evidence. A direct build supplied:

`QMAKE_CFLAGS+=-fsanitize=thread -fno-omit-frame-pointer`

with matching C++ flags and `QMAKE_LFLAGS+=-fsanitize=thread`. The generated
Makefile and compile commands contain `-fsanitize=thread`, but the link fails:

`/usr/bin/ld.bfd: cannot find /usr/lib64/libtsan.so.2.0.0: No such file or directory`

`rpm -q libtsan` reports that the package is not installed. The available GCC
`libtsan.so` file is only a linker script that refers to the missing runtime.
Therefore this report does not claim a TSan run or pass.

## Deferred acceptance and residual concerns

No public network, Xvfb, Polaris host, Sunshine host, ww-DevBox, or 10G path was
used. Task 20 must verify the paired client certificate, exact server leaf,
advertised endpoints, timeout behavior, clipboard limit, and user-visible error
mapping against the live Polaris version. A future environment with a complete
ThreadSanitizer runtime should repeat the transport race and teardown suite.

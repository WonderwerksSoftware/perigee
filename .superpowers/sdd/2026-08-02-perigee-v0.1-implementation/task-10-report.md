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

## Fix round 1: lifecycle, TLS classification, and redaction hardening

Base commit: `b74a7b16d7b327ee85fd9a2136bb974306ecc1be`

The review findings were reproduced before production changes:

- Host-not-found and connection-refused replies that never emitted `encrypted`
  were both classified as `tls_identity_mismatch`: 2 passed and 2 failed.
  Ordinary pre-TLS transport errors are now `network_error`; an explicit SSL
  handshake failure or a no-error finish without exact encrypted identity still
  fails as `tls_identity_mismatch`.
- Duplicate path separators were accepted for both tested positions: 21 passed
  and 2 failed. Canonical `/polaris/v1/` remains accepted, while interior empty
  segments are rejected.
- A GET carrying `CANARY_GET_BODY` reached the fake backend and completed
  successfully: 2 passed and 1 failed. Nonempty GET bodies now fail locally as
  `endpoint_policy_rejected`; the backend and captured log remain canary-free.
- Cancellation returned true after both a locally queued policy result and a
  worker-queued result: each isolated case was 2 passed and 1 failed. It also
  returned true for already-selected callback B: 2 passed and 1 failed.
- A recursive drain from callback A returned 1 and delivered queued C ahead of
  already-selected B: 2 passed and 1 failed.
- The callback-deletion behavior test was already 3 passed and 0 failed because
  destructor cleanup happened to clear callback B, but source tracing showed
  the outer drain still dereferenced the destroyed private object. The test is
  retained as a behavioral guard; the implementation now removes that undefined
  access structurally.
- The constructor completed while another thread held `NvComputer::lock` for
  writing: 2 passed and 1 failed. The active address, HTTPS port, and paired leaf
  are now copied under one read lock, which is released before identity lookup,
  backend creation, or thread startup.
- Encoded header/path material escaped unchanged: 2 passed and 1 failed.
  Encoded and unsafe query names also remained visible: 2 passed and 1 failed.
  Path logging now requires a canonical origin-relative shape, rejects encoded
  path material before vocabulary scanning, recognizes whitespace around header
  colons, sanitizes unsafe query names, and still replaces every query value.

The lifecycle rewrite uses one shared mutex for the terminal FIFO and every
request record. A request is submitted, cancellation-requested, or
terminal-queued under that authority. Cancellation can win exactly once only
from submitted state. The worker and caller compete for the same terminal
transition; a cancellation-first completion becomes one cancelled result and
aborts the reply, while a completion-first result makes cancellation return
false.

Drain removes selected records and callbacks under the shared mutex before
invocation, then calls user code without the mutex. A shared drain guard rejects
recursive drains, selected callbacks are no longer cancellable, and the method
retains only shared state before invoking callback A. If A destroys the client,
destruction marks that state non-accepting and remaining selected callbacks are
discarded without touching the destroyed private object.

Final local results for this fix round:

- Debug application and test compile/link: exit 0.
- `PolarisApiClientTest`: 53 passed, 0 failed, 0 skipped.
- `RedactionTest`: 8 passed, 0 failed, 0 skipped.
- Fresh-process lifecycle/transport stress: 20 of 20 runs exited 0, for 1,060
  test cases. No deadlock, duplicate callback, worker-thread, cross-thread, or
  teardown warning was observed.
- Canonical headless run: 324 passed, 11 failed, 0 skipped. The failures remain
  exactly eight `DeckQmlTest` and three `DeckSurfaceRendererTest` instances of
  the established `Deck OpenGL context creation failed` environment gate.
- ThreadSanitizer remains unavailable for the documented missing
  `/usr/lib64/libtsan.so.2.0.0` runtime. No TSan pass is claimed and no package
  was installed.
- No live network, Xvfb, Polaris, Sunshine, ww-DevBox, 10G, push, merge, or
  release operation was used.

## Fix round 2: HTTP reply precedence and callback exception containment

Base commit: `1bf06429b25f70cda6a52b3bfe2b3d723117e991`

Focused regressions were compiled and run before production changes:

- Authenticated 404 plus `ContentNotFoundError` and authenticated 503 plus
  `InternalServerError` each retained their status and body but returned
  `network_error`. The isolated RED result was 2 passed and 2 failed; both rows
  expected `http_error`.
- Three completions were queued, A and B were selected with `drainCompletions(2)`,
  and callback A threw a canary-bearing exception. The exception escaped the
  polling boundary before B ran. The isolated RED result was 2 passed and 1
  failed at the `exceptionEscaped` assertion.

Authenticated replies now classify every usable non-success HTTP status from
100 through 599 as `http_error` before considering `QNetworkReply::NetworkError`.
The status and response body remain attached to the result. A transport failure
without a usable HTTP status remains `network_error`; the existing fail-closed
pre-encryption and TLS-identity rules still run first.

Completion callbacks now run inside a catch-all boundary. If a callback throws,
the drain emits only the generic warning `Polaris completion callback failed;
continuing` and continues with the next already-selected FIFO completion while
the shared state remains accepting. It never logs exception text, request data,
response data, or identifiers. The accepting-state check still occurs before
each callback, so callback A destroying the client discards selected callback B
instead of invoking it. Callbacks remain outside the shared mutex and the
recursive-drain guard remains unchanged.

Final local results for this fix round:

- Debug application and test compile/link: exit 0; the application transport
  object was recompiled and the application relinked.
- `PolarisApiClientTest`: 56 passed, 0 failed, 0 skipped.
- `RedactionTest`: 8 passed, 0 failed, 0 skipped.
- Fresh-process transport stress: 20 of 20 runs exited 0, for 1,120 test cases.
  No failure, `QThread` destruction, cross-thread, deadlock, or canary output was
  observed.
- Canonical headless run: 327 passed, 11 failed, 0 skipped. The failures remain
  exactly eight `DeckQmlTest` and three `DeckSurfaceRendererTest` instances of
  the established `Deck OpenGL context creation failed` environment gate.
- ThreadSanitizer remains unavailable because the system runtime is absent. No
  TSan pass is claimed and no package was installed.
- No live network, display, Xvfb, Polaris, Sunshine, ww-DevBox, 10G, push,
  merge, or release operation was used.

## Fix round 3: compound sensitive path labels

Base commit: `8b9512d98c9ba718decb7e62dea00f7a42e08002`

A data-driven regression was compiled and run before production changes. The
isolated RED result was 6 passed and 4 failed:

- `session-token`, `authorization_key`, mixed-case `CLIENT.CERTIFICATE`, and
  `clipboard-command` each left the following canary value verbatim.
- The encoded compound-label row already failed closed as `<redacted>`.
- The negative `keyboard-layout`, `hockey-score`, and `monkey` rows already
  remained unchanged.

Source tracing confirmed that the path sanitizer lowercased each complete path
segment and compared it only with a set of complete sensitive labels. It did
not recognize sensitive words delimited inside a compound label.

The sanitizer now lowercases each label and recognizes a sensitive term only
when the complete label matches or when a complete component delimited by
hyphen, underscore, or dot matches. It does not use substring matching, so
`keyboard-layout`, `hockey-score`, and `monkey` remain benign. The prior
fail-closed rejection of encoded path material is unchanged, and the helper
does not log input values.

The final data table contains all four exact reviewer reproductions plus
underscore, dot, and mixed-case variants, an encoded fail-closed row, and the
three false-positive guards.

Final local results for this fix round:

- Debug application and test compile/link: exit 0; the application and test
  redaction objects were compiled and both binaries linked after the production
  change.
- Focused compound-label table: 12 passed, 0 failed, 0 skipped.
- `RedactionTest`: 18 passed, 0 failed, 0 skipped.
- `PolarisApiClientTest`: 56 passed, 0 failed, 0 skipped, with no canary output.
- Fresh-process focused repetition: 20 of 20 runs exited 0, for 240 test cases.
  No failure or canary output was observed.
- Canonical headless run: 337 passed, 11 failed, 0 skipped. The failures remain
  exactly eight `DeckQmlTest` and three `DeckSurfaceRendererTest` instances of
  the established `Deck OpenGL context creation failed` environment gate.
- `libtsan` remains uninstalled. No TSan run or pass is claimed and no package
  was installed.
- No live network, display, Xvfb, Polaris, Sunshine, ww-DevBox, 10G, push,
  merge, or release operation was used.

## Fix round 4: chained sensitive path labels

Base commit: `9704a63f`

Focused regressions were compiled and run before the production change:

- The chained-label data table produced 4 passed and 4 failed. Two exact
  labels, three exact labels, two compound labels, and a longer mixed-case
  hyphen/underscore/dot chain exposed either the trailing canary or an
  intervening sensitive label. Both benign substring and benign compound
  controls passed unchanged.
- A valid Polaris transport request to
  `/polaris/v1/session-token/authorization-key/CANARY_CHAINED_LOG_SECRET`
  completed, but its captured metadata log did not contain the required
  `/polaris/v1/session-token/<redacted>/<redacted>` path. The isolated transport
  result was 2 passed and 1 failed.

Source tracing showed that a pending redaction replaced the current segment,
cleared `redactNext`, and continued before classifying the original segment.
Consequently, a sensitive label consumed as the preceding label's value could
not protect its own following value.

The loop now classifies the original segment before replacing it. When a
pending redaction consumes that segment, the saved classification determines
whether the following segment must also be redacted. Exact labels and compound
labels with hyphen, underscore, or dot boundaries keep the same classifier;
encoded input, query handling, and benign substring guards are unchanged.

This state machine is deliberately fail closed. A consumed secret value such
as `CANARY_COMMAND` is indistinguishable from a path label and is itself a
compound sensitive label, so it re-arms redaction and can hide the next label
as well as its value. The retained command, key, session, token, and clipboard
canary fixtures now assert that safer over-redacted output instead of weakening
their values. Separate benign substring and compound controls prove that broad
substring matching was not introduced.

Final local results for this fix round:

- Debug application and test compile/link: exit 0; the production redaction
  object was recompiled and both binaries linked.
- Focused chained-label table: 8 passed, 0 failed, 0 skipped.
- Captured transport log canary: 3 passed, 0 failed, 0 skipped; the valid
  endpoint logged `/polaris/v1/session-token/<redacted>/<redacted>` and emitted
  no trailing canary.
- `RedactionTest`: 24 passed, 0 failed, 0 skipped.
- `PolarisApiClientTest`: 57 passed, 0 failed, 0 skipped.
- Twenty fresh repetitions of both focused regressions produced 40 of 40 clean
  processes and 220 passed test cases, with no canary output.
- Canonical headless run: 344 passed, 11 failed, 0 skipped. The failures remain
  exactly eight `DeckQmlTest` and three `DeckSurfaceRendererTest` instances of
  the established `Deck OpenGL context creation failed` environment gate.
- ThreadSanitizer remains unavailable because the documented runtime is not
  installed. No TSan run or pass is claimed, and no package was installed.
- No live network, display, Xvfb, Polaris, Sunshine, ww-DevBox, 10G, push,
  merge, or release operation was used.

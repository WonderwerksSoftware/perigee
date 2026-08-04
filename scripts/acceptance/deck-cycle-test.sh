#!/usr/bin/env bash
set -euo pipefail

MODE=automated
CYCLES=""
LIVE=0
TEMPORARY=""
DECK_OPEN=0
DRIVER="${PERIGEE_DECK_CYCLE_DRIVER:-}"

usage()
{
    cat <<'EOF'
Usage: scripts/acceptance/deck-cycle-test.sh [options]

Plan or run repeated Perigee Deck open/close cycles. The default is a dry run.
The script never selects or discovers a remote host.

Options:
  --mode automated|physical  Select driver operation. Default: automated.
  --cycles COUNT             Override 100 automated or 20 physical cycles.
  --live                     Permit driver execution. Also requires
                             PERIGEE_ACCEPT_LIVE_TESTS=YES and an absolute
                             PERIGEE_DECK_CYCLE_DRIVER path.
  -h, --help                 Show this help text.

Live driver interface:
  DRIVER snapshot            Print a deterministic input/capture state.
  DRIVER open                Open Deck for an automated cycle.
  DRIVER close               Close Deck for an automated cycle.
  DRIVER physical-cycle      Coordinate one physical-controller cycle.

Snapshot contents are compared in private temporary files. They are never
printed by this harness.
EOF
}

fail()
{
    printf 'Perigee Deck cycle test failed: %s\n' "$1" >&2
    exit 2
}

cleanup()
{
    trap - EXIT
    trap '' HUP INT TERM
    if [ "$LIVE" -eq 1 ] && [ "$DECK_OPEN" -eq 1 ] && [ -n "$DRIVER" ]; then
        "$DRIVER" close >/dev/null 2>&1 || true
    fi
    if [ -n "$TEMPORARY" ]; then
        rm -rf -- "$TEMPORARY"
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

while [ "$#" -gt 0 ]; do
    case "$1" in
        --mode)
            [ "$#" -ge 2 ] || fail "--mode requires a value"
            MODE="$2"
            shift 2
            ;;
        --cycles)
            [ "$#" -ge 2 ] || fail "--cycles requires a value"
            CYCLES="$2"
            shift 2
            ;;
        --live)
            LIVE=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "unknown option"
            ;;
    esac
done

case "$MODE" in
    automated|physical) ;;
    *) fail "--mode must be automated or physical" ;;
esac

if [ -z "$CYCLES" ]; then
    if [ "$MODE" = automated ]; then
        CYCLES=100
    else
        CYCLES=20
    fi
fi

case "$CYCLES" in
    ''|*[!0-9]*) fail "--cycles must be an integer from 1 through 1000" ;;
esac
[ "$CYCLES" -ge 1 ] && [ "$CYCLES" -le 1000 ] \
    || fail "--cycles must be an integer from 1 through 1000"

if [ "$LIVE" -eq 0 ]; then
    printf 'DRY RUN: no driver was invoked and no host was contacted.\n'
    printf 'Plan: %s cycles: %s\n' "$MODE" "$CYCLES"
    exit 0
fi

[ "${PERIGEE_ACCEPT_LIVE_TESTS:-}" = YES ] \
    || fail "set PERIGEE_ACCEPT_LIVE_TESTS=YES in addition to --live"
[ -n "$DRIVER" ] || fail "set PERIGEE_DECK_CYCLE_DRIVER to an absolute executable path"
case "$DRIVER" in
    /*) ;;
    *) fail "PERIGEE_DECK_CYCLE_DRIVER must be an absolute path" ;;
esac
[ -f "$DRIVER" ] && [ -x "$DRIVER" ] && [ ! -L "$DRIVER" ] \
    || fail "PERIGEE_DECK_CYCLE_DRIVER must be an executable regular file, not a symlink"

umask 077
TEMPORARY="$(mktemp -d "${TMPDIR:-/tmp}/perigee-deck-cycle.XXXXXX")" \
    || fail "cannot create private temporary directory"
INITIAL="$TEMPORARY/initial-state"
FINAL="$TEMPORARY/final-state"
DRIVER_ERRORS="$TEMPORARY/driver-errors"

"$DRIVER" snapshot > "$INITIAL" 2> "$DRIVER_ERRORS" \
    || fail "initial input-state snapshot failed"
[ -s "$INITIAL" ] || fail "initial input-state snapshot is empty"

cycle=1
while [ "$cycle" -le "$CYCLES" ]; do
    if [ "$MODE" = automated ]; then
        DECK_OPEN=1
        "$DRIVER" open >/dev/null 2>> "$DRIVER_ERRORS" || fail "Deck open failed"
        "$DRIVER" close >/dev/null 2>> "$DRIVER_ERRORS" || fail "Deck close failed"
        DECK_OPEN=0
    else
        "$DRIVER" physical-cycle >/dev/null 2>> "$DRIVER_ERRORS" \
            || fail "physical Deck cycle failed"
    fi
    cycle=$((cycle + 1))
done

"$DRIVER" snapshot > "$FINAL" 2>> "$DRIVER_ERRORS" \
    || fail "final input-state snapshot failed"
[ -s "$FINAL" ] || fail "final input-state snapshot is empty"
cmp --silent "$INITIAL" "$FINAL" || fail "input state changed after Deck cycles"

printf 'Initial input-state SHA-256: '
sha256sum "$INITIAL" | awk '{print $1}'
printf 'Final input-state SHA-256: '
sha256sum "$FINAL" | awk '{print $1}'
printf 'PASS: %s %s cycles; input state matches the initial snapshot.\n' "$CYCLES" "$MODE"

#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)"
SOURCE_ROOT="$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd -P)"
PAIRED_SUMMARY=""

usage()
{
    cat <<'EOF'
Usage: scripts/acceptance/collect-environment.sh [options]

Print a redacted, read-only local acceptance-environment report to stdout.
The default mode does not read paired profiles and does not contact a host.

Options:
  --include-paired-summary FILE  Include a prepared local permission summary.
                                 Also requires
                                 PERIGEE_ACCEPT_SENSITIVE_COLLECTION=YES.
  -h, --help                     Show this help text.
EOF
}

fail()
{
    printf 'Perigee environment collection failed: %s\n' "$1" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --include-paired-summary)
            [ "$#" -ge 2 ] || fail "--include-paired-summary requires a file"
            PAIRED_SUMMARY="$2"
            shift 2
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

if [ -n "$PAIRED_SUMMARY" ]; then
    [ "${PERIGEE_ACCEPT_SENSITIVE_COLLECTION:-}" = YES ] \
        || fail "set PERIGEE_ACCEPT_SENSITIVE_COLLECTION=YES to include a paired summary"
    [ -f "$PAIRED_SUMMARY" ] && [ -r "$PAIRED_SUMMARY" ] && [ ! -L "$PAIRED_SUMMARY" ] \
        || fail "paired summary must be a readable regular file, not a symlink"
    summary_size="$(stat --format='%s' -- "$PAIRED_SUMMARY")" \
        || fail "cannot inspect paired summary"
    [ "$summary_size" -le 1048576 ] || fail "paired summary exceeds 1 MiB"
fi

redact_stream()
{
    python3 -B -c '
import ipaddress
import re
import sys

text = sys.stdin.read()

text = re.sub(
    r"-----BEGIN ([^-\r\n]*(?:CERTIFICATE|PRIVATE KEY)[^-\r\n]*)-----.*?"
    r"-----END \1-----",
    "<redacted-certificate>",
    text,
    flags=re.IGNORECASE | re.DOTALL,
)
text = re.sub(
    r"(?i)\b(?:gh[oprsu]_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,})\b",
    "<redacted-token>",
    text,
)
text = re.sub(
    r"(?i)(\bBearer\s+)[A-Za-z0-9._~+/-]+=*",
    r"\1<redacted-token>",
    text,
)
text = re.sub(
    r"(?i)([?&](?:token|auth|authorization|key|certificate|cert|uuid)=)[^&\s]+",
    r"\1<redacted-token>",
    text,
)
text = re.sub(
    r"(?<![0-9A-Fa-f])(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f])",
    "<redacted-address>",
    text,
)
text = re.sub(
    r"(?i)(?<![0-9a-f])(?:[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})(?![0-9a-f])",
    "<redacted-uuid>",
    text,
)

def redact_ip(match):
    value = match.group(0)
    try:
        ipaddress.ip_address(value)
    except ValueError:
        return value
    return "<redacted-address>"

text = re.sub(r"(?<![0-9.])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9.])", redact_ip, text)
# Require a colon so ordinary hexadecimal words and commit IDs are not
# candidates. The address parser rejects version labels and other invalid
# colon-delimited text while accepting compressed IPv6 forms.
text = re.sub(
    r"(?<![0-9A-Fa-f:])(?=[0-9A-Fa-f:]*:)[0-9A-Fa-f:]{2,}(?![0-9A-Fa-f:])",
    redact_ip,
    text,
)

def redact_label(match):
    label = match.group(1)
    lowered = label.lower()
    if "uuid" in lowered:
        replacement = "<redacted-uuid>"
    elif any(word in lowered for word in ("address", "ipv4", "ipv6", "ip", "mac")):
        replacement = "<redacted-address>"
    elif "cert" in lowered or any(
        value in lowered for value in ("private_key", "private-key", "private key")
    ):
        replacement = "<redacted-certificate>"
    else:
        replacement = "<redacted-token>"
    return label + match.group(2) + replacement

text = re.sub(
    r"(?im)(?<![A-Za-z0-9_-])((?:[A-Za-z0-9_-]+[ _-]+)*"
    r"(?:token|authorization|certificate(?:[ _-]+fingerprint)?|cert(?:[ _-]+fingerprint)?|"
    r"uuid|ipv4|ipv6|ip(?:[ _-]+address)?|address|mac|secret|password|credential|"
    r"private[ _-]+key)"
    r"(?:[ _-]+[A-Za-z0-9_-]+)*)"
    r"(\s*[:=]\s*)[^\r\n]+",
    redact_label,
    text,
)
sys.stdout.write(text)
'
}

command_value()
{
    local unavailable="$1"
    shift
    if command -v "$1" >/dev/null 2>&1; then
        "$@" 2>&1 || printf '%s\n' "$unavailable"
    else
        printf '%s\n' "$unavailable"
    fi
}

os_description="unavailable"
if [ -r /etc/os-release ]; then
    os_description="$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release | head -n 1 | sed 's/^"//; s/"$//')"
    [ -n "$os_description" ] || os_description=unavailable
fi

compositor="unavailable"
if command -v pgrep >/dev/null 2>&1 && pgrep -x kwin_wayland >/dev/null 2>&1; then
    compositor="kwin_wayland"
elif [ -n "${XDG_CURRENT_DESKTOP:-}" ]; then
    compositor="${XDG_CURRENT_DESKTOP} (process not confirmed)"
fi

{
    printf '# Perigee live-acceptance environment\n\n'
    printf 'Collection mode: local read-only; no host contact\n'
    printf 'Operating system: %s\n' "$os_description"
    printf 'Session type: %s\n' "${XDG_SESSION_TYPE:-unavailable}"
    printf 'Desktop: %s\n' "${XDG_CURRENT_DESKTOP:-unavailable}"
    printf 'Compositor: %s\n' "$compositor"
    printf 'SDL video-driver override: %s\n' "${SDL_VIDEODRIVER:-not forced}"
    printf 'VA-API driver override: %s\n' "${LIBVA_DRIVER_NAME:-not forced}"
    printf 'Qt version: '
    command_value unavailable qmake6 -query QT_VERSION
    printf 'SDL version: '
    command_value unavailable sdl2-config --version
    printf 'Perigee commit: '
    command_value unavailable git -C "$SOURCE_ROOT" rev-parse HEAD
    printf 'Polaris commit: '
    polaris_root="${PERIGEE_POLARIS_ROOT:-$SOURCE_ROOT/../../../perigee-polaris}"
    if [ -d "$polaris_root/.git" ] || [ -f "$polaris_root/.git" ]; then
        command_value unavailable git -C "$polaris_root" rev-parse HEAD
    else
        printf '%s\n' unavailable
    fi
    printf '\nGPU and renderer summary:\n'
    if [ "${PERIGEE_COLLECT_GLXINFO:-}" = YES ] && [ -n "${DISPLAY:-}" ]; then
        command_value unavailable glxinfo -B
    elif command -v lspci >/dev/null 2>&1; then
        lspci -nnk 2>&1 | awk '/VGA compatible controller|3D controller|Display controller/{show=1} show{print; if ($0 !~ /VGA compatible controller|3D controller|Display controller/ && $0 !~ /^[[:space:]]/){show=0}}' || printf '%s\n' unavailable
    else
        printf '%s\n' 'unavailable (no lspci; set PERIGEE_COLLECT_GLXINFO=YES with a working X11 display for GL details)'
    fi
    printf '\nDisplay outputs:\n'
    if [ "${PERIGEE_COLLECT_DISPLAY_OUTPUTS:-}" = YES ]; then
        command_value unavailable kscreen-doctor -o
    else
        printf '%s\n' 'not collected (set PERIGEE_COLLECT_DISPLAY_OUTPUTS=YES in a display-capable session)'
    fi
    printf '\nPaired permission summary:\n'
    if [ -n "$PAIRED_SUMMARY" ]; then
        cat -- "$PAIRED_SUMMARY"
    else
        printf 'not collected; supply a prepared summary with the explicit sensitive-collection gate\n'
    fi
} | redact_stream

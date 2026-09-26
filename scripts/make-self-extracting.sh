#!/usr/bin/env bash
# Build ONE executable file that carries the whole WssdApi bundle inside it.
#
#   wssd-api.run  =  shell stub  +  "__WSSD_PAYLOAD__" marker  +  tar.gz bytes
#
# On first start the stub unpacks the payload into a content-addressed runtime
# directory (so upgrades never collide and re-running is free), seeds a
# writable data directory with config.json / config.yaml / sapo/ if they are
# not there yet, then execs the real binary with LD_LIBRARY_PATH pointing at
# the unpacked libraries. Subsequent starts skip straight to the exec.
#
# Usage:
#   scripts/make-self-extracting.sh --payload dist.tar.gz --id <hash> \
#       --version <label> --output wssd-api.run
set -euo pipefail

PAYLOAD=""
OUTPUT=""
ID=""
VERSION="dev"

die() { echo "make-self-extracting: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --payload) PAYLOAD="${2:-}"; shift 2 ;;
        --output)  OUTPUT="${2:-}";  shift 2 ;;
        --id)      ID="${2:-}";      shift 2 ;;
        --version) VERSION="${2:-}"; shift 2 ;;
        -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument '$1'" ;;
    esac
done

[[ -f "$PAYLOAD" ]] || die "--payload must point at an existing tar.gz"
[[ -n "$OUTPUT"  ]] || die "--output is required"
[[ -n "$ID"      ]] || ID="$(sha256sum "$PAYLOAD" | cut -c1-16)"

STUB="$(mktemp)"
trap 'rm -f "$STUB"' EXIT

cat > "$STUB" <<STUB_EOF
#!/usr/bin/env bash
# WssdApi self-contained executable — build ${VERSION}, payload ${ID}.
#
# Layout once unpacked:
#   <runtime>/bin/WssdApi  <runtime>/lib/*.so  <runtime>/config.json  <runtime>/sapo/
#
# Environment:
#   WSSD_RUNTIME_DIR  where the payload is unpacked   (default /opt/wssd-api/runtime)
#   WSSD_WORKDIR      writable data/working directory (default /var/lib/wssd-api)
#
# Flags: --extract DIR | --runtime-dir | --version | --help  (anything else is
# passed through to the application).
set -euo pipefail

WSSD_BUILD_VERSION="${VERSION}"
WSSD_PAYLOAD_ID="${ID}"
STUB_EOF

cat >> "$STUB" <<'STUB_EOF'
SELF="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/$(basename -- "${BASH_SOURCE[0]}")"

die() { echo "wssd-api: $*" >&2; exit 1; }

# Byte offset of the payload: everything after the marker line.
payload_offset() {
    local line
    line="$(grep -a -n '^__WSSD_PAYLOAD__$' "$SELF" | head -n 1 | cut -d: -f1)"
    [[ -n "$line" ]] || die "corrupt executable: payload marker not found"
    echo $((line + 1))
}

extract_to() {
    local dest="$1" tmp
    mkdir -p "$dest"
    tmp="$(mktemp -d "${dest%/}.tmp.XXXXXX")"
    tail -n "+$(payload_offset)" "$SELF" | tar -xz -C "$tmp"
    # Atomic-ish publish: a half-written runtime must never look ready.
    if [[ -e "$dest/.ready" ]]; then
        rm -rf "$tmp"
        return 0
    fi
    touch "$tmp/.ready"
    rm -rf "$dest"
    mv "$tmp" "$dest"
}

# A writable runtime directory, preferring the system location and degrading
# to per-user paths so an unprivileged `./wssd-api.run` still works.
pick_runtime_root() {
    local candidates=()
    [[ -n "${WSSD_RUNTIME_DIR:-}" ]] && candidates+=("$WSSD_RUNTIME_DIR")
    candidates+=("/opt/wssd-api/runtime" "${XDG_CACHE_HOME:-$HOME/.cache}/wssd-api" "/tmp/wssd-api-$(id -u)")
    local c
    for c in "${candidates[@]}"; do
        if mkdir -p "$c" 2>/dev/null && [[ -w "$c" ]]; then
            echo "$c"
            return 0
        fi
    done
    die "no writable runtime directory (tried: ${candidates[*]}) — set WSSD_RUNTIME_DIR"
}

RUNTIME_ROOT=""
RUNTIME=""
ensure_runtime() {
    RUNTIME_ROOT="$(pick_runtime_root)"
    RUNTIME="${RUNTIME_ROOT}/${WSSD_PAYLOAD_ID}"
    [[ -e "${RUNTIME}/.ready" ]] || extract_to "$RUNTIME"
    [[ -x "${RUNTIME}/bin/WssdApi" ]] || die "unpacked runtime is missing bin/WssdApi"
}

case "${1:-}" in
    --version)
        echo "WssdApi self-contained bundle: build ${WSSD_BUILD_VERSION}, payload ${WSSD_PAYLOAD_ID}"
        exit 0 ;;
    --help|-h)
        sed -n '2,/^set -euo/p' "$SELF" | sed 's/^# \{0,1\}//' | head -n -1
        exit 0 ;;
    --extract)
        [[ -n "${2:-}" ]] || die "--extract needs a directory"
        mkdir -p "$2"
        tail -n "+$(payload_offset)" "$SELF" | tar -xz -C "$2"
        echo "wssd-api: extracted to $2"
        exit 0 ;;
    --runtime-dir)
        ensure_runtime
        echo "$RUNTIME"
        exit 0 ;;
esac

ensure_runtime

# Working directory: Drogon's loadConfigFile("config.json") and the Sapo file
# state-store are both relative to the CWD, and the runtime directory is
# treated as immutable, so the service runs in a separate writable data dir.
DATA="${WSSD_WORKDIR:-/var/lib/wssd-api}"
if ! mkdir -p "$DATA" 2>/dev/null || [[ ! -w "$DATA" ]]; then
    DATA="$PWD"
fi

# Seed editable files ONCE. Never overwrite: the operator's edited config.json
# must survive an upgrade of the executable.
for f in config.json config.yaml; do
    [[ -f "${RUNTIME}/${f}" && ! -e "${DATA}/${f}" ]] && cp "${RUNTIME}/${f}" "${DATA}/${f}"
done
if [[ -d "${RUNTIME}/sapo" && ! -d "${DATA}/sapo" ]]; then
    cp -r "${RUNTIME}/sapo" "${DATA}/sapo"
fi
mkdir -p "${DATA}/logs"

export LD_LIBRARY_PATH="${RUNTIME}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
cd "$DATA"
exec "${RUNTIME}/bin/WssdApi" "$@"
STUB_EOF

printf '__WSSD_PAYLOAD__\n' >> "$STUB"
cat "$PAYLOAD" >> "$STUB"

mv "$STUB" "$OUTPUT"
trap - EXIT
chmod +x "$OUTPUT"
echo "make-self-extracting: wrote $OUTPUT ($(du -h "$OUTPUT" | cut -f1), payload ${ID})"

#!/usr/bin/env bash
# Fetch the prebuilt Sapo Engine install tree from GitHub Packages (ghcr.io).
#
# WHY THIS EXISTS
#   SapoEngine is a private repository. WssdApi's build must therefore never
#   depend on an anonymous `git clone` of it: the engine's release workflow
#   publishes its `cmake --install` prefix as an OCI artifact, and this script
#   unpacks that artifact into a local prefix which CMake consumes through
#   -DWSSD_SAPO_PREFIX=... (or the SAPO_PREFIX environment variable).
#
# AUTHENTICATION
#   In GitHub Actions nothing extra is needed: the workflow's own GITHUB_TOKEN
#   can pull the package once the package grants this repository read access
#   (package settings -> "Manage Actions access" -> add rhyolite-prime/WssdApi).
#   That is the whole point of shipping the engine as a *package* rather than
#   cloning it — the default token is repository-scoped and CANNOT read another
#   private repo's source, but it CAN read a package that has been shared with
#   this repo. No PAT, no deploy key, no secret rotation.
#
#   Locally either export a token with read:packages
#       export SAPO_GITHUB_TOKEN=ghp_...
#   or log in once yourself and pass --skip-login
#       docker login ghcr.io
#
# Usage (from the repo root):
#   ./scripts/fetch-sapo-engine.sh                    # pinned version -> build/sapo-prefix
#   ./scripts/fetch-sapo-engine.sh --dest /opt/sapo   # explicit destination prefix
#   ./scripts/fetch-sapo-engine.sh --version 0.4.1    # override sapo-engine.lock
#   ./scripts/fetch-sapo-engine.sh --skip-login       # reuse an existing docker login
#   ./scripts/fetch-sapo-engine.sh --no-require-sapoc # tolerate a missing bin/sapoc
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK_FILE="${REPO_ROOT}/sapo-engine.lock"

VERSION=""
DEST="${REPO_ROOT}/build/sapo-prefix"
IMAGE_OVERRIDE="${SAPO_ENGINE_IMAGE:-}"
IMAGE_INNER_PATH="${SAPO_ENGINE_IMAGE_PATH:-/opt/sapo}"
DO_LOGIN=1
REQUIRE_SAPOC=1

usage() {
    sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' | head -n -1
}

die() {
    echo "fetch-sapo-engine: $*" >&2
    exit 1
}

info() {
    echo "[sapo-fetch] $*"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="${2:-}"; shift 2 ;;
        --version=*) VERSION="${1#*=}"; shift ;;
        --dest) DEST="${2:-}"; shift 2 ;;
        --dest=*) DEST="${1#*=}"; shift ;;
        --image) IMAGE_OVERRIDE="${2:-}"; shift 2 ;;
        --image=*) IMAGE_OVERRIDE="${1#*=}"; shift ;;
        --skip-login) DO_LOGIN=0; shift ;;
        --no-require-sapoc) REQUIRE_SAPOC=0; shift ;;
        -h | --help) usage; exit 0 ;;
        *) die "unknown argument '$1' (try --help)" ;;
    esac
done

if [[ -z "$VERSION" && -f "$LOCK_FILE" ]]; then
    VERSION="$(sed -n 's/^[[:space:]]*SAPO_ENGINE_VERSION=//p' "$LOCK_FILE" | tail -n 1 | tr -d '[:space:]')"
fi

[[ -n "$VERSION" ]] ||
    die "no engine version: pass --version or set SAPO_ENGINE_VERSION in $(basename "$LOCK_FILE")"

# ghcr.io image names must be lower-case; the lock value is used verbatim as the
# tag so `sapo-engine.lock` and the registry stay trivially comparable.
IMAGE="${IMAGE_OVERRIDE:-ghcr.io/rhyolite-prime/sapo-engine:${VERSION}}"

command -v docker >/dev/null 2>&1 ||
    die "docker is required to unpack the engine artifact (or set WSSD_SAPO_PREFIX / SAPO_PREFIX to an existing SapoEngine install tree and skip this script)"

if [[ "$DO_LOGIN" == "1" ]]; then
    TOKEN="${SAPO_GITHUB_TOKEN:-${GITHUB_TOKEN:-}}"
    if [[ -n "$TOKEN" ]]; then
        # --password-stdin keeps the token out of the process list and logs.
        printf '%s' "$TOKEN" | docker login ghcr.io \
            --username "${GITHUB_ACTOR:-sapo-ci}" --password-stdin >/dev/null ||
            die "docker login ghcr.io failed — does the token have read:packages, and has the package been shared with this repository?"
    else
        info "no SAPO_GITHUB_TOKEN/GITHUB_TOKEN set; relying on an existing docker login"
    fi
fi

info "pulling ${IMAGE}"
if ! docker pull "$IMAGE"; then
    cat >&2 <<EOF
[sapo-fetch] FAILED to pull ${IMAGE}

Likely causes, in order:
  1. The package has not been shared with this repository. In SapoEngine's
     package settings (https://github.com/orgs/rhyolite-prime/packages) open
     sapo-engine -> Package settings -> "Manage Actions access" and add
     rhyolite-prime/WssdApi with the Read role. Without this, even a valid
     GITHUB_TOKEN gets a 403, because the token is scoped to THIS repository.
  2. Version ${VERSION} was never published. Check SapoEngine's tags against
     $(basename "$LOCK_FILE") and re-run its publish workflow.
  3. No usable credential. Locally: export SAPO_GITHUB_TOKEN=<read:packages>,
     or run 'docker login ghcr.io' and retry with --skip-login.
EOF
    exit 1
fi

STAGING="$(mktemp -d)"
CONTAINER_ID=""
cleanup() {
    [[ -n "$CONTAINER_ID" ]] && docker rm -f "$CONTAINER_ID" >/dev/null 2>&1
    rm -rf "$STAGING"
    return 0
}
trap cleanup EXIT

CONTAINER_ID="$(docker create "$IMAGE")"

# `docker cp <src>/. <dest>` copies the directory *contents*, so the prefix
# lands with bin/ include/ lib/ share/ at its root, exactly like the tree
# `cmake --install` produced.
mkdir -p "$STAGING/prefix"
docker cp "${CONTAINER_ID}:${IMAGE_INNER_PATH}/." "$STAGING/prefix"

# Replace the destination atomically-ish: wipe first so a previous version's
# stale headers cannot be picked up by an incremental build.
rm -rf "$DEST"
mkdir -p "$(dirname "$DEST")"
mv "$STAGING/prefix" "$DEST"

# ---- verify the tree is a usable SapoEngine install prefix -------------------
missing=()
[[ -f "${DEST}/lib/libsapo_core.a" ]] || missing+=("lib/libsapo_core.a")
[[ -f "${DEST}/lib/cmake/SapoEngine/SapoEngineConfig.cmake" ]] ||
    missing+=("lib/cmake/SapoEngine/SapoEngineConfig.cmake")
[[ -d "${DEST}/include/sapo" ]] || missing+=("include/sapo/")

# SapoEngineTargets-release.cmake registers an import check on bin/sapoc, so a
# tree without it makes find_package() FATAL_ERROR with a confusing message.
if [[ "$REQUIRE_SAPOC" == "1" && ! -e "${DEST}/bin/sapoc" ]]; then
    missing+=("bin/sapoc (required by SapoEngineTargets-release.cmake)")
fi

if [[ ${#missing[@]} -gt 0 ]]; then
    die "artifact ${IMAGE} is not a complete SapoEngine install prefix; missing: ${missing[*]}"
fi

if [[ -f "${DEST}/lib/cmake/SapoEngine/SapoEngineConfigVersion.cmake" ]]; then
    PUBLISHED="$(sed -n 's/^set(PACKAGE_VERSION[[:space:]]*"\(.*\)").*/\1/p' \
        "${DEST}/lib/cmake/SapoEngine/SapoEngineConfigVersion.cmake" | head -n 1)"
    if [[ -n "$PUBLISHED" && "$PUBLISHED" != "$VERSION" ]]; then
        die "artifact tag ${VERSION} reports PACKAGE_VERSION ${PUBLISHED} — republish the artifact or fix $(basename "$LOCK_FILE")"
    fi
    info "engine ${PUBLISHED:-$VERSION} ready"
fi

info "prefix: ${DEST}"
echo
echo "Configure WssdApi against it with:"
echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWSSD_SAPO_PREFIX='${DEST}'"

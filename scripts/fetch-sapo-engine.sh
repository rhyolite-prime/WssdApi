#!/usr/bin/env bash
# Fetch the prebuilt Sapo Engine install tree into a local prefix (vendor/sapo).
#
# WHY THIS EXISTS
#   SapoEngine is a private repository. WssdApi's build must therefore never
#   depend on an anonymous `git clone` of it. The engine's build workflow
#   publishes its `cmake --install` prefix twice:
#
#     * as a workflow ARTIFACT named `sapo-dist` (a zip containing sapo-dist/)
#       on EVERY successful run — this is the reliable source;
#     * as an OCI package on ghcr.io (ghcr.io/rhyolite-prime/sapo-engine:<ver>)
#       but ONLY on tag pushes / workflow_dispatch. If the engine has not been
#       tagged, that package simply does not exist and `docker pull` fails with
#       "manifest unknown".
#
#   This script therefore tries both, in order, and unpacks whichever works
#   into a prefix CMake consumes through -DWSSD_SAPO_PREFIX=... (or SAPO_PREFIX).
#
# AUTHENTICATION
#   OCI mode:      a token with read:packages (GITHUB_TOKEN works in Actions once
#                  the package has been shared with this repository), or an
#                  existing `docker login ghcr.io` plus --skip-login.
#   Artifact mode: the GitHub CLI (`gh`) authenticated against a token that can
#                  read the SapoEngine repository. The default repo-scoped
#                  GITHUB_TOKEN CANNOT do this, so in CI supply a PAT/App token
#                  as the SAPO_ENGINE_TOKEN secret (repo scope, read-only).
#
# Usage (from the repo root):
#   ./scripts/fetch-sapo-engine.sh                      # auto: oci then artifact -> build/sapo-prefix
#   ./scripts/fetch-sapo-engine.sh --dest vendor/sapo   # explicit destination prefix
#   ./scripts/fetch-sapo-engine.sh --mode artifact      # only the workflow artifact
#   ./scripts/fetch-sapo-engine.sh --mode oci           # only the ghcr.io package
#   ./scripts/fetch-sapo-engine.sh --version 0.4.1      # override sapo-engine.lock
#   ./scripts/fetch-sapo-engine.sh --run-id 123456789   # artifact from a specific run
#   ./scripts/fetch-sapo-engine.sh --skip-login         # reuse an existing docker login
#   ./scripts/fetch-sapo-engine.sh --no-require-sapoc   # tolerate a missing bin/sapoc
#   ./scripts/fetch-sapo-engine.sh --allow-version-mismatch
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK_FILE="${REPO_ROOT}/sapo-engine.lock"

VERSION=""
DEST="${REPO_ROOT}/build/sapo-prefix"
IMAGE_OVERRIDE="${SAPO_ENGINE_IMAGE:-}"
# Empty = auto-detect. The publish job builds `FROM scratch` + `COPY sapo-dist/ /`,
# so the install tree sits at "/", not /opt/sapo; older images used /opt/sapo.
IMAGE_INNER_PATH="${SAPO_ENGINE_IMAGE_PATH:-}"
MODE="${SAPO_ENGINE_MODE:-auto}"          # auto | oci | artifact
ENGINE_REPO="${SAPO_ENGINE_REPO:-rhyolite-prime/SapoEngine}"
ENGINE_WORKFLOW="${SAPO_ENGINE_WORKFLOW:-}"   # optional workflow file name filter
ENGINE_BRANCH="${SAPO_ENGINE_BRANCH:-}"       # optional branch filter (default: any)
ARTIFACT_NAME="${SAPO_ENGINE_ARTIFACT:-sapo-dist}"
RUN_ID=""
DO_LOGIN=1
REQUIRE_SAPOC=1
ALLOW_VERSION_MISMATCH="${SAPO_ALLOW_VERSION_MISMATCH:-0}"

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
        --mode) MODE="${2:-}"; shift 2 ;;
        --mode=*) MODE="${1#*=}"; shift ;;
        --engine-repo) ENGINE_REPO="${2:-}"; shift 2 ;;
        --engine-repo=*) ENGINE_REPO="${1#*=}"; shift ;;
        --run-id) RUN_ID="${2:-}"; shift 2 ;;
        --run-id=*) RUN_ID="${1#*=}"; shift ;;
        --skip-login) DO_LOGIN=0; shift ;;
        --no-require-sapoc) REQUIRE_SAPOC=0; shift ;;
        --allow-version-mismatch) ALLOW_VERSION_MISMATCH=1; shift ;;
        -h | --help) usage; exit 0 ;;
        *) die "unknown argument '$1' (try --help)" ;;
    esac
done

case "$MODE" in
    auto | oci | artifact) ;;
    *) die "--mode must be auto, oci or artifact (got '$MODE')" ;;
esac

if [[ -z "$VERSION" && -f "$LOCK_FILE" ]]; then
    VERSION="$(sed -n 's/^[[:space:]]*SAPO_ENGINE_VERSION=//p' "$LOCK_FILE" | tail -n 1 | tr -d '[:space:]')"
fi

[[ -n "$VERSION" ]] ||
    die "no engine version: pass --version or set SAPO_ENGINE_VERSION in $(basename "$LOCK_FILE")"

# Absolute destination so `cmake -DWSSD_SAPO_PREFIX=` never depends on cwd.
case "$DEST" in
    /*) ;;
    *) DEST="${REPO_ROOT}/${DEST}" ;;
esac

STAGING="$(mktemp -d)"
CONTAINER_ID=""
cleanup() {
    [[ -n "$CONTAINER_ID" ]] && docker rm -f "$CONTAINER_ID" >/dev/null 2>&1
    rm -rf "$STAGING"
    return 0
}
trap cleanup EXIT

# The install tree can land one or more directories deep (e.g. the artifact zip
# holds sapo-dist/, an old OCI image held /opt/sapo). Find the directory that
# actually looks like a SapoEngine prefix and echo it.
locate_prefix() {
    local root="$1" candidate
    while IFS= read -r candidate; do
        # candidate = <prefix>/lib/cmake/SapoEngine -> three levels up
        echo "$(cd -- "${candidate}/../../.." && pwd)"
        return 0
    done < <(find "$root" -maxdepth 4 -type d -path '*/lib/cmake/SapoEngine' 2>/dev/null)
    return 1
}

install_prefix() {
    # $1 = directory that IS the prefix; replace $DEST with it.
    local src="$1"
    rm -rf "$DEST"
    mkdir -p "$(dirname "$DEST")"
    mv "$src" "$DEST"
}

# ---- source 1: OCI package on ghcr.io ---------------------------------------
try_oci() {
    command -v docker >/dev/null 2>&1 || {
        info "docker not available; skipping the ghcr.io package"
        return 1
    }

    if [[ "$DO_LOGIN" == "1" ]]; then
        local token="${SAPO_GITHUB_TOKEN:-${GITHUB_TOKEN:-}}"
        if [[ -n "$token" ]]; then
            # --password-stdin keeps the token out of the process list and logs.
            if ! printf '%s' "$token" | docker login ghcr.io \
                --username "${GITHUB_ACTOR:-sapo-ci}" --password-stdin >/dev/null 2>&1; then
                info "docker login ghcr.io failed (token lacks read:packages?); skipping OCI"
                return 1
            fi
        else
            info "no SAPO_GITHUB_TOKEN/GITHUB_TOKEN set; relying on an existing docker login"
        fi
    fi

    local images=()
    if [[ -n "$IMAGE_OVERRIDE" ]]; then
        images=("$IMAGE_OVERRIDE")
    else
        # The engine tags releases "v0.4.0" but publishes the package as "0.4.0";
        # `latest` is what a workflow_dispatch publish produces. Try all three.
        images=("ghcr.io/rhyolite-prime/sapo-engine:${VERSION}"
                "ghcr.io/rhyolite-prime/sapo-engine:v${VERSION}"
                "ghcr.io/rhyolite-prime/sapo-engine:latest")
    fi

    local image="" found=""
    for image in "${images[@]}"; do
        info "pulling ${image}"
        if docker pull "$image" >/dev/null 2>&1; then
            found="$image"
            break
        fi
        info "  not available (manifest unknown or no access)"
    done
    [[ -n "$found" ]] || return 1

    CONTAINER_ID="$(docker create "$found")"
    mkdir -p "$STAGING/oci"
    # `docker cp <src>/. <dest>` copies the directory *contents*.
    docker cp "${CONTAINER_ID}:${IMAGE_INNER_PATH:-/}/." "$STAGING/oci" >/dev/null
    docker rm -f "$CONTAINER_ID" >/dev/null 2>&1 || true
    CONTAINER_ID=""

    local prefix
    prefix="$(locate_prefix "$STAGING/oci")" || {
        info "image ${found} does not contain lib/cmake/SapoEngine; skipping"
        return 1
    }
    info "unpacked ${found} (prefix ${prefix#$STAGING/oci/})"
    install_prefix "$prefix"
    return 0
}

# ---- source 2: `sapo-dist` workflow artifact --------------------------------
try_artifact() {
    command -v gh >/dev/null 2>&1 || {
        info "gh CLI not available; skipping the workflow artifact"
        return 1
    }
    # gh reads GH_TOKEN/GITHUB_TOKEN; SAPO_ENGINE_TOKEN (a PAT that can read the
    # private engine repo) wins when present.
    local gh_env=()
    if [[ -n "${SAPO_ENGINE_TOKEN:-}" ]]; then
        gh_env=(env "GH_TOKEN=${SAPO_ENGINE_TOKEN}" "GITHUB_TOKEN=${SAPO_ENGINE_TOKEN}")
    else
        gh_env=(env)
    fi

    local run_id="$RUN_ID"
    if [[ -z "$run_id" ]]; then
        local list_args=(run list -R "$ENGINE_REPO" --status success --limit 30
                         --json databaseId,headBranch,workflowName,createdAt)
        [[ -n "$ENGINE_BRANCH" ]] && list_args+=(--branch "$ENGINE_BRANCH")
        [[ -n "$ENGINE_WORKFLOW" ]] && list_args+=(--workflow "$ENGINE_WORKFLOW")
        run_id="$("${gh_env[@]}" gh "${list_args[@]}" \
            --jq 'map(select(.workflowName | test("Sapo"; "i"))) | .[0].databaseId' 2>/dev/null || true)"
    fi
    if [[ -z "$run_id" || "$run_id" == "null" ]]; then
        info "no successful SapoEngine run found in ${ENGINE_REPO} (token cannot read it?)"
        return 1
    fi

    info "downloading artifact '${ARTIFACT_NAME}' from ${ENGINE_REPO} run ${run_id}"
    mkdir -p "$STAGING/art"
    if ! "${gh_env[@]}" gh run download "$run_id" -R "$ENGINE_REPO" \
            -n "$ARTIFACT_NAME" -D "$STAGING/art" >/dev/null 2>&1; then
        info "artifact download failed (expired, missing, or token lacks access)"
        return 1
    fi

    # The artifact is a zip that itself contains sapo-dist.zip.
    local z
    while IFS= read -r z; do
        (cd "$(dirname "$z")" && unzip -qo "$(basename "$z")")
    done < <(find "$STAGING/art" -name '*.zip')

    local prefix
    prefix="$(locate_prefix "$STAGING/art")" || {
        info "artifact does not contain a SapoEngine install tree"
        return 1
    }
    install_prefix "$prefix"
    return 0
}

ok=1
case "$MODE" in
    oci)      try_oci      && ok=0 ;;
    artifact) try_artifact && ok=0 ;;
    auto)     if try_oci; then ok=0; else
                  info "ghcr.io package unavailable — falling back to the workflow artifact"
                  try_artifact && ok=0
              fi ;;
esac

if [[ "$ok" != "0" ]]; then
    cat >&2 <<EOF
[sapo-fetch] FAILED to obtain SapoEngine ${VERSION}

Tried (mode=${MODE}):
  * ghcr.io/rhyolite-prime/sapo-engine:{${VERSION},v${VERSION},latest}
  * the '${ARTIFACT_NAME}' workflow artifact of ${ENGINE_REPO}

Fix one of these:
  1. PUBLISH THE PACKAGE. SapoEngine only pushes to ghcr.io on a tag push or a
     manual run, so an untagged engine has NO package and docker reports
     "manifest unknown". Tag the engine (git tag v${VERSION} && git push --tags)
     or run its "Build Sapo Engine" workflow via workflow_dispatch, then make
     sure the package is shared with this repository:
     https://github.com/orgs/rhyolite-prime/packages -> sapo-engine ->
     Package settings -> "Manage Actions access" -> add rhyolite-prime/WssdApi.
  2. USE THE ARTIFACT. Add a repo secret SAPO_ENGINE_TOKEN (a PAT or GitHub App
     token with read access to ${ENGINE_REPO}); this script then pulls the
     sapo-dist artifact from the engine's latest successful build. The default
     GITHUB_TOKEN cannot do this — it is scoped to this repository only.
  3. BUILD IT YOURSELF and point CMake at the install tree:
       cmake -S . -B build -DWSSD_SAPO_PREFIX=/path/to/sapo-dist
EOF
    exit 1
fi

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
    die "the fetched tree is not a complete SapoEngine install prefix; missing: ${missing[*]}"
fi

if [[ -f "${DEST}/lib/cmake/SapoEngine/SapoEngineConfigVersion.cmake" ]]; then
    PUBLISHED="$(sed -n 's/^set(PACKAGE_VERSION[[:space:]]*"\(.*\)").*/\1/p' \
        "${DEST}/lib/cmake/SapoEngine/SapoEngineConfigVersion.cmake" | head -n 1)"
    if [[ -n "$PUBLISHED" && "$PUBLISHED" != "$VERSION" ]]; then
        if [[ "$ALLOW_VERSION_MISMATCH" == "1" ]]; then
            info "WARNING: fetched engine reports ${PUBLISHED}, sapo-engine.lock pins ${VERSION}"
            info "         configure with -DWSSD_SAPO_VERSION_OVERRIDE or update the lock file"
        else
            die "fetched engine reports PACKAGE_VERSION ${PUBLISHED} but $(basename "$LOCK_FILE") pins ${VERSION} — update the lock file, republish the artifact, or pass --allow-version-mismatch"
        fi
    fi
    info "engine ${PUBLISHED:-$VERSION} ready"
fi

info "prefix: ${DEST}"
echo
echo "Configure WssdApi against it with:"
echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWSSD_SAPO_PREFIX='${DEST}'"

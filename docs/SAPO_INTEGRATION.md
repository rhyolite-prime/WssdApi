# Sapo Engine integration

WssdApi embeds the [Sapo Engine](https://github.com/rhyolite-prime/SapoEngine)
as its USSD workflow runtime. One process-wide `VirtualMachine` serves every
USSD turn; gateways keep their own wire models and the engine only ever sees
provider-neutral JSON. The engine repository is public and is consumed as
a published artifact, never cloned — see [Build](#build).

```
                Nalo webhook                    Hubtel webhook
  handset ---> POST /api/v1/ussd-interaction/nalo|hubtel ---> handset
                            |
                 UssdInteractionController (thin: validate -> normalize -> render)
                            |
              ProviderAdapters (per-aggregator request/response mapping)
                            |
                 UssdSessionOrchestrator (one Sapo turn per interaction)
                       |                |
        UssdWorkflowResolver      SapoEngineService (owns the VM)
     (registry/file/default)       |      |       |
                              blueprints state  HTTP
                              registry  store  transport
```

## Request flow

1. The controller validates the gateway payload (`USERID/MSISDN` for Nalo —
   `SESSIONID` is optional since the provider passes none — `SessionId/Mobile`
   for Hubtel; malformed JSON is a 400, exactly as before) and normalizes it
   into a `UssdInteraction`. The Nalo session key is the normalized MSISDN.
2. The orchestrator resolves the workflow for the dialed service: the
   session's pinned flow binding on continuation turns, else
   `wssd_registry` by `ussd_code` (the row's `executable` column **is** the
   Sapo blueprint JSON), then
   `sapo/workflows/<key>.json`, then the `default` fallback blueprint.
   Initiation turns pin the binding for the turns that follow. Resolution
   also attaches the `ussd_subscriptions.id` whose `ussd_code` matches, for
   the audit row's `business_subscription_id` (sessions without one skip
   the audit insert).
3. The blueprint is registered under a deterministic id (`wssd:<code>`,
   content-hashed so steady-state turns skip re-registration).
4. Exactly one engine turn runs **off the Drogon IO threads** on the
   `BlockingRunner` pool. The engine session id is deterministic —
   `nalo:<msisdn>` / `hubtel:<SessionId>` — so gateway retries resume the
   same checkpoint instead of forking the conversation. Initiation turns
   (Hubtel `Initiation`, Nalo dial string) force a fresh start; other turns
   resume, and a resume that fails (expired checkpoint) is retried once as
   a fresh start. Resumes pass the subscriber's raw reply as a scalar — the
   engine stores the resume argument verbatim into the prompt's
   `input_variable`, so the context object is start-only.
5. The `ExecutionOutcome` is rendered to a provider-neutral `UssdResult`
   (`awaiting_input`/`suspended` => continue, everything terminal => close)
   and the adapter encodes the gateway's response shape. Failures degrade to
   a close-session apology; engine internals are logged, never sent to the
   handset.
6. A best-effort audit row is upserted into `ussd_sessions`. Audit failures
   never fail the subscriber's turn.

Hubtel `Release` callbacks cancel the engine session instead of running it.

## Provider contracts

### Nalo — `POST /api/v1/ussd-interaction/nalo`

Request: `{USERID, MSISDN, USERDATA, MSGTYPE, NETWORK, SESSIONID}`.
Response: `{USERID, MSISDN, SESSIONID, USERDATA, MSGTYPE, MSG}` with
`MSGTYPE=true` to continue and `false` to end. Request ids are echoed back.

Nalo passes no usable session id and no explicit new/continue flag, so the
normalized MSISDN **is** the session key (one live flow per subscriber,
matching the single handset USSD channel; the provider's `SESSIONID`, when
present, is echoed back and kept in the audit payload but ignored for
identity). A `USERDATA` dial string (contains `*` and `#`) marks initiation
and force-starts a fresh flow — redialling mid-flow restarts it; any other
input continues the session's pinned flow (see below). The dial string
identifies the service and is not treated as a menu choice (also exposed
to blueprints as `$dial_code`).

Initiation pins the resolved blueprint to the session id in
`UssdFlowBindingStore` (Redis `wssd:ussd:binding:<session>` entries with
the checkpoint TTL, in-memory map without Redis), because continuation
turns no longer carry the dial string needed to resolve it. Hubtel uses
the same path with its passed `SessionId` as the key. A missed binding
(expired, cold store) falls back to resolving from the request.

### Hubtel — `POST /api/v1/ussd-interaction/hubtel`

Request: `{Type, Mobile, SessionId, ServiceCode, Message, Operator,
Sequence, ClientState, Platform}`. `Type` is matched case-insensitively:
`Initiation` starts, `Response` continues, `Release` cancels.
Response: `{SessionId, Type, Message, Label, ClientState, DataType,
FieldType}` with `Type` `"Response"`/`"Release"`. `ClientState` carries the
engine resume cursor while the session is open and `"End"` on release.
`DataType`/`FieldType` are derived from the blueprint prompt
(`menu`/`input`/`display`, `text`/`password`); prompts may override them via
`data_type`/`field_type` fields.

## What blueprints see

Every turn starts or resumes with this input context (SEL `$`-variables):

| Key            | Meaning                                              |
|----------------|------------------------------------------------------|
| `input`        | Subscriber input (`""` on a dial-string first hit)   |
| `user_input`   | Raw input, always verbatim                           |
| `msisdn`/`phone` | Subscriber number                                  |
| `session_id`   | Gateway session id                                   |
| `provider`     | `nalo` or `hubtel`                                   |
| `network`      | Mobile network                                       |
| `service_key`  | Gateway service id (USERID / ServiceCode)            |
| `ussd_code`    | Dial code when known, else the service key           |
| `dial_code`    | Dial string (`*123#`) or `""`                        |
| `sequence`     | Hubtel sequence (0 for Nalo)                         |
| `client_state` | Hubtel client state                                  |
| `is_start`     | True for the turn that starts the workflow           |

Suspend with an `action` + `prompt_config.message` to continue the
conversation; `terminate` with `output.message` (and/or `message`) to end it.
`interaction_type: menu|input|pin|display` drives the Hubtel hints. Structured
`prompt.options` arrays are rendered as a numbered list when the message text
does not already list them.

Sample blueprints live in `sapo/workflows/` (`default.json` is the fallback
menu, `wssd-demo.json` a multi-step flow). Blueprints must be forward-only
DAGs: the engine validator flags any control-flow cycle (e.g. a menu
"try again" edge back to an earlier node) as a `WARNING`, so invalid input
terminates the session with a redial message instead of looping. Validate
any blueprint with the engine CLI before deploying it:

```
sapoc validate sapo/workflows/my-service.json
```

Blueprints stored in `wssd_registry.executable` are hot-reloadable: the next
turn that resolves them re-registers the changed text automatically, with
validate-before-replace (a bad edit is rejected and the previous flow keeps
serving instead of being removed). Open sessions keep running the version
they started with — turns re-register their own pinned text — so keep edits
to a live flow's node ids additive, or drain sessions before incompatible
ones.

## Configuration

Engine settings live in the `SapoEnginePlugin` block of `config.json`
(workflow/config/state directories, Redis URL/TTL/pool, log level, default
blueprint, blocking threads). Every value has a `SAPO_*` environment
override (`SAPO_REDIS_URL` or `SAPO_REDIS_HOST`/`SAPO_REDIS_PORT`,
`SAPO_WORKFLOW_DIR`, `SAPO_CONFIG_PATH`, `SAPO_STATE_DIR`, `SAPO_LOG_LEVEL`,
`SAPO_DEFAULT_WORKFLOW`) — see `SapoSettings::applyEnvOverrides`.

`sapo/sapo-config.json` is the optional engine provider config (connector
settings, `config.*`/`secret.*` bindings for blueprints, engine limits).
Secrets declared there are redacted from every engine log line.

Ownership split: the plugin owns the **runtime environment** (Redis URL,
log level, state dir, paths) via `config.json` + `SAPO_*` env; the engine
file owns only the **execution profile** (`engine.workers`,
`engine.max_node_visits`, `engine.max_depth`, `engine.default_timeout_ms`).
Do **not** add `engine.state_redis`, `engine.state_dir`, or
`engine.log_level` to `sapo-config.json` — `VirtualMachine::start()` takes
over state-store and log-level selection whenever those keys exist, and
fails startup outright when `state_redis` is present but unresolvable (even
a well-formed `{"$env": "SAPO_REDIS_URL"}` breaks boot when the variable is
unset, because the mere presence of the key disables the file-store
fallback). Keep Redis configuration in exactly one place: `redis_url` /
`SAPO_REDIS_URL`, which the plugin validates with a dial check at startup.

## State, scaling, timeouts

* **Single node:** the default file store (`sapo/state-store/`, git-ignored)
  survives restarts. Good for dev and single-instance deployments.
* **Multi node:** set `redis_url` (or `SAPO_REDIS_URL`). Sessions become
  `sapo:session:<id>` hashes with a 15-minute TTL; resumes are
  compare-and-swap guarded so a duplicated gateway request cannot
  double-execute a side-effecting node. On Redis Cluster set
  `redis_atomic_index: false` (see `RedisStateStoreOptions::atomic_index`).
  The engine uses its own pooled socket client against the same Redis the
  Drogon cache client uses; key prefixes do not overlap.
* **Threads:** gateway IO stays on Drogon's threads; engine turns run on the
  `blocking_threads` pool (default 8). Blueprint HTTP calls (`http.*`
  commands) go through `HostDrogonTransport`, honoring each request's
  timeout (default 10 s) without parking IO threads.
* **Timeouts:** gateways hang up after seconds, so keep blueprint HTTP
  downstream fast and put side effects after the first prompt (a retried
  first hit re-runs the workflow head idempotently only if it is
  side-effect free). Engine-side `wait` nodes and prompt timeouts fire via
  the background scheduler tick (500 ms).

## Build

The engine is built from source and installed into `vendor/sapo`. The expected
version is declared once, in `sapo-engine.lock`:

```
SAPO_ENGINE_VERSION=0.4.0
```

CMake enforces that exact version (`find_package(SapoEngine <v> EXACT)`), so a
stale or mismatched engine fails at configure time instead of quietly changing
behaviour. CI compares the version the engine actually builds against this file
and stops with an explicit message if they drift.

### Getting the engine

Clone [SapoEngine](https://github.com/rhyolite-prime/SapoEngine), build it and
install the result straight into `vendor/sapo` — the same three commands CI
runs:

```sh
git clone https://github.com/rhyolite-prime/SapoEngine.git
cmake -S SapoEngine -B SapoEngine/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DSAPO_ENABLE_CPR=OFF \
  -DSAPO_ENABLE_REDIS=ON \
  -DSAPO_BUILD_TESTS=OFF
cmake --build SapoEngine/build -j"$(nproc)"
cmake --install SapoEngine/build --prefix "$PWD/vendor/sapo"
```

The install tree is `bin/sapoc`, `include/sapo/`, `lib/libsapo_core.a` and
`lib/cmake/SapoEngine/`. Build it with **GCC 13**: the engine is C++23, and
linking an engine built by a different libstdc++ into WssdApi produces
`undefined reference to std::__cxx11::basic_string<...>::_M_replace_cold`.

### CMake resolution order

1. **A prebuilt install tree**, from the first of these holding a *complete*
   tree (`lib/libsapo_core.a` + `lib/cmake/SapoEngine/` + `bin/sapoc`):
   `-DWSSD_SAPO_PREFIX=<dir>`, then `$SAPO_PREFIX`, then `vendor/sapo`. The
   package exports the library as `Sapo::core`.
2. **A source build** via `FetchContent`, only when no prebuilt tree was found
   and `WSSD_SAPO_SOURCE` permits it. The repository is public, so this needs
   no credentials; a `git ls-remote` pre-flight turns an unreachable remote
   into one clear message.

`WSSD_SAPO_SOURCE` is `AUTO` (prebuilt, else source — the default for local
dev), `ON` (source only, ignoring any prebuilt tree), or `OFF` (prebuilt only —
what CI uses, so it can never build an unpinned engine).

Committed under `vendor/sapo` are the **headers and the CMake package config
only** — enough for the engine-free tests to compile, never enough to link. The
binaries are git-ignored on purpose: they come from the install step above.

### Local build

```sh
# after installing the engine into vendor/sapo as shown above
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 \
  -DWSSD_SAPO_PREFIX="$PWD/vendor/sapo" -DWSSD_SAPO_SOURCE=OFF
cmake --build build -j"$(nproc)"
./build/WssdApi            # serves :5107 per config.json
```

Requires CMake ≥ 3.28 (Ubuntu 22.04's apt CMake is 3.22). Run the binary from a
directory that contains `config.json` and `sapo/`: both are resolved relative to
the **working directory**, which is why a run from `cmake-build-debug/` reports
`0 workflow(s)`.

### CI

`.github/workflows/build_wssd-api.yml` runs on `master`/`dev` pushes and PRs to
`master` with `permissions: {contents: read, packages: read}`. It reads
`sapo-engine.lock`, pulls the artifact **first** (a bad pin or an unshared
package fails in seconds, not after a Drogon build), then builds Drogon
`v1.9.13` into a cached prefix at `/home/runner/drogon-prefix`, configures with
`-DWSSD_SAPO_SOURCE=OFF`, builds, validates the blueprints with the artifact's
own `sapoc`, and runs `ctest`.

### Bumping the engine

1. Tag and publish the release in SapoEngine.
2. Update `SAPO_ENGINE_VERSION` in `sapo-engine.lock`.
3. Commit — CI verifies the artifact exists and matches before building.

The artifact is a static archive, so the publishing workflow must keep using
the same runner image and distro compiler as WssdApi's CI (`ubuntu-22.04`);
toolchain drift between the two repos is an ABI bug, not a rebuild.

## Testing

* `test/test_sapo_adapters.cc` covers the gateway mapping layer
  (normalization, dial-code handling, Hubtel/Nalo rendering, outcome
  rendering, JSON bridging) with no database or engine required:
  `ctest --test-dir build`.
* Live gateway checks: point Nalo/Hubtel callbacks at
  `/api/v1/ussd-interaction/nalo` and `/hubtel`, dial the service code, and
  watch for `[ussd]`/`[sapo]` log lines plus `ussd_sessions` audit rows.
* Engine-only blueprint debugging stays in SapoEngine (`sapoc run`,
  record/replay transports) — no gateway needed.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Every turn answers "Service temporarily unavailable" | Engine failed to start — read the `[sapo]` errors at boot (bad blueprint in `sapo/workflows/`, unreadable state dir). `[sapo] engine warning at startup` lines are non-fatal by design (blueprint validator advisories such as cycle reports) |
| `config: environment variable 'redis://…' … is not set` at boot | `sapo-config.json` contains `{"$env": "<url>"}` — a URL pasted where an env var *name* belongs. Delete `engine.state_redis` from the file and set Redis via `redis_url` / `SAPO_REDIS_URL` instead; discard the bad file on `git pull` conflicts |
| `config: engine.state_redis must be a non-empty redis:// URL string` at boot | `state_redis` is present but unresolvable (unset env var, typo'd URL). Same fix: remove the key from `sapo-config.json`, configure Redis only through the plugin |
| `unknown top-level config section 'redis_url'` at boot / engine did not start | `redis_url` was added to `sapo-config.json`, where nothing reads it (and the engine rejects unknown sections, fatally). It belongs in Drogon `config.json` (`plugins.SapoEnginePlugin.config.redis_url`) or `SAPO_REDIS_URL` env — remove it from the engine file |
| "Service not available for this code" | No registry blueprint matched (static file blueprints were removed, so the DB is the only tier) — compare `wssd_registry.ussd_code` against the tried spellings in the `[ussd] no workflow ...` line |
| Resolution falls through to "Service not available" (no fallback menu exists since the static blueprints were removed) | No registry row matched the tried key spellings (or the row's `executable` is empty/invalid) — the `[ussd] no workflow ... (tried keys: ...)` line names every spelling attempted: compare against `wssd_registry.ussd_code`. Also look for `[ussd] registry entry ... has no executable blueprint` (row matched, empty flow) or `[ussd] registry lookup failed` (DB error) |
| Hubtel session restarts mid-menu | State store lost (file store on an ephemeral disk, or Redis flushed) — use Redis in production |
| Blueprint HTTP calls fail | Read the `[ussd] turn failed code=...` line: `HTTP_ERROR` = transport problem (DNS/TLS/refused/timeout — the message names the URL and cause), `HTTP_STATUS_ERROR` = downstream answered non-2xx, `VALIDATION_ERROR` = non-absolute URL in the blueprint (absolute `http(s)` URLs only). Redirects are followed (5 hops); express transient retries as node `retry` policies, not redials |
| Logs show `resume failed ... restarting session` | Checkpoint expired between lookup and resume (gateway retry after a long pause) — expected, self-heals. Turn *execution* errors (e.g. `HTTP_ERROR` from a blueprint API call) no longer restart: they log `turn execution failed` with the underlying message instead |
| CI fails at **Verify the engine prefix** with a version message | `SapoEngine` now builds a different version than `sapo-engine.lock` pins. Update the one line in `sapo-engine.lock`, or point `SAPO_ENGINE_REF` at the matching tag |
| `undefined reference to ..._M_replace_cold` at link | Engine and app were built by different compilers. Build both with GCC 13 |
| `Cannot reach https://github.com/rhyolite-prime/SapoEngine.git` at configure | No prebuilt tree was found and the clone failed (network/proxy). Install the engine into `vendor/sapo` as shown above and pass `-DWSSD_SAPO_PREFIX=$PWD/vendor/sapo` |
| `No usable SapoEngine 0.4.0 was found` at configure | No *complete* prebuilt tree in any candidate prefix while `WSSD_SAPO_SOURCE=OFF` forbids a source build. Fetch the artifact, or re-configure without `-DWSSD_SAPO_SOURCE=OFF` |
| `SapoEngine prefix … does not provide version 0.4.0` | That prefix was built from a different engine version than `sapo-engine.lock` pins — the `EXACT` check rejecting a stale artifact is the feature. Re-fetch, or bump the lock file deliberately |
| `skipping incomplete SapoEngine prefix …/vendor/sapo` | Expected and harmless: only headers + CMake package config are committed there, never `libsapo_core.a`. It only matters if it is the *only* prefix found (then see the "No usable SapoEngine" row) |
| `CMake 3.28 or higher is required. You are running version 3.22.1` | Ubuntu 22.04's apt CMake is too old for this project. Install a pinned 3.31 from Kitware — the CI workflow already does |

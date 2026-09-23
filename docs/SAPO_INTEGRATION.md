# Sapo Engine integration

WssdApi embeds the [Sapo Engine](https://github.com/rhyolite-prime/SapoEngine)
as its USSD workflow runtime. One process-wide `VirtualMachine` serves every
USSD turn; gateways keep their own wire models and the engine only ever sees
provider-neutral JSON.

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

1. The controller validates the gateway payload (`USERID/MSIDN/SESSIONID`
   for Nalo, `SessionId/Mobile` for Hubtel; malformed JSON is a 400, exactly
   as before) and normalizes it into a `UssdInteraction`.
2. The orchestrator resolves the workflow for the dialed service:
   `wssd_registry` by `ussd_code`, then by `merchant_identifier` (the row's
   `executable` column **is** the Sapo blueprint JSON), then
   `sapo/workflows/<key>.json`, then the `default` fallback blueprint.
3. The blueprint is registered under a deterministic id (`wssd:<code>`,
   content-hashed so steady-state turns skip re-registration).
4. Exactly one engine turn runs **off the Drogon IO threads** on the
   `BlockingRunner` pool. The engine session id is deterministic —
   `nalo:<SESSIONID>` / `hubtel:<SessionId>` — so gateway retries resume the
   same checkpoint instead of forking the conversation, and a resume that
   fails (expired checkpoint) is retried once as a fresh start.
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

Nalo sends no explicit new/continue flag: the first hit for an unknown
session id starts the workflow, later hits resume it. When the first hit's
`USERDATA` is the dial string itself (`*123#`), it identifies the service
and is not treated as a menu choice (also exposed to blueprints as
`$dial_code`).

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
menu, `wssd-demo.json` a multi-step flow). Validate any blueprint with the
engine CLI before deploying it:

```
sapoc validate sapo/workflows/my-service.json
```

Blueprints stored in `wssd_registry.executable` are hot-reloadable: the next
turn that resolves them re-registers the changed text automatically.

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

CMake resolves the engine automatically:

1. `vendor/sapo/lib/libsapo_core.a` — a `cmake --install` tree of SapoEngine
   (the repo currently vendors headers + CMake config only; drop the built
   `.a` here for hermetic offline builds), else
2. `FetchContent` from `rhyolite-prime/SapoEngine` (`WSSD_SAPO_GIT_TAG`,
   default `master`, with `SAPO_ENABLE_REDIS=ON`, tests off).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/WssdApi            # serves :5107 per config.json
```

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
| Every turn answers "Service temporarily unavailable" | Engine failed to start — read the `[sapo]` errors at boot (bad blueprint in `sapo/workflows/`, unreadable state dir) |
| "Service not available for this code" | No registry/file/default blueprint matched — check `wssd_registry.executable` for the code and `sapo/workflows/default.json` |
| Hubtel session restarts mid-menu | State store lost (file store on an ephemeral disk, or Redis flushed) — use Redis in production |
| Blueprint HTTP calls fail | Downstream unreachable from the API host, or a relative URL in the blueprint (absolute `http(s)` URLs only) |
| Logs show `resume failed ... restarting session` | Checkpoint expired between lookup and resume (gateway retry after a long pause) — expected, self-heals |

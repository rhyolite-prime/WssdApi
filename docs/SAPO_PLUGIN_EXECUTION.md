# Sapo plugin execution endpoint

WssdApi exposes the embedded [Sapo Engine](https://github.com/rhyolite-prime/SapoEngine)
as a **plugin runtime** over plain REST: a client submits a DSL blueprint (or
the id of one shipped in `sapo/plugins/`) plus its variables as JSON, the API
executes it off the IO threads, and the response carries whatever the
blueprint decided to return — a gateway checkout URL, a verification result,
or a `suspended`/`awaiting_input` state the caller later resumes.

Shipped sample plugins (validated at boot, listed below): **Hubtel web
checkout**, **Paystack checkout**, **Paystack verify**, **Magma Pay USSD
checkout**. Description, inputs and outputs of each are in
`sapo/plugins/*.json`. For engine plumbing (state store, secrets, transport,
build wiring) read [SAPO_INTEGRATION.md](SAPO_INTEGRATION.md) first.

```
POST /api/v1/sapo/plugins/execute                  # run a plugin
POST /api/v1/sapo/plugins/sessions/{sid}/resume    # resume a suspended one
GET  /api/v1/sapo/plugins/sessions/{sid}           # session snapshot (debug)
GET  /api/v1/sapo/plugins/list                     # registered blueprints
```

## `POST /api/v1/sapo/plugins/execute`

Body (all JSON):

| Field            | Type    | Notes |
|------------------|---------|-------|
| `plugin`         | string  | Registered blueprint id (`hubtel_web_checkout`, …); aliases `plugin_id`, `workflow_id`. Either this or `blueprint` is required. |
| `blueprint`      | object/array | Inline DSL blueprint: workflow object `{name?, nodes:[...]}` or a bare node array. Registers under the given id, deterministically hashed. |
| `variables`      | object  | Initial context (execution "meta data"). Alias: `input`. |
| `metadata`       | object  | Extra context: merged first so `variables` wins key conflicts; raw copy preserved under `$metadata`. |
| `session_id`     | string  | Client-pinned session id (e.g. idempotency). Default: `plugin:<slug>:<uuid>`. **Re-using an existing id resumes that session's flow if possible** — pin stable ids for at-most-once execution. |
| `correlation_id` | string  | Tracing id in engine report/logs. Default: `plugin:<session_id>`. |
| `persist`        | bool    | Default `true`: keep the checkpoint so suspended flows can be resumed. `false` => checkpoint removed at completion (one-shot calls). |
| `include_context`| bool    | Default `false`: also return the full execution context in `result.context` (debug only; may contain raw provider payloads). |

### Response envelope

Standard API envelope (`result/message/success/error`). `result` mirrors the
engine's `ExecutionOutcome`:

```json
{
  "success": true,
  "message": "Hubtel checkout initiated; redirect the customer to checkout_url.",
  "result": {
    "plugin": "hubtel_web_checkout",
    "workflow_id": "hubtel_web_checkout",
    "session_id": "plugin:hubtel_web_checkout:5077bb07-…",
    "execution_id": "3b1b8e5c-…",
    "status": "terminated",
    "resumable": false,
    "output": { "checkout_url": "https://paylink.hubtel.com/…", "client_reference": "31bb2f2a-…", "provider": "hubtel", "mode": "web_checkout" },
    "node_visits": 5,
    "elapsed_ms": 7
  },
  "error": {}
}
```

| `result.status`    | `success` | HTTP | Meaning / what to do |
|--------------------|-----------|------|----------------------|
| `terminated`       | true      | 200  | A terminate node answered. `result.output` is the terminate payload — **this is the plugin's direct-response surface**. |
| `completed`        | true      | 200  | Flow ran off the end of a node chain (no terminate). `result.output` holds the final context. Design blueprints with an explicit `terminate` unless you want this. |
| `awaiting_input` / `suspended` | true | 200 | Blueprint suspended at a prompt/wait. `result.prompt` (if any) is the prompt to show; resume via `result.resume_path` (see below). |
| `failed`           | false     | 200  | The blueprint *chose* this failure: `error.code` is the blueprint-defined code (`INVALID_AMOUNT`, `PAYSTACK_INIT_REJECTED`, …), `error.message`/`error.node`/`error.data` explain. |
| `failed`           | false     | 404  | `UNKNOWN_PLUGIN` — no registered plugin has this id and no inline `blueprint` was supplied. |
| —                  | false     | 422  | `BLUEPRINT_REJECTED` — the inline blueprint was invalid (JSON/grammar/sel), the message describes the parse problem. |
| —                  | false     | 503  | Engine not configured/running. |

Whether an execution answers directly (terminate output → 200 + output) or
asynchronously (status `awaiting_input`/`suspended` + `resume_path`, or the
provider calls your webhook later and you resume/check) is a property of the
blueprint, not the endpoint.

### Example — Hubtel web checkout

```bash
curl -X POST .../api/v1/sapo/plugins/execute -H 'Content-Type: application/json' -d '{
  "plugin": "hubtel_web_checkout",
  "variables": {
    "amount": 325.50,
    "description": "WSSD subscription",
    "callback_url": "https://wssd.example.com/hooks/payment",
    "metadata": {"order_id": "ord-1001"}
  }
}'
```

(Requests override config/environment: `gateway_url` swaps the provider base
URL — point it at a mock in tests; `gateway_secret` overrides the secret;
`client_reference` skips the uuid autogeneration when you need your own.)

### `POST /api/v1/sapo/plugins/sessions/{sid}/resume`

For `awaiting_input`/`suspended` flows. Body is the user/provider reply:
`{"input": <any json>}`. The input lands verbatim in the prompt's
`input_variable` and execution continues to the next suspend/finish; same
envelope as execute. `404 SESSION_NOT_FOUND` (expired/unknown checkpoint),
`409 SESSION_NOT_SUSPENDED` (already finished).

### Authoring plugin blueprints

- Required input validation: use a first `condition` node with a relaxed
  expression — unbound $vars must be referenced via `coalesce(x, dflt)` or a
  straight `is_empty(x)` (which throws when `x` is unbound — use
  `is_empty(coalesce(x,'')) == false`); then `on_false` routes to a
  terminate with your `INVALID_REQUEST` code. Blueprint-defined codes and
  messages are the API contract — invent them deliberately.
- HTTP: `command: http.<method>` nodes; `expect`/`either` + a rich `"as":
  failure` capture on rejection; conditional `next` routing via
  `condition`/`choice`; the response body is under `$.body`.
- HTTPS URLs only in the request nodes; secrets via `${secret.<name>}` (see
  `sapo/sapo-config.json` + `SAPO_SECRET_*` env). IDs like payment client
  references auto-generate with `${coalesce(client_reference, uuid())}` in a
  `noop` assign node.
- Timeouts: the default prompt timeout is 5 min (engine default). A waited
  prompt that expires finishes the session `failed` with
  `TIMEOUT`/`no input received…`, unless your blueprint wires a timeout
  capture (`try`/`catch: {as: failure}`).
- **Caveat**: a missing extraction path in an http output map writes the
  *literal reference string* (e.g. `"$.body.data.checkoutUrl"`) into the
  context var — gate success branches on provider-semantic fields
  (`responseCode == '0000'`, `body.status == true`, `body.code == 200`) and
  optionally add `&& !starts_with(coalesce(to_string(x),''),'$.')`.

Blueprint validation (structural + SEL compile, same rules as the server):

```bash
sapoc validate sapo/plugins/*.json --config sapo/sapo-config.json
```

## Operational notes

- Executions run on the BlockingRunner pool (`threads` in the SapoEnginePlugin
  config block); monitor `elapsed_ms` + the `[sapo-plugin]` INFO line per call
  (records plugin, session, status, node visits, elapsed).
- Sessions persist in the configured state store (`sapo/state-store` files in
  dev, Redis in prod per `redis_url`).
- Suspended sessions are also resumable via the operator tooling of the
  engine's own debug CLI; the REST session endpoints exist so clients do not
  need DB access.

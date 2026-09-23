#!/usr/bin/env bash
# End-to-end smoke test for the Sapo-backed USSD webhooks.
#
# No database or Redis required: with no wssd_registry rows the API serves
# sapo/workflows/default.json, and sessions persist in the local file store.
#
# Usage (from the repo root):
#   ./build/WssdApi &                      # terminal 1: run the API
#   ./scripts/test-ussd-endpoints.sh       # terminal 2: run this (default http://localhost:5107)
#   ./scripts/test-ussd-endpoints.sh http://localhost:5107
set -u

BASE_URL="${1:-http://localhost:5107}"
PASS=0
FAIL=0
TS="$(date +%s)"

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "missing required tool: $1 (install it and retry)" >&2
        exit 2
    }
}
need curl
need jq

say_pass() {
    PASS=$((PASS + 1))
    printf '\033[32mPASS\033[0m %s\n' "$1"
}
say_fail() {
    FAIL=$((FAIL + 1))
    printf '\033[31mFAIL\033[0m %s\n' "$1"
    [ -n "${2:-}" ] && printf '     %s\n' "$2"
}

# post <path> <json> -> sets REPLY_CODE / REPLY_BODY ("000" if unreachable)
post() {
    local body_file
    body_file="$(mktemp)"
    REPLY_CODE="$(curl -s -m 15 -o "$body_file" -w '%{http_code}' \
        -H 'Content-Type: application/json' --data "$2" "$BASE_URL$1" 2>/dev/null)" || REPLY_CODE="000"
    REPLY_BODY="$(cat "$body_file" 2>/dev/null)"
    rm -f "$body_file"
}

expect_code() { # $1=want $2=label
    if [ "$REPLY_CODE" = "$1" ]; then
        say_pass "$2 (HTTP $1)"
    else
        say_fail "$2 (want HTTP $1, got $REPLY_CODE)" "$REPLY_BODY"
        return 1
    fi
}

expect_jq() { # $1=filter $2=label  (filter must yield true)
    if printf '%s' "$REPLY_BODY" | jq -e "$1" >/dev/null 2>&1; then
        say_pass "$2"
    else
        say_fail "$2" "$REPLY_BODY"
        return 1
    fi
}

echo "== WSSD USSD smoke test against $BASE_URL =="

# 0. Liveness (PromExporter ships on /metrics)
if curl -s -m 10 -o /dev/null -w '%{http_code}' "$BASE_URL/metrics" 2>/dev/null | grep -q 200; then
    say_pass "server is up (GET /metrics -> 200)"
else
    say_fail "server is up (GET /metrics -> 200)" "is ./build/WssdApi running?"
fi

# 1+2. Nalo: dial -> menu (MSGTYPE true), then "1" -> terminate (MSGTYPE false)
NALO_SESSION="nalo-e2e-$TS"
post /api/v1/ussd-interaction/nalo "$(cat <<JSON
{"USERID": "wssd-test", "MSISDN": "233241234567", "USERDATA": "*123#",
 "MSGTYPE": true, "NETWORK": "MTN", "SESSIONID": "$NALO_SESSION"}
JSON
)"
expect_code 200 "nalo dial" && {
    expect_jq '.MSGTYPE == true' "nalo dial keeps session open (MSGTYPE true)"
    expect_jq '.MSG | contains("Welcome")' "nalo dial renders menu"
    expect_jq '.SESSIONID == "'"$NALO_SESSION"'"' "nalo dial echoes SESSIONID"
}

# NOTE: the continuation deliberately sends a bogus SESSIONID — the Nalo
# session is anchored on the MSISDN, so the flow must continue regardless.
post /api/v1/ussd-interaction/nalo "$(cat <<JSON
{"USERID": "wssd-test", "MSISDN": "233241234567", "USERDATA": "1",
 "MSGTYPE": true, "NETWORK": "MTN", "SESSIONID": "bogus-ignored-$TS"}
JSON
)"
expect_code 200 "nalo continue" && {
    expect_jq '.MSGTYPE == false' "nalo choice closes session (MSGTYPE false)"
    expect_jq '.MSG | contains("balance")' "nalo choice renders balance message"
}

# 2b. Nalo: an unknown choice terminates with a redial message (blueprints
# are forward-only DAGs; the engine validator forbids menu back-edges).
NALO_SESSION_INVALID="nalo-e2e-invalid-$TS"
post /api/v1/ussd-interaction/nalo "$(cat <<JSON
{"USERID": "wssd-test", "MSISDN": "233241234567", "USERDATA": "*123#",
 "MSGTYPE": true, "NETWORK": "MTN", "SESSIONID": "$NALO_SESSION_INVALID"}
JSON
)"
expect_code 200 "nalo invalid-choice dial" || true
post /api/v1/ussd-interaction/nalo "$(cat <<JSON
{"USERID": "wssd-test", "MSISDN": "233241234567", "USERDATA": "9",
 "MSGTYPE": true, "NETWORK": "MTN", "SESSIONID": "bogus-ignored-invalid-$TS"}
JSON
)"
expect_code 200 "nalo invalid choice" && {
    expect_jq '.MSGTYPE == false' "nalo invalid choice closes session"
    expect_jq '.MSG | contains("Invalid choice")' "nalo invalid choice renders redial message"
}

# 3+4. Hubtel: Initiation -> menu, then Response "2" -> Release
HUB_SESSION="hub-e2e-$TS"
post /api/v1/ussd-interaction/hubtel "$(cat <<JSON
{"Type": "Initiation", "Mobile": "233201234567", "SessionId": "$HUB_SESSION",
 "ServiceCode": "713", "Message": "*713#", "Operator": "vodafone",
 "Sequence": 1, "ClientState": "", "Platform": "USSD"}
JSON
)"
expect_code 200 "hubtel initiation" && {
    expect_jq '.Type == "Response"' "hubtel initiation continues (Type Response)"
    expect_jq '.Message | contains("Welcome")' "hubtel initiation renders menu"
    expect_jq '.DataType == "menu"' "hubtel menu prompt sets DataType=menu"
    HUB_STATE="$(printf '%s' "$REPLY_BODY" | jq -r '.ClientState // ""')"
}

post /api/v1/ussd-interaction/hubtel "$(cat <<JSON
{"Type": "Response", "Mobile": "233201234567", "SessionId": "$HUB_SESSION",
 "ServiceCode": "713", "Message": "2", "Operator": "vodafone",
 "Sequence": 2, "ClientState": "${HUB_STATE:-}", "Platform": "USSD"}
JSON
)"
expect_code 200 "hubtel response" && {
    expect_jq '.Type == "Release"' "hubtel choice closes session (Type Release)"
    expect_jq '.Message | contains("help")' "hubtel choice renders help message"
    expect_jq '.ClientState == "End"' "hubtel release sets ClientState=End"
}

# 5. Hubtel Release cancels cleanly
post /api/v1/ussd-interaction/hubtel "$(cat <<JSON
{"Type": "Release", "Mobile": "233201234567", "SessionId": "hub-rel-$TS",
 "ServiceCode": "713", "Message": "", "Operator": "vodafone",
 "Sequence": 3, "ClientState": "route_choice", "Platform": "USSD"}
JSON
)"
expect_code 200 "hubtel release" && {
    expect_jq '.Type == "Release"' "hubtel release acknowledged"
}

# 6-8. Nalo SESSIONID is optional (MSISDN-anchored sessions); the
# subscriber and service key are still mandatory.
post /api/v1/ussd-interaction/nalo '{"USERID": "wssd-test", "MSISDN": "233249999999", "USERDATA": "*123#", "NETWORK": "MTN"}'
expect_code 200 "nalo without SESSIONID works" && {
    expect_jq '.MSGTYPE == true' "nalo SESSIONID-less dial keeps session open"
    expect_jq '.MSG | contains("Welcome")' "nalo SESSIONID-less dial renders menu"
}
post /api/v1/ussd-interaction/nalo '{"USERID": "wssd-test", "USERDATA": "*123#"}'
expect_code 400 "nalo missing MSISDN -> 400" && {
    expect_jq '.success == false' "nalo 400 body reports success=false"
}
post /api/v1/ussd-interaction/hubtel '{"Type": "Response"}'
expect_code 400 "hubtel missing SessionId/Mobile -> 400" && {
    expect_jq '.success == false' "hubtel 400 body reports success=false"
}

# 9-12. Multi-step demo flow (ServiceCode wssd-demo -> sapo/workflows/wssd-demo.json)
DEMO_SESSION="demo-e2e-$TS"
demo_turn() { # $1=message $2=sequence
    post /api/v1/ussd-interaction/hubtel "$(cat <<JSON
{"Type": "Response", "Mobile": "233201234567", "SessionId": "$DEMO_SESSION",
 "ServiceCode": "wssd-demo", "Message": "$1", "Operator": "mtn",
 "Sequence": $2, "ClientState": "", "Platform": "USSD"}
JSON
)"
}
demo_turn "*123#" 1
expect_code 200 "demo turn 1 (name prompt)" && {
    expect_jq '.Type == "Response"' "demo turn 1 continues"
    expect_jq '.Message | contains("recipient name")' "demo turn 1 asks recipient"
}
demo_turn "Kofi" 2
expect_code 200 "demo turn 2 (amount prompt)" && {
    expect_jq '.Message | contains("amount")' "demo turn 2 asks amount"
}
demo_turn "50" 3
expect_code 200 "demo turn 3 (confirm prompt)" && {
    expect_jq '.Message | contains("Confirm")' "demo turn 3 asks confirmation"
}
demo_turn "1" 4
expect_code 200 "demo turn 4 (done)" && {
    expect_jq '.Type == "Release"' "demo turn 4 releases"
    expect_jq '.Message | contains("sent to")' "demo turn 4 renders receipt"
}

echo
echo "== $PASS passed, $FAIL failed =="
if [ "$FAIL" -ne 0 ]; then
    echo "Hint: check the API log for [sapo] (engine boot) and [ussd] (per-turn) lines."
    exit 1
fi

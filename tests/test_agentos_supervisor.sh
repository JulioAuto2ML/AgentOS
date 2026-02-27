#!/usr/bin/env bash
# =============================================================================
# tests/test_agentos_supervisor.sh — Integration tests for agentos-supervisor
# =============================================================================
# Tests the REST API without making actual LLM calls (agent run endpoints
# are tested separately since they need a live LLM backend).
#
# Run with agentos-server also running on :8888, then:
#   bash tests/test_agentos_supervisor.sh

set -euo pipefail

SUPERVISOR_PORT=18889
BIN=./build/src/agentos-supervisor/agentos-supervisor
PASS=0; FAIL=0

GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'

pass() { echo -e "${GREEN}[PASS]${NC} $1"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $1"; FAIL=$((FAIL+1)); }

echo "════════════════════════════════════════"
echo "  AgentOS agentos-supervisor — Tests"
echo "════════════════════════════════════════"
echo ""

# ── Start supervisor ──────────────────────────────────────────────────────────
echo "[INFO] Starting agentos-supervisor on port $SUPERVISOR_PORT..."
$BIN --port $SUPERVISOR_PORT --agents-dir ./agents \
     AGENTOS_LLM_URL=http://localhost:8080 2>/dev/null &
SUP_PID=$!
sleep 1

# Check it's running
if ! kill -0 $SUP_PID 2>/dev/null; then
    echo "[ERROR] agentos-supervisor failed to start"
    exit 1
fi
echo "[INFO] agentos-supervisor PID=$SUP_PID"
echo ""

BASE="http://localhost:$SUPERVISOR_PORT"

# ── Test 1: /health ───────────────────────────────────────────────────────────
echo "[INFO] Test 1: /health"
RESP=$(curl -s "$BASE/health")
if echo "$RESP" | grep -q '"status":"ok"'; then
    AGENTS=$(echo "$RESP" | python3 -c "import json,sys; print(json.load(sys.stdin)['agents'])" 2>/dev/null || echo "?")
    pass "/health → OK ($AGENTS agents loaded)"
else
    fail "/health returned: $RESP"
fi

# ── Test 2: GET /agents ───────────────────────────────────────────────────────
echo ""
echo "[INFO] Test 2: GET /agents"
RESP=$(curl -s "$BASE/agents")
if echo "$RESP" | grep -q '"agents"'; then
    NAMES=$(echo "$RESP" | python3 -c "
import json,sys
agents = json.load(sys.stdin)['agents']
print(', '.join(a['name'] for a in agents))
" 2>/dev/null || echo "?")
    pass "GET /agents → agents: $NAMES"
    echo "   Agents: $NAMES"
else
    fail "GET /agents returned: $RESP"
fi

# ── Test 3: GET /agents/{name} ────────────────────────────────────────────────
echo ""
echo "[INFO] Test 3: GET /agents/sysmonitor"
RESP=$(curl -s "$BASE/agents/sysmonitor")
if echo "$RESP" | grep -q '"name":"sysmonitor"'; then
    STATUS=$(echo "$RESP" | python3 -c "import json,sys; print(json.load(sys.stdin)['status'])" 2>/dev/null || echo "?")
    pass "GET /agents/sysmonitor → status=$STATUS"
else
    fail "GET /agents/sysmonitor returned: $RESP"
fi

# ── Test 4: GET /agents/{unknown} ────────────────────────────────────────────
echo ""
echo "[INFO] Test 4: GET /agents/nonexistent (expect 404)"
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/agents/nonexistent_agent_xyz")
if [ "$HTTP_CODE" = "404" ]; then
    pass "GET /agents/nonexistent → 404"
else
    fail "Expected 404, got $HTTP_CODE"
fi

# ── Test 5: POST /agents/{name}/run missing message ──────────────────────────
echo ""
echo "[INFO] Test 5: POST /agents/sysmonitor/run — missing message field"
RESP=$(curl -s -X POST "$BASE/agents/sysmonitor/run" \
    -H "Content-Type: application/json" \
    -d '{}')
if echo "$RESP" | grep -q '"error"'; then
    pass "Missing message → error response"
else
    fail "Expected error, got: $RESP"
fi

# ── Test 6: POST /agents/reload ───────────────────────────────────────────────
echo ""
echo "[INFO] Test 6: POST /agents/reload"
RESP=$(curl -s -X POST "$BASE/agents/reload" -H "Content-Type: application/json" -d '{}')
if echo "$RESP" | grep -q '"loaded"'; then
    N=$(echo "$RESP" | python3 -c "import json,sys; print(json.load(sys.stdin)['loaded'])" 2>/dev/null || echo "?")
    pass "POST /agents/reload → loaded=$N"
else
    fail "POST /agents/reload returned: $RESP"
fi

# ── Cleanup ───────────────────────────────────────────────────────────────────
echo ""
kill $SUP_PID 2>/dev/null
wait $SUP_PID 2>/dev/null || true
echo "[INFO] agentos-supervisor stopped"

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════"
if [ $FAIL -eq 0 ]; then
    echo -e "Todos los tests pasaron ${GREEN}✓${NC} ($PASS/$((PASS+FAIL)))"
else
    echo -e "${RED}$FAIL tests fallaron${NC} ($PASS/$((PASS+FAIL)) OK)"
fi
echo "════════════════════════════════════════"
[ $FAIL -eq 0 ]

#!/usr/bin/env bash
# =============================================================================
# tests/test_nos_server.sh — Test de integración para aos-server
# =============================================================================
#
# Este script levanta aos-server, llama a cada tool via HTTP y verifica que
# las respuestas tengan el formato correcto. No necesita un LLM — testea
# el servidor MCP directamente con curl.
#
# USO:
#   cd ~/Documents/GitHub/AgentOS
#   bash tests/test_nos_server.sh
#
# REQUISITOS:
#   - cmake --build build --target aos-server (ya compilado)
#   - curl, python3 (para pretty-print del JSON)
# =============================================================================

set -euo pipefail

BINARY="./build/src/aos-server/aos-server"
HOST="localhost"
PORT="18888"   # puerto alternativo para no chocar con una instancia real
BASE_URL="http://${HOST}:${PORT}"
SESSION="test-session-$$"

# ── colores ────────────────────────────────────────────────────────────────────
GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { echo -e "${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; FAILURES=$((FAILURES+1)); }
info() { echo -e "${YELLOW}[INFO]${NC} $1"; }

FAILURES=0

# ── verificar que el binario existe ───────────────────────────────────────────
if [[ ! -f "$BINARY" ]]; then
    echo -e "${RED}ERROR:${NC} Binario no encontrado: $BINARY"
    echo "Compilá primero con: cmake --build build --target aos-server"
    exit 1
fi

# ── levantar aos-server en background ─────────────────────────────────────────
info "Iniciando aos-server en puerto $PORT..."
"$BINARY" --port "$PORT" &
SERVER_PID=$!

# Limpiar al salir (Ctrl+C o fin del script)
cleanup() {
    info "Deteniendo aos-server (PID $SERVER_PID)..."
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Esperar que levante
sleep 1
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    fail "aos-server no pudo iniciarse"
    exit 1
fi
pass "aos-server iniciado (PID $SERVER_PID)"

# ── helper: llamar una tool por MCP ───────────────────────────────────────────
# El protocolo MCP requiere primero abrir una sesión SSE, pero podemos
# simplificar el test pasando el session_id directamente en el POST.
call_tool() {
    local tool_name="$1"
    local arguments="$2"   # JSON object string, e.g. '{"path":"/"}'

    curl -s -X POST \
        "${BASE_URL}/message?session_id=${SESSION}" \
        -H "Content-Type: application/json" \
        -d "{
            \"jsonrpc\": \"2.0\",
            \"id\": 1,
            \"method\": \"tools/call\",
            \"params\": {
                \"name\": \"${tool_name}\",
                \"arguments\": ${arguments}
            }
        }"
}

# ── helper: verificar que la respuesta tiene un campo ─────────────────────────
has_field() {
    local response="$1"
    local field="$2"
    echo "$response" | python3 -c "import sys,json; d=json.load(sys.stdin); assert '$field' in str(d)" 2>/dev/null
}

echo ""
echo "════════════════════════════════════════"
echo "  AgentOS aos-server — Tests Fase 1"
echo "════════════════════════════════════════"
echo ""

# ── TEST 1: sysinfo ────────────────────────────────────────────────────────────
info "Test 1: sysinfo"
resp=$(call_tool "sysinfo" "{}")
if has_field "$resp" "cpu" && has_field "$resp" "memory" && has_field "$resp" "disk"; then
    pass "sysinfo → contiene cpu, memory, disk"
    # Mostrar datos reales de Luke
    echo "$resp" | python3 -c "
import sys, json
d = json.loads(sys.stdin.read())
content = json.loads(d['result']['content'][0]['text'])
cpu = content['cpu']
mem = content['memory']
disk = content['disk']
print(f'   CPU: {cpu[\"usage_pct\"]:.1f}% ({cpu[\"cores_online\"]} cores)')
print(f'   RAM: {mem[\"used_mb\"]}MB usados de {mem[\"total_mb\"]}MB ({mem[\"used_pct\"]:.1f}%)')
print(f'   Disco: {disk[\"used_mb\"]}MB usados de {disk[\"total_mb\"]}MB ({disk[\"used_pct\"]:.1f}%)')
print(f'   Uptime: {content[\"uptime\"][\"human\"]}')
" 2>/dev/null || echo "   (no se pudo parsear el detalle)"
else
    fail "sysinfo → respuesta inesperada: $resp"
fi

echo ""

# ── TEST 2: list_dir ───────────────────────────────────────────────────────────
info "Test 2: list_dir (/tmp)"
resp=$(call_tool "list_dir" '{"path":"/tmp"}')
if has_field "$resp" "entries"; then
    pass "list_dir → respuesta contiene 'entries'"
else
    fail "list_dir → respuesta inesperada: $resp"
fi

echo ""

# ── TEST 3: read_file ──────────────────────────────────────────────────────────
info "Test 3: read_file (/proc/version)"
resp=$(call_tool "read_file" '{"path":"/proc/version"}')
if has_field "$resp" "content"; then
    pass "read_file → respuesta contiene 'content'"
    echo "$resp" | python3 -c "
import sys, json
d = json.loads(sys.stdin.read())
c = json.loads(d['result']['content'][0]['text'])
print('   Kernel: ' + c['content'][:60].strip())
" 2>/dev/null || true
else
    fail "read_file → respuesta inesperada: $resp"
fi

echo ""

# ── TEST 4: write_file + read_file ────────────────────────────────────────────
info "Test 4: write_file + read_file (round-trip)"
TMP_FILE="/tmp/nos_test_$$.txt"
resp=$(call_tool "write_file" "{\"path\":\"${TMP_FILE}\",\"content\":\"AgentOS test OK\"}")
if has_field "$resp" "bytes_written"; then
    resp2=$(call_tool "read_file" "{\"path\":\"${TMP_FILE}\"}")
    if has_field "$resp2" "AgentOS test OK"; then
        pass "write_file + read_file → round-trip correcto"
    else
        fail "read_file no devolvió el contenido escrito"
    fi
    rm -f "$TMP_FILE"
else
    fail "write_file → respuesta inesperada: $resp"
fi

echo ""

# ── TEST 5: exec ───────────────────────────────────────────────────────────────
info "Test 5: exec (echo hello)"
resp=$(call_tool "exec" '{"command":"echo hello AgentOS"}')
if has_field "$resp" "stdout" && has_field "$resp" "hello AgentOS"; then
    pass "exec → stdout contiene 'hello AgentOS'"
else
    fail "exec → respuesta inesperada: $resp"
fi

echo ""

# ── TEST 6: exec timeout ──────────────────────────────────────────────────────
info "Test 6: exec timeout (sleep 10 con timeout 1s)"
resp=$(call_tool "exec" '{"command":"sleep 10","timeout_ms":1000}')
if has_field "$resp" "timed_out"; then
    timed=$(echo "$resp" | python3 -c "
import sys,json
d=json.loads(sys.stdin.read())
c=json.loads(d['result']['content'][0]['text'])
print(c.get('timed_out','false'))
" 2>/dev/null)
    if [[ "$timed" == "True" ]] || [[ "$timed" == "true" ]]; then
        pass "exec timeout → el comando fue interrumpido correctamente"
    else
        fail "exec timeout → timed_out=$timed (esperado: true)"
    fi
else
    fail "exec timeout → respuesta inesperada: $resp"
fi

echo ""

# ── TEST 7: process_list ──────────────────────────────────────────────────────
info "Test 7: process_list"
resp=$(call_tool "process_list" '{"limit":5}')
if has_field "$resp" "processes"; then
    pass "process_list → respuesta contiene 'processes'"
    echo "$resp" | python3 -c "
import sys, json
d = json.loads(sys.stdin.read())
c = json.loads(d['result']['content'][0]['text'])
print(f'   {c[\"count\"]} procesos devueltos (top por RSS):')
for p in c['processes'][:3]:
    print(f'   PID {p[\"pid\"]:6d}  {p[\"name\"]:20s}  {p[\"rss_mb\"]}MB')
" 2>/dev/null || true
else
    fail "process_list → respuesta inesperada: $resp"
fi

echo ""

# ── TEST 8: network_info ──────────────────────────────────────────────────────
info "Test 8: network_info"
resp=$(call_tool "network_info" "{}")
if has_field "$resp" "interfaces"; then
    pass "network_info → respuesta contiene 'interfaces'"
    echo "$resp" | python3 -c "
import sys, json
d = json.loads(sys.stdin.read())
c = json.loads(d['result']['content'][0]['text'])
for iface in c['interfaces']:
    if iface['state'] == 'up':
        print(f'   {iface[\"interface\"]:12s}  {iface[\"ipv4\"]:20s}  {iface[\"state\"]}')
" 2>/dev/null || true
else
    fail "network_info → respuesta inesperada: $resp"
fi

echo ""

# ── TEST 9: error handling ────────────────────────────────────────────────────
info "Test 9: error handling (archivo inexistente)"
resp=$(call_tool "read_file" '{"path":"/archivo/que/no/existe"}')
if has_field "$resp" "error" || has_field "$resp" "not found" || has_field "$resp" "File not found"; then
    pass "read_file → error manejado correctamente para archivo inexistente"
else
    fail "read_file → debería retornar error: $resp"
fi

echo ""

# ── resumen ───────────────────────────────────────────────────────────────────
echo "════════════════════════════════════════"
if [[ $FAILURES -eq 0 ]]; then
    echo -e "${GREEN}Todos los tests pasaron ✓${NC}"
else
    echo -e "${RED}${FAILURES} test(s) fallaron ✗${NC}"
fi
echo "════════════════════════════════════════"
echo ""

exit $FAILURES

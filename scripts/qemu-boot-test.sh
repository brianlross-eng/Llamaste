#!/bin/bash
# Quick QEMU boot test for Llamaste
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/root/llamaste-build/output/images/llamaste.img
KERNEL=/root/llamaste-build/output/images/bzImage

if [ ! -f "$IMG" ]; then
    echo "ERROR: $IMG not found"
    exit 1
fi

# Start QEMU in background with port forwarding
qemu-system-x86_64 \
  -m 512M \
  -nographic \
  -no-reboot \
  -drive file="$IMG",format=raw,if=virtio \
  -append 'console=ttyS0 root=/dev/vda3 rootfstype=squashfs ro init=/opt/llamaste/llamaste ip=dhcp' \
  -kernel "$KERNEL" \
  -net nic,model=e1000 -net user,hostfwd=tcp::8080-:80 \
  </dev/null >/tmp/qemu-boot.log 2>&1 &

QEMU_PID=$!
echo "QEMU started (PID $QEMU_PID)"

# Wait for HTTP server to be ready
echo "Waiting for HTTP server..."
READY=0
for i in $(seq 1 30); do
    CODE=$(curl -s -o /dev/null -w '%{http_code}' http://localhost:8080/health 2>/dev/null || echo "000")
    if [ "$CODE" = "200" ]; then
        echo "Server ready after ${i}s"
        READY=1
        break
    fi
    sleep 1
done

if [ "$READY" = "0" ]; then
    echo "TIMEOUT: Server did not start in 30s"
    echo "--- QEMU log ---"
    tail -30 /tmp/qemu-boot.log
    kill $QEMU_PID 2>/dev/null
    exit 1
fi

PASS=0
FAIL=0

# Test 1: Health endpoint
echo ""
echo "=== Test 1: GET /health ==="
HEALTH=$(curl -s http://localhost:8080/health)
echo "$HEALTH"
if echo "$HEALTH" | grep -q "ok"; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
fi

# Test 2: System info
echo ""
echo "=== Test 2: GET /llamaste/system ==="
SYS=$(curl -s http://localhost:8080/llamaste/system)
echo "$SYS" | python3 -m json.tool 2>/dev/null | head -15
if echo "$SYS" | grep -q "cpu"; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
fi

# Test 3: Tools list
echo ""
echo "=== Test 3: GET /llamaste/tools ==="
TOOLS=$(curl -s http://localhost:8080/llamaste/tools)
echo "$TOOLS" | python3 -m json.tool 2>/dev/null | head -10
if echo "$TOOLS" | grep -q "function"; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
fi

# Test 4: Web UI
echo ""
echo "=== Test 4: GET / (Web UI) ==="
UI=$(curl -s http://localhost:8080/)
echo "$UI" | head -3
if echo "$UI" | grep -q -i "llamaste\|html"; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
fi

# Test 5: OpenAI-compatible API
echo ""
echo "=== Test 5: POST /v1/chat/completions ==="
API=$(curl -s -X POST http://localhost:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"llamaste","messages":[{"role":"user","content":"hello"}]}')
echo "$API" | python3 -m json.tool 2>/dev/null | head -15
if echo "$API" | grep -q "choices"; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    FAIL=$((FAIL + 1))
fi

# Cleanup
kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null || true

echo ""
echo "========================================"
echo "  Results: $PASS passed, $FAIL failed"
echo "========================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi

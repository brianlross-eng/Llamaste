#!/bin/bash
# build-update.sh — Build a signed Llamaste .update package
#
# .update format (binary):
#   [4 bytes]  magic: "LMUP"
#   [4 bytes]  format version: uint32 LE (= 1)
#   [4 bytes]  manifest size: uint32 LE
#   [N bytes]  manifest.json
#   [64 bytes] Ed25519 signature of manifest.json
#   [rest]     squashfs payload
#
# Usage:
#   ./scripts/build-update.sh <version> <squashfs> <secret-key> [output]
#
# Example:
#   ./scripts/build-update.sh 1.1.0 output/images/rootfs.squashfs signing-key.secret
#   # → llamaste-1.1.0.update
#
set -euo pipefail

VERSION="${1:?Usage: $0 <version> <squashfs> <secret-key> [output]}"
SQUASHFS="${2:?Usage: $0 <version> <squashfs> <secret-key> [output]}"
SECRET_KEY="${3:?Usage: $0 <version> <squashfs> <secret-key> [output]}"
OUTPUT="${4:-llamaste-${VERSION}.update}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SIGN_HELPER="${PROJECT_DIR}/tools/sign-helper"

# Validate inputs
[ -f "$SQUASHFS" ] || { echo "error: squashfs not found: $SQUASHFS"; exit 1; }
[ -f "$SECRET_KEY" ] || { echo "error: secret key not found: $SECRET_KEY"; exit 1; }

# Build sign-helper if needed
if [ ! -x "$SIGN_HELPER" ]; then
    echo "[build-update] Compiling sign-helper..."
    make -C "${PROJECT_DIR}/tools" sign-helper
fi
[ -x "$SIGN_HELPER" ] || { echo "error: sign-helper not found at $SIGN_HELPER"; exit 1; }

# Compute SHA-256 of squashfs
echo "[build-update] Computing SHA-256 of squashfs..."
PAYLOAD_SHA256=$(sha256sum "$SQUASHFS" | cut -d' ' -f1)
PAYLOAD_SIZE=$(stat -c%s "$SQUASHFS" 2>/dev/null || stat -f%z "$SQUASHFS")

echo "[build-update] Payload: $PAYLOAD_SIZE bytes, SHA-256: $PAYLOAD_SHA256"

# Build manifest.json
MANIFEST=$(cat <<MANIFEST_EOF
{
  "format_version": 1,
  "version": "${VERSION}",
  "build_date": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "arch": "x86-64",
  "min_version": "1.0.0",
  "components": {
    "system": {
      "file": "system.squashfs",
      "sha256": "${PAYLOAD_SHA256}",
      "size_compressed": ${PAYLOAD_SIZE},
      "size_uncompressed": ${PAYLOAD_SIZE}
    }
  },
  "changelog": []
}
MANIFEST_EOF
)

# Write manifest to temp file for signing
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT
echo -n "$MANIFEST" > "$TMPDIR/manifest.json"
MANIFEST_SIZE=$(stat -c%s "$TMPDIR/manifest.json" 2>/dev/null || stat -f%z "$TMPDIR/manifest.json")

echo "[build-update] Manifest: $MANIFEST_SIZE bytes"

# Sign manifest
echo "[build-update] Signing manifest..."
SIG_HEX=$("$SIGN_HELPER" "$SECRET_KEY" "$TMPDIR/manifest.json")
echo "[build-update] Signature: ${SIG_HEX:0:16}..."

# Convert hex signature to binary
echo -n "$SIG_HEX" | xxd -r -p > "$TMPDIR/signature.bin"
SIG_SIZE=$(stat -c%s "$TMPDIR/signature.bin" 2>/dev/null || stat -f%z "$TMPDIR/signature.bin")
[ "$SIG_SIZE" -eq 64 ] || { echo "error: signature is $SIG_SIZE bytes, expected 64"; exit 1; }

# Build .update file
echo "[build-update] Assembling $OUTPUT..."

# Write header: magic (4) + format_version (4) + manifest_size (4)
printf 'LMUP' > "$OUTPUT"
# Format version = 1 (uint32 LE)
printf '\x01\x00\x00\x00' >> "$OUTPUT"
# Manifest size (uint32 LE)
python3 -c "import struct; import sys; sys.stdout.buffer.write(struct.pack('<I', $MANIFEST_SIZE))" >> "$OUTPUT"

# Write manifest
cat "$TMPDIR/manifest.json" >> "$OUTPUT"
# Write signature
cat "$TMPDIR/signature.bin" >> "$OUTPUT"
# Write squashfs payload
cat "$SQUASHFS" >> "$OUTPUT"

TOTAL_SIZE=$(stat -c%s "$OUTPUT" 2>/dev/null || stat -f%z "$OUTPUT")
echo ""
echo "[build-update] SUCCESS: $OUTPUT"
echo "  Version:      $VERSION"
echo "  Payload SHA:  $PAYLOAD_SHA256"
echo "  Payload size: $PAYLOAD_SIZE bytes"
echo "  Total size:   $TOTAL_SIZE bytes"
echo ""
echo "  For latest.json:"
echo "    \"version\": \"$VERSION\","
echo "    \"size\": $TOTAL_SIZE,"
echo "    \"sha256\": \"$PAYLOAD_SHA256\""

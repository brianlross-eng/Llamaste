#!/bin/bash
# release.sh — Full Llamaste release workflow
#
# Builds a signed .update package, creates a GitHub release,
# uploads the asset, and updates latest.json.
#
# Prerequisites:
#   - gh CLI authenticated (gh auth login)
#   - signing-key.secret in project root (from tools/keygen)
#   - Built squashfs image
#
# Usage:
#   ./scripts/release.sh <version> <squashfs> [--notes "Release notes"]
#
# Example:
#   ./scripts/release.sh 1.1.0 /root/llamaste-build/output/images/rootfs.squashfs
#
set -euo pipefail

VERSION="${1:?Usage: $0 <version> <squashfs> [--notes \"...\"]}"
SQUASHFS="${2:?Usage: $0 <version> <squashfs> [--notes \"...\"]}"
NOTES=""

shift 2
while [[ $# -gt 0 ]]; do
    case "$1" in
        --notes) NOTES="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SECRET_KEY="${PROJECT_DIR}/signing-key.secret"
UPDATE_FILE="${PROJECT_DIR}/llamaste-${VERSION}.update"

[ -f "$SECRET_KEY" ] || { echo "error: signing-key.secret not found in project root. Run: cd tools && make keygen && ./keygen"; exit 1; }
[ -f "$SQUASHFS" ] || { echo "error: squashfs not found: $SQUASHFS"; exit 1; }
command -v gh >/dev/null || { echo "error: gh CLI not found. Install: https://cli.github.com"; exit 1; }

# Detect GitHub repo from git remote
REPO=$(gh repo view --json nameWithOwner -q '.nameWithOwner' 2>/dev/null || echo "")
[ -n "$REPO" ] || { echo "error: cannot detect GitHub repo. Run 'gh repo view' to check."; exit 1; }

echo "=== Llamaste Release v${VERSION} ==="
echo "Repo:      $REPO"
echo "Squashfs:  $SQUASHFS"
echo ""

# Step 1: Build signed .update package
echo "--- Step 1: Build .update package ---"
"$SCRIPT_DIR/build-update.sh" "$VERSION" "$SQUASHFS" "$SECRET_KEY" "$UPDATE_FILE"

# Step 2: Create GitHub release
echo ""
echo "--- Step 2: Create GitHub release ---"
TAG="v${VERSION}"

if [ -n "$NOTES" ]; then
    gh release create "$TAG" \
        --title "Llamaste $TAG" \
        --notes "$NOTES"
else
    gh release create "$TAG" \
        --title "Llamaste $TAG" \
        --generate-notes
fi

# Step 3: Upload .update asset
echo ""
echo "--- Step 3: Upload .update asset ---"
gh release upload "$TAG" "$UPDATE_FILE" --clobber

# Get the download URL
DOWNLOAD_URL="https://github.com/${REPO}/releases/download/${TAG}/llamaste-${VERSION}.update"
TOTAL_SIZE=$(stat -c%s "$UPDATE_FILE" 2>/dev/null || stat -f%z "$UPDATE_FILE")
PAYLOAD_SHA256=$(sha256sum "$SQUASHFS" | cut -d' ' -f1)

# Step 4: Update latest.json
echo ""
echo "--- Step 4: Update latest.json ---"
cat > "${PROJECT_DIR}/latest.json" <<EOF
{
  "version": "${VERSION}",
  "url": "${DOWNLOAD_URL}",
  "size": ${TOTAL_SIZE},
  "sha256": "${PAYLOAD_SHA256}"
}
EOF

cd "$PROJECT_DIR"
git add latest.json
git commit -m "release: v${VERSION} — update latest.json"
git push

echo ""
echo "=== Release v${VERSION} complete ==="
echo "  GitHub:     https://github.com/${REPO}/releases/tag/${TAG}"
echo "  Download:   ${DOWNLOAD_URL}"
echo "  latest.json pushed to main"
echo ""
echo "Devices running update.check will see this update on next poll."

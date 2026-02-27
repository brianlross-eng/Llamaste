#!/bin/bash
set -eu

# Clean PATH to avoid Buildroot issues with spaces from Windows PATH
export PATH="/usr/lib/ccache:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

echo "=== Buildroot Build ==="
echo "PATH: $PATH"
echo "Working dir: $(pwd)"
echo "nproc: $(nproc)"
echo ""

cd /root/llamaste-build/output

# Run the build
make -j$(nproc) 2>&1

echo ""
echo "=== Build complete ==="
echo ""
ls -lh /root/llamaste-build/output/images/ 2>/dev/null || echo "No images directory"

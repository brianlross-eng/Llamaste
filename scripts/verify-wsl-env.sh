#!/bin/bash
# Verification script for WSL2 dev environment (Task 0)
set -e

echo "=== Llamaste WSL2 Dev Environment Verification ==="
echo ""

echo "--- 1. Build Toolchain ---"
gcc --version | head -1
g++ --version | head -1
cmake --version | head -1
echo "ninja: $(ninja --version)"
make --version | head -1
echo ""

echo "--- 2. Buildroot Dependencies ---"
for tool in cpio bc flex bison rsync unzip wget curl git python3; do
    loc=$(which $tool 2>/dev/null || echo "NOT FOUND")
    printf "  %-10s: %s\n" "$tool" "$loc"
done
echo ""

echo "--- 3. Kernel Build Libraries ---"
for lib in libncurses-dev libssl-dev libelf-dev; do
    ver=$(dpkg -l "$lib" 2>/dev/null | grep '^ii' | awk '{print $3}')
    printf "  %-20s: %s\n" "$lib" "${ver:-NOT FOUND}"
done
echo ""

echo "--- 4. QEMU & OVMF ---"
qemu-system-x86_64 --version | head -1
echo "  OVMF: $(ls /usr/share/OVMF/OVMF_CODE_4M.fd 2>/dev/null || echo 'NOT FOUND')"
echo ""

echo "--- 5. Image Tools ---"
for tool in genimage mksquashfs mkdosfs mkfs.ext4 mtools; do
    loc=$(which $tool 2>/dev/null || echo "NOT FOUND")
    printf "  %-15s: %s\n" "$tool" "$loc"
done
echo ""

echo "--- 6. musl-tools ---"
musl-gcc --version 2>&1 | head -1
echo ""

echo "--- 7. ccache ---"
ccache --version | head -1
ccache -s 2>/dev/null | grep "Cache size" || echo "  (no cache stats yet)"
export PATH="/usr/lib/ccache:$PATH"
echo "  gcc via ccache: $(which gcc)"
echo "  g++ via ccache: $(which g++)"
echo ""

echo "--- 8. Workspace Layout ---"
echo "  Build dir: /root/llamaste-build"
ls -la /root/llamaste-build/
echo ""
echo "  Source symlink target:"
ls /root/llamaste-build/src/ | head -5
echo "  ..."
echo ""

echo "--- 9. Filesystem Types ---"
echo "  Build dir (should be ext4):"
df -T /root/llamaste-build | tail -1
echo "  Source dir (Windows NTFS via 9p):"
df -T /mnt/d/Llamaste 2>/dev/null | tail -1 || echo "  /mnt/d not mounted"
echo ""

echo "--- 10. Quick Compile Test ---"
TMPDIR=$(mktemp -d)
cat > "$TMPDIR/test.cpp" << 'CPPEOF'
#include <cstdio>
int main() {
    printf("Llamaste build env OK!\n");
    return 0;
}
CPPEOF
g++ -o "$TMPDIR/test" "$TMPDIR/test.cpp" && "$TMPDIR/test"
echo "  musl static compile test:"
musl-gcc -static -o "$TMPDIR/test_musl" "$TMPDIR/test.cpp" -lstdc++ 2>/dev/null && "$TMPDIR/test_musl" || echo "  musl-gcc C++ static: needs musl C++ setup (expected - Buildroot handles this)"
rm -rf "$TMPDIR"
echo ""

echo "=== All checks complete ==="

#!/bin/bash
set -eu

# Clean PATH to avoid Buildroot issues with spaces from Windows PATH
export PATH="/usr/lib/ccache:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

# Host-toolchain compatibility for newer distros (e.g. Ubuntu 26.04 ships GCC 15 +
# CMake 4), which Buildroot 2024.02 predates:
#   - CMAKE_POLICY_VERSION_MINIMUM=3.5: CMake 4 dropped compat with cmake_minimum_required
#     < 3.5; several host packages (host-hiredis, ...) still declare old minimums.
#   - HOSTCC/HOSTCXX=gcc-13: GCC 15 defaults to C23 and turns old-C constructs into hard
#     errors, breaking bundled gnulib in host-m4/flex/bison/autotools. GCC 13 (-std=gnu17)
#     builds them. Install with: sudo apt-get install -y gcc-13 g++-13
export CMAKE_POLICY_VERSION_MINIMUM=3.5
export HOSTCC=gcc-13
export HOSTCXX=g++-13

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

# Research 29: Building ONNX Runtime from Source with musl libc (Buildroot)

**Date**: 2026-03-06
**Purpose**: Cross-compile onnxruntime for Llamaste's musl-based Buildroot image (sherpa-onnx + Piper TTS)

---

## 1. Latest Stable Version

**ONNX Runtime v1.24.3** — released March 5, 2026

Recent releases:
| Version | Date | Notes |
|---------|------|-------|
| 1.24.3 | 2026-03-05 | Bug fixes, security, EP updates |
| 1.24.2 | 2026-02-19 | Bug fixes, security |
| 1.24.1 | 2026-02-06 | Python 3.14 support, dropped Python 3.10 |
| 1.24.0 | 2026-01 | Major release |
| 1.23.2 | 2024-10-25 | Maintenance |
| 1.22.0 | 2025-05-10 | Last pre-1.23 |

Source: https://github.com/microsoft/onnxruntime/releases

**Alpine Linux packages v1.24.2** (edge/community, all arches except s390x/x86).

---

## 2. Key Dependencies (from v1.24.2 cmake/deps.txt)

| Dependency | Version |
|-----------|---------|
| protobuf | v21.12 |
| abseil-cpp | 20250814.0 |
| flatbuffers | v23.5.26 |
| re2 | 2024-07-02 |
| ONNX | v1.20.1 |
| Eigen | 1d8b82b0 (commit) |
| GoogleTest | v1.17.0 |
| nlohmann/json | v3.11.3 |
| pybind11 | v2.13.6 |
| XNNPACK | 3cf85e70 |

**CRITICAL for cross-compile**: protobuf v21.12 — host `protoc` must match this version exactly.

---

## 3. musl Patches Required

### 3.1 execinfo.h Fix (THE main musl issue)

**PR #25726** (https://github.com/microsoft/onnxruntime/pull/25726) — **NOT YET MERGED** as of 2026-03-06.

File: `onnxruntime/core/platform/posix/stacktrace.cc`

```diff
--- a/onnxruntime/core/platform/posix/stacktrace.cc
+++ b/onnxruntime/core/platform/posix/stacktrace.cc
@@ -3,7 +3,7 @@
 #include "core/common/common.h"

-#if !defined(__ANDROID__) && !defined(__wasm__) && !defined(_OPSCHEMA_LIB_) && !defined(_AIX)
+#if defined(__GLIBC__) && !defined(__ANDROID__) && !defined(__wasm__) && !defined(_OPSCHEMA_LIB_) && !defined(_AIX)
 #include <execinfo.h>
 #endif
 #include <vector>
@@ -13,7 +13,7 @@ namespace onnxruntime {
 std::vector<std::string> GetStackTrace() {
   std::vector<std::string> stack;

-#if !defined(NDEBUG) && !defined(__ANDROID__) && !defined(__wasm__) && !defined(_OPSCHEMA_LIB_)
+#if defined(__GLIBC__) && !defined(NDEBUG) && !defined(__ANDROID__) && !defined(__wasm__) && !defined(_OPSCHEMA_LIB_)
   constexpr int kCallstackLimit = 64;
```

**Since PR #25726 is NOT merged**, we MUST carry this patch ourselves.

### 3.2 Flatbuffers Locale Fix (from RapidAI musl patch)

File: `_deps/flatbuffers-src/include/flatbuffers/base.h` (line ~273)

```diff
-  (defined(_XOPEN_VERSION) && (_XOPEN_VERSION >= 700)) && \
+  (defined(__GLIBC__) && defined(_XOPEN_VERSION) && (_XOPEN_VERSION >= 700)) && \
```

This guards `uselocale()` behind `__GLIBC__` since musl's locale support differs.

**Note**: If we use Buildroot's system flatbuffers instead of the vendored one, this patch may not be needed.

### 3.3 Alpine Linux Patches (7 total for v1.24.2)

From https://github.com/alpinelinux/aports/tree/master/community/onnxruntime:

1. **no-execinfo.patch** — Same `__GLIBC__` guard on `execinfo.h` (equivalent to PR #25726)
2. **system.patch** — Replaces FetchContent with `find_package()` for system abseil, re2, protobuf, nlohmann_json, gtest. This is Alpine-specific (uses system packages).
3. **0001-Remove-MATH_NO_EXCEPT-macro.patch** — Removes a math exception handling macro
4. **flatbuffers-locale.patch.noauto** — The locale/uselocale fix for flatbuffers on musl
5. **gcc-15.patch** — GCC 15 compatibility
6. **upb-fix.patch** — Protocol buffer (upb) compatibility fix
7. **26187_disable-hascpudevice-test.patch.noauto** — Test fix

**For Buildroot, we need at minimum**: no-execinfo.patch + flatbuffers-locale.patch (if using vendored flatbuffers).

---

## 4. CMake Invocation for Minimal CPU-Only Build

### 4.1 Via build.sh (recommended wrapper)

```bash
./build.sh \
  --config MinSizeRel \
  --build_shared_lib \
  --minimal_build \
  --disable_ml_ops \
  --disable_exceptions \
  --disable_rtti \
  --skip_tests \
  --parallel \
  --compile_no_warning_as_error \
  --cmake_extra_defines CMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
  --cmake_extra_defines ONNX_CUSTOM_PROTOC_EXECUTABLE=/path/to/host/protoc \
  --cmake_extra_defines onnxruntime_CROSS_COMPILING=ON
```

### 4.2 Via Raw CMake (bypassing build.py)

```bash
cmake ../cmake \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DONNX_CUSTOM_PROTOC_EXECUTABLE=/path/to/host/protoc \
  \
  -Donnxruntime_BUILD_SHARED_LIB=ON \
  -Donnxruntime_BUILD_UNIT_TESTS=OFF \
  -Donnxruntime_CROSS_COMPILING=ON \
  \
  -Donnxruntime_MINIMAL_BUILD=ON \
  -Donnxruntime_DISABLE_ML_OPS=ON \
  -Donnxruntime_DISABLE_RTTI=ON \
  -Donnxruntime_DISABLE_EXCEPTIONS=ON \
  -Donnxruntime_DISABLE_CONTRIB_OPS=ON \
  -Donnxruntime_DISABLE_SPARSE_TENSORS=ON \
  -Donnxruntime_DISABLE_OPTIONAL_TYPE=ON \
  -Donnxruntime_DISABLE_FLOAT8_TYPES=ON \
  \
  -Donnxruntime_ENABLE_PYTHON=OFF \
  -Donnxruntime_ENABLE_TRAINING=OFF \
  -Donnxruntime_ENABLE_TRAINING_OPS=OFF \
  -Donnxruntime_ENABLE_TRAINING_APIS=OFF \
  -Donnxruntime_ENABLE_LTO=ON \
  \
  -Donnxruntime_USE_CUDA=OFF \
  -Donnxruntime_USE_TENSORRT=OFF \
  -Donnxruntime_USE_DML=OFF \
  -Donnxruntime_USE_OPENVINO=OFF \
  -Donnxruntime_USE_MIGRAPHX=OFF \
  -Donnxruntime_USE_NNAPI_BUILTIN=OFF \
  -Donnxruntime_USE_COREML=OFF \
  -Donnxruntime_USE_XNNPACK=OFF \
  -Donnxruntime_USE_WEBNN=OFF \
  -Donnxruntime_USE_QNN=OFF \
  -Donnxruntime_USE_VITISAI=OFF \
  -Donnxruntime_USE_ACL=OFF \
  -Donnxruntime_USE_ARMNN=OFF \
  -Donnxruntime_USE_DNNL=OFF \
  -Donnxruntime_USE_JSEP=OFF \
  -Donnxruntime_USE_WEBGPU=OFF \
  -Donnxruntime_USE_CANN=OFF \
  -Donnxruntime_USE_NV=OFF \
  \
  -Donnxruntime_BUILD_CSHARP=OFF \
  -Donnxruntime_BUILD_JAVA=OFF \
  -Donnxruntime_BUILD_NODEJS=OFF \
  -Donnxruntime_BUILD_OBJC=OFF \
  -Donnxruntime_BUILD_APPLE_FRAMEWORK=OFF \
  -Donnxruntime_BUILD_BENCHMARKS=OFF \
  -Donnxruntime_BUILD_MS_EXPERIMENTAL_OPS=OFF \
  \
  -Donnxruntime_USE_VCPKG=OFF \
  -Donnxruntime_USE_MIMALLOC=OFF \
  -Donnxruntime_ENABLE_MICROSOFT_INTERNAL=OFF \
  -Donnxruntime_ENABLE_EXTERNAL_CUSTOM_OP_SCHEMAS=OFF \
  -DFETCHCONTENT_QUIET=OFF
```

### 4.3 Complete GPU-Disable Variables Reference

All OFF by default, but explicit is safer:
```
onnxruntime_USE_CUDA=OFF
onnxruntime_USE_TENSORRT=OFF
onnxruntime_USE_DML=OFF            # DirectML (Windows)
onnxruntime_USE_NV=OFF             # TensorRT/RTX
onnxruntime_USE_OPENVINO=OFF
onnxruntime_USE_MIGRAPHX=OFF       # AMD ROCm
onnxruntime_USE_COREML=OFF         # Apple
onnxruntime_USE_NNAPI_BUILTIN=OFF  # Android
onnxruntime_USE_XNNPACK=OFF
onnxruntime_USE_WEBNN=OFF
onnxruntime_USE_QNN=OFF            # Qualcomm
onnxruntime_USE_VITISAI=OFF        # Xilinx
onnxruntime_USE_AZURE=OFF
onnxruntime_USE_ACL=OFF            # ARM Compute Library
onnxruntime_USE_ARMNN=OFF
onnxruntime_USE_DNNL=OFF           # Intel oneDNN
onnxruntime_USE_JSEP=OFF           # JavaScript EP
onnxruntime_USE_WEBGPU=OFF
onnxruntime_USE_CANN=OFF           # Huawei Ascend
onnxruntime_CUDA_MINIMAL=OFF
```

---

## 5. build.py/build.sh vs Raw CMake

### What build.py does
- Sets ~80+ CMake definitions based on command-line flags
- Handles protoc detection and host-vs-target separation
- Sets architecture-specific MLAS flags (CRITICAL)
- Manages `-fcf-protection` flag for x86 (must not be used for cross-compiling to ARM)
- Invokes flake8 Python linting (can be skipped)
- Creates build directory structure
- Runs cmake configure + build + test in sequence

### Can you bypass it?
**Yes, but with caveats:**

1. **MLAS architecture selection**: `cmake/onnxruntime_mlas.cmake` uses `CMAKE_SYSTEM_PROCESSOR` to select architecture-specific assembly/intrinsics. Your toolchain file MUST correctly set this variable or you get undefined reference errors to symbols like `MlasGemmFloatKernelAvx512F`.

2. **Recommended approach**: Run `build.py` once on a similar machine with `--dry-run` or capture the logged CMake invocation, then replicate those args in your Buildroot .mk file.

3. **Protoc requirement**: Must pass `-DONNX_CUSTOM_PROTOC_EXECUTABLE=<host-protoc>` when cross-compiling. The protoc version must match the bundled protobuf (v21.12 for onnxruntime 1.24.x).

### For Buildroot specifically:
Raw CMake is the way to go. Buildroot packages use direct CMake invocation via `$(CMAKE_TARGET_CONFIGURE)`. The `build.sh` wrapper needs Python and does things Buildroot handles natively (toolchain, sysroot, install prefix).

---

## 6. RapidAI Musl Build Reference

### musl-cross.toolchain.cmake (from RapidAI/OnnxruntimeBuilder)

```cmake
set(CMAKE_SYSTEM_NAME Linux)
# CMAKE_SYSTEM_PROCESSOR is derived from TOOLCHAIN_NAME (e.g., aarch64-linux-musl → aarch64)
set(CMAKE_C_COMPILER   $ENV{TOOLCHAIN_NAME}-gcc)
set(CMAKE_CXX_COMPILER $ENV{TOOLCHAIN_NAME}-g++)
set(CMAKE_FIND_ROOT_PATH $ENV{TOOLCHAIN_PATH})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

### onnxruntime_cmake_options.txt (81 variables)

Key options from their file (all GPU/training/wasm OFF):
```
-Donnxruntime_BUILD_SHARED_LIB=ON
-Donnxruntime_BUILD_UNIT_TESTS=OFF
-Donnxruntime_CROSS_COMPILING=ON
-Donnxruntime_DISABLE_ML_OPS=OFF        # they keep ML ops ON
-Donnxruntime_DISABLE_RTTI=OFF          # they keep RTTI ON
-Donnxruntime_DISABLE_EXCEPTIONS=OFF    # they keep exceptions ON
-Donnxruntime_MINIMAL_BUILD=OFF         # they do full build
-Donnxruntime_ENABLE_PYTHON=OFF
-Donnxruntime_ENABLE_TRAINING=OFF
-Donnxruntime_USE_CUDA=OFF  (implicit - all GPU flags OFF)
```

### build-onnxruntime-musl.sh flow:
1. Set `PATH` to include musl toolchain bin
2. `cmake ../cmake $(cat ../onnxruntime_cmake_options.txt) -DCMAKE_TOOLCHAIN_FILE=../musl-cross.toolchain.cmake`
3. Apply musl patch: `patch -p0 -i ../patches/onnxruntime-1.18.0-musl.patch`
4. `cmake --build . --config Release -j $(nproc)`
5. `cmake --build . --config Release --target install`
6. Collect shared + static libs (combines all .a files into single libonnxruntime.a)

### RapidAI musl patch (for v1.18.0, but same principle)
Patches flatbuffers `base.h` to guard `uselocale()` behind `defined(__GLIBC__)`.

---

## 7. Build Time Estimates

| Scenario | Time |
|----------|------|
| Fast x86_64 (8+ cores, parallel) | 5-17 min |
| Cloud VM (8GB+ RAM) | 20-40 min |
| Single-threaded / low-memory | 1-2+ hours |
| ARM32 cross-compilation | Several hours |
| Minimal build (reduced ops) | ~30-50% less than full |
| QEMU-emulated cross-compile | Hours |

**Buildroot on our WSL2 setup (4+ cores)**: Expect **15-30 minutes** for CPU-only minimal build with native cross-compiler.

**Memory**: Needs >2GB free. OOM killer will hit if parallel jobs too high on low-RAM systems. Buildroot sets `--parallel` by default.

---

## 8. Known Issues & Gotchas

### 8.1 onnx_minimal.cmake removed (v1.22.1/v1.22.2 only)
- Issue: https://github.com/microsoft/onnxruntime/issues/25796
- `onnxruntime_external_deps.cmake` references `onnx_minimal.cmake` which was removed
- **Fix**: Copy from v1.22.0 or use v1.24.x (fixed)
- Does NOT affect v1.24.x

### 8.2 CMAKE_SYSTEM_PROCESSOR must be set correctly
- MLAS selects architecture-specific kernels based on this
- Buildroot toolchain cmake files set this automatically
- Values: `x86_64`, `aarch64`, `armv7l`, `riscv64`, etc.

### 8.3 Protoc host/target version mismatch
- Host protoc must match bundled protobuf version (v21.12 for 1.24.x)
- Buildroot builds host-protobuf automatically — use `$(HOST_DIR)/bin/protoc`
- Known issue: ONNX CMake may pull host sysroot libs into target link

### 8.4 -fcf-protection flag
- build.py adds `-fcf-protection` on Linux x86_64 builds
- This flag is invalid for ARM cross-compilers → build failure
- Raw CMake avoids this entirely (build.py-specific issue)

### 8.5 cpuinfo
- `onnxruntime_ENABLE_CPUINFO=ON` by default
- May need `=OFF` if target architecture not supported by cpuinfo
- Works fine for x86_64 and aarch64

### 8.6 OpenMP
- Since v1.7.0, official CPU packages are built WITHOUT OpenMP
- `onnxruntime_USE_OPENMP=OFF` is the default and recommended

---

## 9. Buildroot .mk File Strategy

### Package structure:
```
package/onnxruntime/
  onnxruntime.mk
  onnxruntime.hash
  Config.in
  0001-musl-no-execinfo.patch       # Guards execinfo.h with __GLIBC__
  0002-musl-flatbuffers-locale.patch # Guards uselocale with __GLIBC__
```

### Key Buildroot variables:
```makefile
ONNXRUNTIME_VERSION = 1.24.2
ONNXRUNTIME_SITE = https://github.com/microsoft/onnxruntime/archive/refs/tags/v$(ONNXRUNTIME_VERSION).tar.gz
ONNXRUNTIME_LICENSE = MIT
ONNXRUNTIME_LICENSE_FILES = LICENSE
ONNXRUNTIME_INSTALL_STAGING = YES
ONNXRUNTIME_SUPPORTS_IN_SOURCE_BUILD = NO

# CMake subdir is cmake/, not root
ONNXRUNTIME_SUBDIR = cmake

ONNXRUNTIME_CONF_OPTS = \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -Donnxruntime_BUILD_SHARED_LIB=ON \
  -Donnxruntime_BUILD_UNIT_TESTS=OFF \
  -Donnxruntime_CROSS_COMPILING=ON \
  -DONNX_CUSTOM_PROTOC_EXECUTABLE=$(HOST_DIR)/bin/protoc \
  -Donnxruntime_MINIMAL_BUILD=ON \
  -Donnxruntime_DISABLE_ML_OPS=ON \
  -Donnxruntime_DISABLE_RTTI=OFF \
  -Donnxruntime_DISABLE_EXCEPTIONS=OFF \
  -Donnxruntime_DISABLE_CONTRIB_OPS=ON \
  -Donnxruntime_DISABLE_SPARSE_TENSORS=ON \
  -Donnxruntime_ENABLE_PYTHON=OFF \
  -Donnxruntime_ENABLE_TRAINING=OFF \
  -Donnxruntime_ENABLE_LTO=ON \
  -Donnxruntime_USE_CUDA=OFF \
  -Donnxruntime_USE_TENSORRT=OFF \
  -Donnxruntime_USE_DML=OFF \
  -Donnxruntime_USE_OPENVINO=OFF \
  -Donnxruntime_USE_XNNPACK=OFF \
  -Donnxruntime_USE_OPENMP=OFF \
  -Donnxruntime_USE_VCPKG=OFF \
  -DFETCHCONTENT_QUIET=OFF

# Host protobuf dependency (for protoc)
ONNXRUNTIME_DEPENDENCIES = host-protobuf protobuf
```

### Critical decisions for Llamaste:
1. **DISABLE_EXCEPTIONS=OFF** — sherpa-onnx uses exceptions internally
2. **DISABLE_RTTI=OFF** — sherpa-onnx needs RTTI for dynamic_cast
3. **MINIMAL_BUILD=ON** — Smaller binary, ORT format models only
4. **DISABLE_ML_OPS=ON** — We only need NN ops, not traditional ML
5. **LTO=ON** — Smaller binary
6. **Protobuf**: Use Buildroot's host-protobuf (ensure version matches v21.12)

### Minimal build caveat:
With `MINIMAL_BUILD=ON`, models must be in ORT format (not ONNX). Convert with:
```bash
python -m onnxruntime.tools.convert_onnx_models_to_ort <model.onnx>
```
If sherpa-onnx expects .onnx files, use `MINIMAL_BUILD=OFF` or `extended`.

---

## 10. References

- Releases: https://github.com/microsoft/onnxruntime/releases
- Build docs: https://onnxruntime.ai/docs/build/inferencing.html
- Custom/minimal build: https://onnxruntime.ai/docs/build/custom.html
- CMakeLists.txt: https://github.com/microsoft/onnxruntime/blob/main/cmake/CMakeLists.txt
- musl PR #25726: https://github.com/microsoft/onnxruntime/pull/25726
- Alpine APKBUILD: https://github.com/alpinelinux/aports/tree/master/community/onnxruntime
- RapidAI builder: https://github.com/RapidAI/OnnxruntimeBuilder
- RapidAI musl script: https://github.com/RapidAI/OnnxruntimeBuilder/blob/main/build-onnxruntime-musl.sh
- Raw CMake discussion: https://github.com/microsoft/onnxruntime/discussions/8109
- Cross-compile protoc: https://onnxruntime.ai/docs/build/dependencies.html
- onnx_minimal.cmake issue: https://github.com/microsoft/onnxruntime/issues/25796
- Build time benchmarks: https://openbenchmarking.org/test/pts/onnx

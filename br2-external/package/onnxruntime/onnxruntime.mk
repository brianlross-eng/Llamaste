################################################################################
#
# onnxruntime — ONNX inference engine, built from source for musl
#
# Builds CPU-only shared library for linking by sherpa-onnx.
# Uses vendored deps (FetchContent) — no system abseil/re2/etc. needed.
#
################################################################################

ONNXRUNTIME_VERSION = v1.23.2
ONNXRUNTIME_SITE = $(call github,microsoft,onnxruntime,$(ONNXRUNTIME_VERSION))
ONNXRUNTIME_LICENSE = MIT
ONNXRUNTIME_LICENSE_FILES = LICENSE
ONNXRUNTIME_INSTALL_STAGING = YES
ONNXRUNTIME_INSTALL_TARGET = YES
ONNXRUNTIME_SUPPORTS_IN_SOURCE_BUILD = NO
ONNXRUNTIME_DEPENDENCIES = host-protobuf host-python3 host-flatbuffers

# Use the cmake/ subdirectory as the CMake source root
ONNXRUNTIME_SUBDIR = cmake

ONNXRUNTIME_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_CXX_FLAGS="$(ONNXRUNTIME_WNOERROR)" \
	-DBUILD_SHARED_LIBS=OFF \
	-Donnxruntime_BUILD_SHARED_LIB=ON \
	-Donnxruntime_BUILD_UNIT_TESTS=OFF \
	-Donnxruntime_BUILD_BENCHMARKS=OFF \
	-Donnxruntime_CROSS_COMPILING=ON \
	-Donnxruntime_DISABLE_RTTI=OFF \
	-Donnxruntime_DISABLE_EXCEPTIONS=OFF \
	-Donnxruntime_ENABLE_PYTHON=OFF \
	-Donnxruntime_ENABLE_TRAINING=OFF \
	-Donnxruntime_ENABLE_LTO=OFF \
	-Donnxruntime_USE_CUDA=OFF \
	-Donnxruntime_USE_TENSORRT=OFF \
	-Donnxruntime_USE_DML=OFF \
	-Donnxruntime_USE_OPENVINO=OFF \
	-Donnxruntime_USE_XNNPACK=OFF \
	-Donnxruntime_USE_OPENMP=OFF \
	-Donnxruntime_USE_VCPKG=OFF \
	-Donnxruntime_USE_MIGRAPHX=OFF \
	-Donnxruntime_USE_COREML=OFF \
	-Donnxruntime_USE_NNAPI_BUILTIN=OFF \
	-Donnxruntime_USE_WEBNN=OFF \
	-Donnxruntime_USE_QNN=OFF \
	-Donnxruntime_USE_VITISAI=OFF \
	-Donnxruntime_USE_AZURE=OFF \
	-Donnxruntime_USE_ACL=OFF \
	-Donnxruntime_USE_ARMNN=OFF \
	-Donnxruntime_USE_DNNL=OFF \
	-Donnxruntime_USE_CANN=OFF \
	-Donnxruntime_USE_JSEP=OFF \
	-Donnxruntime_USE_WEBGPU=OFF \
	-DONNX_CUSTOM_PROTOC_EXECUTABLE=$(ONNXRUNTIME_PROTOC) \
	-DFETCHCONTENT_QUIET=OFF

# onnxruntime v1.23.2 fetches and links protobuf v21.12 (libprotoc 3.21.12). It uses
# ONNX_CUSTOM_PROTOC_EXECUTABLE to generate its .pb.{h,cc}. Buildroot 2025.02's host
# protoc is 29.3, which emits `#include "google/protobuf/runtime_version.h"` -- a header
# that does not exist in the v21.12 runtime -> fatal compile error. So generate the
# protos with a matching 3.21.12 protoc (self-contained copy from the 2024.02 tree).
ONNXRUNTIME_PROTOC = /root/ort-protoc-3.21.12/bin/protoc

# onnxruntime appends a blanket `-Werror` at the END of each target's flags, so a blanket
# `-Wno-error` in CMAKE_CXX_FLAGS (which lands earlier) is overridden. A *specific*
# `-Wno-error=<warn>` overrides a later blanket `-Werror` regardless of order (GCC
# semantics), so enumerate the warnings GCC 13 (Buildroot 2025.02) newly promotes to
# errors in onnxruntime that older toolchains did not. Extra entries are harmless no-ops.
ONNXRUNTIME_WNOERROR = \
	-Wno-error=array-bounds \
	-Wno-error=range-loop-construct \
	-Wno-error=dangling-reference \
	-Wno-error=maybe-uninitialized \
	-Wno-error=restrict \
	-Wno-error=stringop-overflow \
	-Wno-error=stringop-overread \
	-Wno-error=nonnull \
	-Wno-error=attributes

# Buildroot's host-cmake bundles a curl without TLS, so onnxruntime's CMake FetchContent
# https downloads fail ("Protocol https not supported"). deps.txt accepts local file
# paths, so pre-download the needed deps to a persistent cache and rewrite deps.txt to
# point at them (SHA1s preserved and re-verified). Runs at post-extract so dircleans work.
define ONNXRUNTIME_REWIRE_DEPS
	/root/llamaste-build/ort-deps-fetch.sh $(@D)/cmake/deps.txt /root/llamaste-build/ort-deps-cache \
		abseil_cpp protobuf onnx flatbuffers re2 date safeint mp11 microsoft_gsl pytorch_cpuinfo json eigen
endef
ONNXRUNTIME_POST_EXTRACT_HOOKS += ONNXRUNTIME_REWIRE_DEPS

# Install shared lib and headers to staging for sherpa-onnx
# Note: ONNXRUNTIME_SUBDIR=cmake means build dir is $(@D)/cmake/buildroot-build/
define ONNXRUNTIME_INSTALL_STAGING_CMDS
	# Shared library
	find $(@D)/cmake/buildroot-build -name "libonnxruntime.so*" -exec \
		cp -a {} $(STAGING_DIR)/usr/lib/ \;
	# All headers — sherpa-onnx expects flat include path (onnxruntime_cxx_api.h etc.)
	mkdir -p $(STAGING_DIR)/usr/include/onnxruntime
	for h in $(@D)/include/onnxruntime/core/session/*.h; do \
		$(INSTALL) -m 0644 "$$h" $(STAGING_DIR)/usr/include/onnxruntime/; \
	done
endef

# Install shared lib to target rootfs
define ONNXRUNTIME_INSTALL_TARGET_CMDS
	find $(@D)/cmake/buildroot-build -name "libonnxruntime.so*" -exec \
		cp -a {} $(TARGET_DIR)/usr/lib/ \;
endef

$(eval $(cmake-package))

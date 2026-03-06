################################################################################
#
# onnxruntime — ONNX inference engine, built from source for musl
#
# Builds CPU-only shared library for linking by sherpa-onnx.
# Uses vendored deps (FetchContent) — no system abseil/re2/etc. needed.
#
################################################################################

ONNXRUNTIME_VERSION = v1.24.2
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
	-DCMAKE_BUILD_TYPE=MinSizeRel \
	-Donnxruntime_BUILD_SHARED_LIB=ON \
	-Donnxruntime_BUILD_UNIT_TESTS=OFF \
	-Donnxruntime_BUILD_BENCHMARKS=OFF \
	-Donnxruntime_CROSS_COMPILING=ON \
	-Donnxruntime_DISABLE_RTTI=OFF \
	-Donnxruntime_DISABLE_EXCEPTIONS=OFF \
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
	-DONNX_CUSTOM_PROTOC_EXECUTABLE=$(HOST_DIR)/bin/protoc \
	-DFETCHCONTENT_QUIET=OFF

# Install shared lib and headers to staging for sherpa-onnx
# Note: ONNXRUNTIME_SUBDIR=cmake means build dir is $(@D)/cmake/buildroot-build/
define ONNXRUNTIME_INSTALL_STAGING_CMDS
	# Shared library
	find $(@D)/cmake/buildroot-build -name "libonnxruntime.so*" -exec \
		cp -a {} $(STAGING_DIR)/usr/lib/ \;
	# C API header
	$(INSTALL) -D -m 0644 \
		$(@D)/include/onnxruntime/core/session/onnxruntime_c_api.h \
		$(STAGING_DIR)/usr/include/onnxruntime/core/session/onnxruntime_c_api.h
	# Additional headers sherpa-onnx may need
	for h in onnxruntime_session_options_config_keys.h onnxruntime_run_options_config_keys.h; do \
		if [ -f "$(@D)/include/onnxruntime/core/session/$$h" ]; then \
			$(INSTALL) -D -m 0644 \
				"$(@D)/include/onnxruntime/core/session/$$h" \
				"$(STAGING_DIR)/usr/include/onnxruntime/core/session/$$h"; \
		fi; \
	done
endef

# Install shared lib to target rootfs
define ONNXRUNTIME_INSTALL_TARGET_CMDS
	find $(@D)/cmake/buildroot-build -name "libonnxruntime.so*" -exec \
		cp -a {} $(TARGET_DIR)/usr/lib/ \;
endef

$(eval $(cmake-package))

################################################################################
#
# sherpa-onnx — offline TTS (Piper VITS) via onnxruntime C API
#
################################################################################

SHERPA_ONNX_VERSION = v1.11.3
SHERPA_ONNX_SITE = $(call github,k2-fsa,sherpa-onnx,$(SHERPA_ONNX_VERSION))
SHERPA_ONNX_LICENSE = Apache-2.0
SHERPA_ONNX_LICENSE_FILES = LICENSE
SHERPA_ONNX_INSTALL_STAGING = YES
SHERPA_ONNX_INSTALL_TARGET = YES

SHERPA_ONNX_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DBUILD_SHARED_LIBS=ON \
	-DSHERPA_ONNX_ENABLE_TTS=ON \
	-DSHERPA_ONNX_ENABLE_C_API=ON \
	-DSHERPA_ONNX_ENABLE_BINARY=OFF \
	-DSHERPA_ONNX_ENABLE_PYTHON=OFF \
	-DSHERPA_ONNX_ENABLE_TESTS=OFF \
	-DSHERPA_ONNX_ENABLE_CHECK=OFF \
	-DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF \
	-DSHERPA_ONNX_ENABLE_JNI=OFF \
	-DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF \
	-DSHERPA_ONNX_ENABLE_GPU=OFF \
	-DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF \
	-DFETCHCONTENT_QUIET=OFF

# Install shared libs + C API header to staging for llamaste linking
define SHERPA_ONNX_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/sherpa-onnx/c-api/c-api.h \
		$(STAGING_DIR)/usr/include/sherpa-onnx/c-api/c-api.h
	find $(@D)/buildroot-build/lib -name "*.so*" -exec \
		cp -a {} $(STAGING_DIR)/usr/lib/ \;
endef

# Install shared libs to target rootfs
define SHERPA_ONNX_INSTALL_TARGET_CMDS
	find $(@D)/buildroot-build/lib -name "*.so*" -exec \
		cp -a {} $(TARGET_DIR)/usr/lib/ \;
endef

$(eval $(cmake-package))

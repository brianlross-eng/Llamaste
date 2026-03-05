################################################################################
#
# whisper-cpp -- whisper.cpp speech-to-text static library
#
################################################################################

WHISPER_CPP_VERSION = v1.8.3
WHISPER_CPP_SITE = $(call github,ggml-org,whisper.cpp,$(WHISPER_CPP_VERSION))
WHISPER_CPP_LICENSE = MIT
WHISPER_CPP_LICENSE_FILES = LICENSE
WHISPER_CPP_INSTALL_STAGING = YES

WHISPER_CPP_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DBUILD_SHARED_LIBS=OFF \
	-DGGML_STATIC=ON \
	-DGGML_NATIVE=OFF \
	-DGGML_CPU=ON \
	-DGGML_OPENMP=OFF \
	-DGGML_CUDA=OFF \
	-DGGML_VULKAN=OFF \
	-DGGML_METAL=OFF \
	-DGGML_OPENCL=OFF \
	-DGGML_SYCL=OFF \
	-DGGML_BLAS=OFF \
	-DGGML_ACCELERATE=OFF \
	-DGGML_LLAMAFILE=ON \
	-DGGML_CPU_ALL_VARIANTS=OFF \
	-DWHISPER_BUILD_TESTS=OFF \
	-DWHISPER_BUILD_EXAMPLES=OFF \
	-DWHISPER_BUILD_SERVER=OFF \
	-DWHISPER_COREML=OFF \
	-DWHISPER_OPENVINO=OFF \
	-DWHISPER_CURL=OFF \
	-DWHISPER_SDL2=OFF \
	-DWHISPER_FFMPEG=OFF

# Install static libraries + headers to staging for linking into llamaste
define WHISPER_CPP_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/src/libwhisper.a \
		$(STAGING_DIR)/usr/lib/libwhisper.a
	$(INSTALL) -D -m 0644 $(@D)/ggml/src/libggml.a \
		$(STAGING_DIR)/usr/lib/libggml.a
	$(INSTALL) -D -m 0644 $(@D)/ggml/src/libggml-base.a \
		$(STAGING_DIR)/usr/lib/libggml-base.a
	$(INSTALL) -D -m 0644 $(@D)/ggml/src/libggml-cpu.a \
		$(STAGING_DIR)/usr/lib/libggml-cpu.a
	$(INSTALL) -D -m 0644 $(@D)/include/whisper.h \
		$(STAGING_DIR)/usr/include/whisper.h
	$(INSTALL) -D -m 0644 $(@D)/ggml/include/ggml.h \
		$(STAGING_DIR)/usr/include/ggml.h
	$(INSTALL) -D -m 0644 $(@D)/ggml/include/ggml-alloc.h \
		$(STAGING_DIR)/usr/include/ggml-alloc.h
	$(INSTALL) -D -m 0644 $(@D)/ggml/include/ggml-backend.h \
		$(STAGING_DIR)/usr/include/ggml-backend.h
	$(INSTALL) -D -m 0644 $(@D)/ggml/include/ggml-cpu.h \
		$(STAGING_DIR)/usr/include/ggml-cpu.h
endef

$(eval $(cmake-package))

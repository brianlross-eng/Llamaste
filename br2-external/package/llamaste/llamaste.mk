################################################################################
#
# llamaste
#
################################################################################

LLAMASTE_VERSION = 0.1.0
LLAMASTE_SITE = $(HOME)/llamaste-build/llama.cpp
LLAMASTE_SITE_METHOD = local
LLAMASTE_LICENSE = Apache-2.0
LLAMASTE_INSTALL_STAGING = NO
LLAMASTE_INSTALL_TARGET = YES
LLAMASTE_SUPPORTS_IN_SOURCE_BUILD = NO

LLAMASTE_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DGGML_STATIC=ON \
	-DBUILD_SHARED_LIBS=OFF \
	-DGGML_NATIVE=OFF \
	-DGGML_CUDA=OFF \
	-DGGML_VULKAN=OFF \
	-DGGML_METAL=OFF \
	-DGGML_RPC=OFF \
	-DGGML_BLAS=OFF \
	-DLLAMA_CURL=OFF \
	-DLLAMA_BUILD_TESTS=OFF \
	-DLLAMA_BUILD_EXAMPLES=ON \
	-DCMAKE_EXE_LINKER_FLAGS="-static" \
	-DCMAKE_C_FLAGS="-static" \
	-DCMAKE_CXX_FLAGS="-static"

define LLAMASTE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/bin/llama-server \
		$(TARGET_DIR)/opt/llamaste/llama-server
endef

$(eval $(cmake-package))

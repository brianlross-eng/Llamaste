################################################################################
#
# llama-server -- llama.cpp inference server
#
################################################################################

LLAMA_SERVER_VERSION = b5460
LLAMA_SERVER_SITE = $(call github,ggml-org,llama.cpp,$(LLAMA_SERVER_VERSION))
LLAMA_SERVER_LICENSE = MIT
LLAMA_SERVER_LICENSE_FILES = LICENSE

LLAMA_SERVER_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DGGML_STATIC=ON \
	-DBUILD_SHARED_LIBS=OFF \
	-DGGML_NATIVE=OFF \
	-DGGML_CPU=ON \
	-DGGML_CUDA=OFF \
	-DGGML_VULKAN=OFF \
	-DGGML_METAL=OFF \
	-DGGML_RPC=ON \
	-DGGML_BLAS=OFF \
	-DLLAMA_CURL=OFF \
	-DLLAMA_BUILD_TESTS=OFF \
	-DLLAMA_BUILD_EXAMPLES=OFF \
	-DLLAMA_BUILD_SERVER=ON

define LLAMA_SERVER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/bin/llama-server \
		$(TARGET_DIR)/opt/llamaste/llama-server
	$(INSTALL) -D -m 0755 $(@D)/bin/rpc-server \
		$(TARGET_DIR)/opt/llamaste/llama-rpc-server
endef

$(eval $(cmake-package))

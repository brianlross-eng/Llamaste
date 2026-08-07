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
	-DGGML_NATIVE=ON \
	-DGGML_CPU=ON \
	-DGGML_CUDA=OFF \
	-DGGML_VULKAN=ON \
	-DGGML_METAL=OFF \
	-DGGML_HIP=OFF \
	-DGGML_RPC=ON \
	-DGGML_BLAS=OFF \
	-DLLAMA_CURL=OFF \
	-DLLAMA_BUILD_TESTS=OFF \
	-DLLAMA_BUILD_EXAMPLES=OFF \
	-DLLAMA_BUILD_SERVER=ON

# GPU backend notes:
# - Vulkan (ON): portable; works on AMD, Intel, NVIDIA GPUs. Requires
#   vulkan-loader (BR2_PACKAGE_VULKAN_LOADER=y) and Mesa Vulkan drivers.
# - HIP/ROCm (OFF): AMD GPU compute via HSA/KFD (/dev/kfd). Requires a ROCm
#   cross-compiler (hipcc) not available in Buildroot; llama.cpp b5460 hard-fails
#   at enable_language(HIP) when GGML_HIP=ON without it. AMD GPUs (incl. Strix
#   Halo gfx1151) use the Vulkan/RADV backend above instead.
# - CUDA (OFF): requires proprietary NVIDIA driver + CUDA toolkit, not
#   available in Buildroot. NVIDIA GPUs should use the Vulkan backend.
# - BLAS (OFF): would need OpenBLAS (BR2_PACKAGE_OPENBLAS=y) in the target. With
#   GGML_BLAS=ON but no BLAS lib, ggml-backend-reg references ggml_backend_blas_reg
#   which is never built -> link error. CPU acceleration still comes from GGML_NATIVE
#   (AVX2/AVX-512). Re-enable by adding OpenBLAS to the defconfig if CPU BLAS is wanted.

define LLAMA_SERVER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/bin/llama-server \
		$(TARGET_DIR)/opt/llamaste/llama-server
	$(INSTALL) -D -m 0755 $(@D)/bin/rpc-server \
		$(TARGET_DIR)/opt/llamaste/llama-rpc-server
endef

$(eval $(cmake-package))

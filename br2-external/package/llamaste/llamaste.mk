################################################################################
#
# llamaste — LLM IS the OS
#
# Builds the llamaste binary from src/llamaste/ which includes:
# - PID 1 supervisor with child fork pattern
# - HTTP server (httplib) with agent chat, dashboard, OpenAI API
# - 25 system tools across 6 categories
# - mDNS responder for llamaste.local
# - Embedded web UI (chat + dashboard)
#
################################################################################

LLAMASTE_VERSION = 0.2.0
LLAMASTE_SITE = /mnt/d/Llamaste/src/llamaste
LLAMASTE_SITE_METHOD = local
LLAMASTE_LICENSE = Apache-2.0
LLAMASTE_INSTALL_STAGING = NO
LLAMASTE_INSTALL_TARGET = YES
LLAMASTE_SUPPORTS_IN_SOURCE_BUILD = NO

# Dynamic linking: rootfs already has shared libs from desktop packages
# (eudev, wayland, mesa, cage, wpewebkit, etc.) so the musl dynamic
# linker and all .so deps are already present.  This avoids the
# transitive static-dependency chain for libcurl (nghttp2, psl, icu, z).
LLAMASTE_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DLLAMASTE_STATIC=OFF \
	-DLLAMASTE_EMBED_WEB=ON \
	-DLLAMASTE_TEST_API=ON

# sherpa-onnx added when BR2_PACKAGE_SHERPA_ONNX=y is enabled in .config
LLAMASTE_DEPENDENCIES = libcurl openssl whisper-cpp alsa-lib espeak-ng
ifeq ($(BR2_PACKAGE_SHERPA_ONNX),y)
LLAMASTE_DEPENDENCIES += sherpa-onnx
endif

define LLAMASTE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/buildroot-build/llamaste \
		$(TARGET_DIR)/opt/llamaste/llamaste
	$(INSTALL) -D -m 0755 $(@D)/buildroot-build/dhcpcd-hook \
		$(TARGET_DIR)/lib/dhcpcd/dhcpcd-run-hooks
endef

$(eval $(cmake-package))

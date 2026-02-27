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

LLAMASTE_VERSION = 0.1.0
LLAMASTE_SITE = /mnt/d/Llamaste/src/llamaste
LLAMASTE_SITE_METHOD = local
LLAMASTE_LICENSE = Apache-2.0
LLAMASTE_INSTALL_STAGING = NO
LLAMASTE_INSTALL_TARGET = YES
LLAMASTE_SUPPORTS_IN_SOURCE_BUILD = NO

LLAMASTE_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DLLAMASTE_STATIC=ON \
	-DLLAMASTE_EMBED_WEB=ON

define LLAMASTE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/llamaste \
		$(TARGET_DIR)/opt/llamaste/llamaste
endef

$(eval $(cmake-package))

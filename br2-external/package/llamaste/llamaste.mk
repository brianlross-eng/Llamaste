################################################################################
#
# llamaste
#
################################################################################

LLAMASTE_VERSION = 0.1.0
LLAMASTE_SITE = /mnt/d/Llamaste/src/llamaste
LLAMASTE_SITE_METHOD = local
LLAMASTE_LICENSE = Apache-2.0
LLAMASTE_INSTALL_STAGING = NO
LLAMASTE_INSTALL_TARGET = YES

# Placeholder — will add cmake-package integration in Task 3
# For now, just install a hello-world static binary to verify the pipeline

define LLAMASTE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -static \
		-o $(@D)/llamaste $(@D)/stub.c
endef

define LLAMASTE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/llamaste $(TARGET_DIR)/opt/llamaste/llamaste
endef

$(eval $(generic-package))

################################################################################
#
# labwc -- stacking Wayland compositor (wlroots-based, openbox-compatible config)
#
################################################################################

LABWC_VERSION = 0.6.6
LABWC_SITE = $(call github,labwc,labwc,$(LABWC_VERSION))
LABWC_SOURCE = labwc-$(LABWC_VERSION).tar.gz
LABWC_LICENSE = GPL-2.0
LABWC_LICENSE_FILES = LICENSE

LABWC_DEPENDENCIES = \
	host-pkgconf \
	wlroots \
	libglib2 \
	cairo \
	pango \
	libxml2 \
	libdrm \
	libpng \
	libinput \
	libxkbcommon \
	wayland-protocols

LABWC_CONF_OPTS = \
	-Dxwayland=disabled \
	-Dman-pages=disabled

$(eval $(meson-package))

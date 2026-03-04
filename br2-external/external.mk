include $(sort $(wildcard $(BR2_EXTERNAL_LLAMASTE_PATH)/package/*/*.mk))

# Fix: ICU (C++ library, pulled in by WPEWebKit) installs into the sysroot but
# the linker can't find libstdc++.so when resolving ICU's DT_NEEDED entries.
# libstdc++.so lives in the toolchain lib64/ dir, not the sysroot. This breaks
# ANY C package that transitively depends on ICU (libxml2, libsoup3, gio, etc.)
# with undefined C++ references (__gxx_personality_v0, operator delete, etc.).
#
# Fix 1: Symlink libstdc++ into sysroot so the linker can find it globally.
# Fix 2: Also disable ICU in libxml2 (it doesn't need unicode normalization).

# After ICU installs to staging, create libstdc++ symlinks in the sysroot
define ICU_SYMLINK_LIBSTDCXX
	@if [ ! -e "$(STAGING_DIR)/usr/lib64/libstdc++.so" ]; then \
		ln -sf ../../../lib64/libstdc++.so "$(STAGING_DIR)/usr/lib64/libstdc++.so"; \
	fi
	@if [ ! -e "$(STAGING_DIR)/usr/lib64/libstdc++.so.6" ]; then \
		ln -sf ../../../lib64/libstdc++.so.6 "$(STAGING_DIR)/usr/lib64/libstdc++.so.6"; \
	fi
endef
ICU_POST_INSTALL_STAGING_HOOKS += ICU_SYMLINK_LIBSTDCXX

# libxml2 doesn't need ICU features — disable it to avoid the C/C++ link issue
LIBXML2_CONF_OPTS += --without-icu

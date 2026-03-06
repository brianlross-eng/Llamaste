################################################################################
#
# espeak-ng — open source speech synthesizer (formant, 22kHz, 100+ languages)
#
# Uses the csukuangfj fork which adds CMake support (upstream uses autotools).
# Same commit that sherpa-onnx v1.11.3 uses for piper-phonemize compatibility.
#
# Cross-compilation note: phoneme data (phontab, phondata, phonindex,
# intonations, *_dict) must be compiled by running espeak-ng-bin natively.
# We build the library for the target (cross) but compile data using a
# native (host) build of the same source, done in POST_BUILD_HOOKS.
#
################################################################################

ESPEAK_NG_VERSION = f6fed6c58b5e0998b8e68c6610125e2d07d595a7
ESPEAK_NG_SITE = $(call github,csukuangfj,espeak-ng,$(ESPEAK_NG_VERSION))
ESPEAK_NG_LICENSE = GPL-3.0+
ESPEAK_NG_LICENSE_FILES = COPYING
ESPEAK_NG_INSTALL_STAGING = YES
ESPEAK_NG_INSTALL_TARGET = YES
ESPEAK_NG_SUPPORTS_IN_SOURCE_BUILD = NO

# Cross build: library only (no exe, data compiled natively below)
ESPEAK_NG_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DBUILD_SHARED_LIBS=ON \
	-DBUILD_ESPEAK_NG_TESTS=OFF \
	-DBUILD_ESPEAK_NG_EXE=OFF \
	-DUSE_ASYNC=OFF \
	-DUSE_MBROLA=OFF \
	-DUSE_LIBSONIC=OFF \
	-DUSE_LIBPCAUDIO=OFF \
	-DUSE_KLATT=OFF \
	-DUSE_SPEECHPLAYER=OFF \
	-DEXTRA_cmn=ON \
	-DEXTRA_ru=ON

# After cross-building the library, do a native build to compile phoneme data.
# The data files are architecture-independent (phoneme tables, dictionaries).
define ESPEAK_NG_COMPILE_DATA
	@echo "Compiling espeak-ng phoneme data (native build)..."
	mkdir -p $(@D)/native-build
	cd $(@D)/native-build && cmake $(@D) \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=ON \
		-DBUILD_ESPEAK_NG_EXE=ON \
		-DBUILD_ESPEAK_NG_TESTS=OFF \
		-DUSE_ASYNC=OFF \
		-DUSE_MBROLA=OFF \
		-DUSE_LIBSONIC=OFF \
		-DUSE_LIBPCAUDIO=OFF \
		-DUSE_KLATT=OFF \
		-DUSE_SPEECHPLAYER=OFF \
		$(if $(QUIET),-DCMAKE_VERBOSE_MAKEFILE=OFF,) \
		> /dev/null 2>&1
	$(MAKE) -C $(@D)/native-build -j$(PARALLEL_JOBS) > /dev/null 2>&1
endef
ESPEAK_NG_POST_BUILD_HOOKS += ESPEAK_NG_COMPILE_DATA

# Install library + headers to staging for llamaste linking
define ESPEAK_NG_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/src/include/espeak-ng/speak_lib.h \
		$(STAGING_DIR)/usr/include/espeak-ng/speak_lib.h
	$(INSTALL) -D -m 0644 $(@D)/src/include/espeak-ng/espeak_ng.h \
		$(STAGING_DIR)/usr/include/espeak-ng/espeak_ng.h
	find $(@D)/buildroot-build -name "libespeak-ng*.so*" -exec \
		cp -a {} $(STAGING_DIR)/usr/lib/ \;
	find $(@D)/buildroot-build -name "libucd*.so*" -exec \
		cp -a {} $(STAGING_DIR)/usr/lib/ \;
endef

# Install shared libs + natively-compiled data files to target rootfs
define ESPEAK_NG_INSTALL_TARGET_CMDS
	find $(@D)/buildroot-build -name "libespeak-ng*.so*" -exec \
		cp -a {} $(TARGET_DIR)/usr/lib/ \;
	find $(@D)/buildroot-build -name "libucd*.so*" -exec \
		cp -a {} $(TARGET_DIR)/usr/lib/ \;
	mkdir -p $(TARGET_DIR)/usr/share/espeak-ng-data
	cp -r $(@D)/native-build/espeak-ng-data/* \
		$(TARGET_DIR)/usr/share/espeak-ng-data/
endef

$(eval $(cmake-package))

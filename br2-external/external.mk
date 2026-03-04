include $(sort $(wildcard $(BR2_EXTERNAL_LLAMASTE_PATH)/package/*/*.mk))

# Fix: libxml2 links against ICU (a C++ library pulled in by WPEWebKit), but
# libxml2's programs (xmllint, xmlcatalog) are pure C. The C linker doesn't add
# -lstdc++, so the transitive DT_NEEDED from libicui18n.so -> libstdc++.so.6
# fails to resolve, causing undefined references to __gxx_personality_v0 etc.
LIBXML2_CONF_ENV += LIBS="-lstdc++"

cmake_minimum_required(VERSION 3.14)
project(llamaste VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LLAMASTE_SOURCES
    main.cpp
    supervisor.cpp
    init.cpp
    hwdetect.cpp
    child_main.cpp
    tools.cpp
    tools_fs.cpp
    tools_process.cpp
    tools_network.cpp
    tools_system.cpp
    tools_config.cpp
    tools_model.cpp
    tools_install.cpp
    tools_schedule.cpp
    tools_auth.cpp
    tools_model_download.cpp
    tools_audio.cpp
    tools_cluster.cpp
    tools_update.cpp
    cluster.cpp
    updater.cpp
    tweetnacl.c
    voice.cpp
    mcp_server.cpp
    agent.cpp
    prompt_builder.cpp
    net_mdns.cpp
    scheduler.cpp
    bcrypt.cpp
    auth.cpp
)

add_executable(llamaste ${LLAMASTE_SOURCES})

# httplib.h is in the source directory; ensure it is on the include path
target_include_directories(llamaste PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# httplib requires threads (pthreads on Linux)
find_package(Threads REQUIRED)
target_link_libraries(llamaste PRIVATE Threads::Threads)

option(LLAMASTE_STATIC "Build fully static binary" OFF)
if(LLAMASTE_STATIC)
    target_link_options(llamaste PRIVATE -static)
else()
    # Even without full-static, always link C++ runtime statically so we don't
    # need libstdc++.so / libgcc_s.so in the rootfs.  Shared libs like
    # libcurl.so are still linked dynamically.
    target_link_options(llamaste PRIVATE -static-libgcc -static-libstdc++)
endif()

# liblzma for XZ decompression in installer (optional, provided by BR2_PACKAGE_XZ)
# Only search for static library when building static binary
if(LLAMASTE_STATIC)
    find_library(LZMA_LIB NAMES liblzma.a)
else()
    find_library(LZMA_LIB NAMES lzma)
endif()
if(LZMA_LIB)
    message(STATUS "Found liblzma: ${LZMA_LIB}")
    target_compile_definitions(llamaste PRIVATE HAVE_LZMA)
    target_link_libraries(llamaste PRIVATE ${LZMA_LIB})
else()
    message(STATUS "liblzma not found - XZ decompression disabled, installer will use raw images only")
endif()

# libcurl for HTTPS downloads (model download from Hugging Face)
# Static linking requires all transitive deps (nghttp2, psl, ssl, crypto, z).
# Use pkg-config --static to resolve the full chain automatically.
find_package(PkgConfig QUIET)
set(CURL_FOUND FALSE)
if(PKG_CONFIG_FOUND AND LLAMASTE_STATIC)
    pkg_check_modules(CURL_STATIC QUIET IMPORTED_TARGET libcurl)
    if(CURL_STATIC_FOUND)
        # Get the full static link flags (includes transitive deps)
        execute_process(
            COMMAND ${PKG_CONFIG_EXECUTABLE} --libs --static libcurl
            OUTPUT_VARIABLE CURL_STATIC_LDFLAGS
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        message(STATUS "Found libcurl (pkg-config static): ${CURL_STATIC_LDFLAGS}")
        target_compile_definitions(llamaste PRIVATE HAVE_LIBCURL CURL_STATICLIB)
        # Parse the flags and add them as link libraries
        separate_arguments(CURL_STATIC_LDFLAGS_LIST UNIX_COMMAND "${CURL_STATIC_LDFLAGS}")
        target_link_libraries(llamaste PRIVATE ${CURL_STATIC_LDFLAGS_LIST})
        set(CURL_FOUND TRUE)
    endif()
endif()
if(NOT CURL_FOUND)
    # Fallback: find shared libcurl
    find_library(CURL_LIB NAMES curl)
    if(CURL_LIB)
        message(STATUS "Found libcurl (shared): ${CURL_LIB}")
        target_compile_definitions(llamaste PRIVATE HAVE_LIBCURL)
        target_link_libraries(llamaste PRIVATE ${CURL_LIB})
        set(CURL_FOUND TRUE)
    else()
        message(STATUS "libcurl not found - model download disabled, search/browse only with pre-placed models")
    endif()
endif()

# whisper.cpp for speech-to-text (Voice I/O)
find_library(WHISPER_LIB NAMES whisper)
find_library(GGML_LIB NAMES ggml)
find_library(GGML_BASE_LIB NAMES ggml-base)
find_library(GGML_CPU_LIB NAMES ggml-cpu)
if(WHISPER_LIB)
    message(STATUS "Found whisper: ${WHISPER_LIB}")
    target_compile_definitions(llamaste PRIVATE HAVE_WHISPER)
    target_link_libraries(llamaste PRIVATE ${WHISPER_LIB})
    if(GGML_LIB)
        target_link_libraries(llamaste PRIVATE ${GGML_LIB})
    endif()
    if(GGML_CPU_LIB)
        target_link_libraries(llamaste PRIVATE ${GGML_CPU_LIB})
    endif()
    if(GGML_BASE_LIB)
        target_link_libraries(llamaste PRIVATE ${GGML_BASE_LIB})
    endif()
    # whisper/ggml deps
    target_link_libraries(llamaste PRIVATE m dl)
else()
    message(STATUS "whisper not found — speech-to-text disabled")
endif()

# ALSA for audio capture/playback (Voice I/O)
find_library(ALSA_LIB NAMES asound)
if(ALSA_LIB)
    message(STATUS "Found ALSA: ${ALSA_LIB}")
    target_compile_definitions(llamaste PRIVATE HAVE_ALSA)
    target_link_libraries(llamaste PRIVATE ${ALSA_LIB})
else()
    message(STATUS "ALSA not found — audio capture disabled")
endif()

# Flite TTS (BSD-licensed, linked directly)
find_library(FLITE_LIB NAMES flite)
find_library(FLITE_USENGLISH_LIB NAMES flite_usenglish)
find_library(FLITE_CMULEX_LIB NAMES flite_cmulex)
find_library(FLITE_CMU_US_KAL_LIB NAMES flite_cmu_us_kal)
if(FLITE_LIB)
    message(STATUS "Found flite: ${FLITE_LIB}")
    target_compile_definitions(llamaste PRIVATE HAVE_FLITE)
    target_link_libraries(llamaste PRIVATE
        ${FLITE_CMU_US_KAL_LIB}
        ${FLITE_USENGLISH_LIB}
        ${FLITE_CMULEX_LIB}
        ${FLITE_LIB}
        m)
else()
    message(STATUS "flite not found — text-to-speech disabled")
endif()

# espeak-ng for formant TTS (22kHz, 100+ languages)
find_library(ESPEAK_NG_LIB NAMES espeak-ng)
find_path(ESPEAK_NG_INCLUDE NAMES espeak-ng/speak_lib.h)
if(ESPEAK_NG_LIB AND ESPEAK_NG_INCLUDE)
    message(STATUS "Found espeak-ng: ${ESPEAK_NG_LIB}")
    target_compile_definitions(llamaste PRIVATE HAVE_ESPEAK_NG)
    target_include_directories(llamaste PRIVATE ${ESPEAK_NG_INCLUDE})
    target_link_libraries(llamaste PRIVATE ${ESPEAK_NG_LIB})
else()
    message(STATUS "espeak-ng not found — formant TTS disabled, using flite fallback")
endif()

# sherpa-onnx for neural TTS (Piper VITS models) — requires musl-built onnxruntime
find_library(SHERPA_ONNX_LIB NAMES sherpa-onnx-c-api)
find_path(SHERPA_ONNX_INCLUDE NAMES sherpa-onnx/c-api/c-api.h)
if(SHERPA_ONNX_LIB AND SHERPA_ONNX_INCLUDE)
    message(STATUS "Found sherpa-onnx: ${SHERPA_ONNX_LIB}")
    target_compile_definitions(llamaste PRIVATE HAVE_SHERPA_ONNX)
    target_include_directories(llamaste PRIVATE ${SHERPA_ONNX_INCLUDE})
    target_link_libraries(llamaste PRIVATE ${SHERPA_ONNX_LIB})
else()
    message(STATUS "sherpa-onnx not found — neural TTS disabled")
endif()

# Web asset embedding (production builds)
# When web_embed.cpp/h are generated, add them and define LLAMASTE_HAS_EMBED
set(WEB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(WEB_EMBED_CPP "${CMAKE_CURRENT_BINARY_DIR}/web_embed.cpp")
set(WEB_EMBED_H "${CMAKE_CURRENT_BINARY_DIR}/web_embed.h")

option(LLAMASTE_EMBED_WEB "Embed web UI files into the binary" OFF)
if(LLAMASTE_EMBED_WEB)
    add_custom_command(
        OUTPUT ${WEB_EMBED_CPP} ${WEB_EMBED_H}
        COMMAND ${CMAKE_COMMAND}
          -DWEB_DIR=${WEB_DIR}
          -DOUTPUT_CPP=${WEB_EMBED_CPP}
          -DOUTPUT_H=${WEB_EMBED_H}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_web.cmake
        DEPENDS ${WEB_DIR}/index.html ${WEB_DIR}/chat.js
                ${WEB_DIR}/dashboard.js ${WEB_DIR}/files.js
                ${WEB_DIR}/system.js ${WEB_DIR}/notifications.js
                ${WEB_DIR}/install.js ${WEB_DIR}/style.css
                ${WEB_DIR}/login.html ${WEB_DIR}/setup.html
        COMMENT "Embedding web assets into C++ source"
    )
    target_sources(llamaste PRIVATE ${WEB_EMBED_CPP})
    target_include_directories(llamaste PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(llamaste PRIVATE LLAMASTE_HAS_EMBED)
endif()

install(TARGETS llamaste RUNTIME DESTINATION opt/llamaste)

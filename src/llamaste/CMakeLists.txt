cmake_minimum_required(VERSION 3.14)
project(llamaste VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Option to build against an external llama.cpp source tree
set(LLAMA_CPP_DIR "" CACHE PATH "Path to llama.cpp source directory")

if(NOT LLAMA_CPP_DIR)
    message(FATAL_ERROR "LLAMA_CPP_DIR must be set to the llama.cpp source directory")
endif()

# Build llama.cpp as a subdirectory
# We need: libllama, libggml, the server's httplib, and common utilities
add_subdirectory(${LLAMA_CPP_DIR} ${CMAKE_BINARY_DIR}/llama.cpp)

# Collect our source files
set(LLAMASTE_SOURCES
    main.cpp
    agent.cpp
    tools.cpp
    tools_fs.cpp
    tools_process.cpp
    tools_network.cpp
    tools_system.cpp
    tools_config.cpp
    tools_model.cpp
    prompt_builder.cpp
)

add_executable(llamaste ${LLAMASTE_SOURCES})

target_include_directories(llamaste PRIVATE
    ${LLAMA_CPP_DIR}/include
    ${LLAMA_CPP_DIR}/common
    ${LLAMA_CPP_DIR}/ggml/include
    ${LLAMA_CPP_DIR}/examples/server
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(llamaste PRIVATE
    llama
    ggml
    common
)

# Static linking for musl builds
option(LLAMASTE_STATIC "Build fully static binary" OFF)
if(LLAMASTE_STATIC)
    target_link_options(llamaste PRIVATE -static)
endif()

# Embed web UI files into the binary
# We'll use a simple xxd-style embedding at build time
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/web_embed.h
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_web.cmake
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
        ${CMAKE_CURRENT_SOURCE_DIR}/web/chat.js
        ${CMAKE_CURRENT_SOURCE_DIR}/web/dashboard.js
        ${CMAKE_CURRENT_SOURCE_DIR}/web/style.css
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Embedding web UI files into header"
)
target_sources(llamaste PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/web_embed.h)
target_include_directories(llamaste PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Install
install(TARGETS llamaste RUNTIME DESTINATION opt/llamaste)

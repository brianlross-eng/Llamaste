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
    agent.cpp
    prompt_builder.cpp
    net_mdns.cpp
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
                ${WEB_DIR}/dashboard.js ${WEB_DIR}/style.css
        COMMENT "Embedding web assets into C++ source"
    )
    target_sources(llamaste PRIVATE ${WEB_EMBED_CPP})
    target_include_directories(llamaste PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(llamaste PRIVATE LLAMASTE_HAS_EMBED)
endif()

install(TARGETS llamaste RUNTIME DESTINATION opt/llamaste)

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
)

add_executable(llamaste ${LLAMASTE_SOURCES})

option(LLAMASTE_STATIC "Build fully static binary" OFF)
if(LLAMASTE_STATIC)
    target_link_options(llamaste PRIVATE -static)
endif()

install(TARGETS llamaste RUNTIME DESTINATION opt/llamaste)

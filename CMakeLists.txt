cmake_minimum_required(VERSION 3.10)

project(wl-remote
        VERSION 0.1.0
        LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(PROTOCOL_NAME "wlr-virtual-pointer-unstable-v1")
set(PROTOCOL_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocols/${PROTOCOL_NAME}.xml")
set(PROTOCOL_H "${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL_NAME}-protocol.h")
set(PROTOCOL_C "${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL_NAME}-protocol.c")
add_custom_command(
        OUTPUT ${PROTOCOL_H}
        COMMAND ${WAYLAND_SCANNER} client-header ${PROTOCOL_XML} ${PROTOCOL_H}
        DEPENDS ${PROTOCOL_XML}
        VERBATIM
)
add_custom_command(
        OUTPUT ${PROTOCOL_C}
        COMMAND ${WAYLAND_SCANNER} private-code ${PROTOCOL_XML} ${PROTOCOL_C}
        DEPENDS ${PROTOCOL_XML}
        VERBATIM
)


add_executable(wl-remote
        src/main.c
        ${PROTOCOL_H}
        ${PROTOCOL_C}
)
target_include_directories(wl-remote PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wl-remote PRIVATE PkgConfig::WAYLAND_CLIENT)

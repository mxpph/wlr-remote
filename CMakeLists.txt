cmake_minimum_required(VERSION 3.10)

project(wlr-remote
        VERSION 0.1.0
        LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)
set(make_tests OFF CACHE BOOL "Disable tinydtls tests" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Disable tinydtls shared libraries" FORCE)
set(DTLS_ECC OFF CACHE BOOL "Disable tinydtls ECDHE_ECDSA cipher suites support" FORCE)
set(DTLS_PSK ON CACHE BOOL "Enable tinydtls PSK support" FORCE)
FetchContent_Declare(
        tinydtls
        GIT_REPOSITORY https://github.com/eclipse-tinydtls/tinydtls.git
        GIT_TAG main
)
FetchContent_MakeAvailable(tinydtls)

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

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
add_executable(wlr-remote
        src/main.c
        ${PROTOCOL_H}
        ${PROTOCOL_C}
)
target_include_directories(wlr-remote PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wlr-remote PRIVATE PkgConfig::WAYLAND_CLIENT tinydtls)

add_executable(test-client
        src/test-client.c
)
target_link_libraries(test-client PRIVATE tinydtls)

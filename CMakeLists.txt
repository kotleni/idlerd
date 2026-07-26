cmake_minimum_required(VERSION 3.10)
project(idlerd VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocol)
file(MAKE_DIRECTORY ${PROTOCOL_DIR})

pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
set(IDLE_PROTOCOL_XML ${WAYLAND_PROTOCOLS_DIR}/staging/ext-idle-notify/ext-idle-notify-v1.xml)
if(NOT EXISTS "${IDLE_PROTOCOL_XML}")
    message(FATAL_ERROR "ext-idle-notify-v1.xml not found at ${IDLE_PROTOCOL_XML}")
endif()

add_custom_command(
    OUTPUT ${PROTOCOL_DIR}/idle-protocol.c ${PROTOCOL_DIR}/idle-protocol.h
    COMMAND ${WAYLAND_SCANNER} client-header ${IDLE_PROTOCOL_XML} ${PROTOCOL_DIR}/idle-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code ${IDLE_PROTOCOL_XML} ${PROTOCOL_DIR}/idle-protocol.c
    DEPENDS ${IDLE_PROTOCOL_XML}
    COMMENT "Generating Wayland protocol bindings for ext-idle-notify-v1"
)

add_executable(idlerd
    src/main.c
    ${PROTOCOL_DIR}/idle-protocol.c
)

target_include_directories(idlerd PRIVATE
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${PROTOCOL_DIR}
)

target_link_libraries(idlerd ${WAYLAND_CLIENT_LIBRARIES})
target_link_directories(idlerd PRIVATE ${WAYLAND_CLIENT_LIBRARY_DIRS})

install(TARGETS idlerd DESTINATION bin)

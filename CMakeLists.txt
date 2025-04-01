cmake_minimum_required(VERSION 3.16)

project(packet_forwarder LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDPDK REQUIRED libdpdk)

add_executable(packet_forwarder main.c
    packet_forwarder.h
    packet_forwarder.c
    dpdk_utils.h
    dpdk_utils.c
    utils.h
    utils.c
    dpdk_port.h
    dpdk_port.c
    types.h
    config.h
    dpdk_thresh.c
    dpdk_thresh.h)

target_compile_options(packet_forwarder PRIVATE ${LIBDPDK_CFLAGS})
target_link_libraries(packet_forwarder ${LIBDPDK_LDFLAGS})

include(GNUInstallDirs)
install(TARGETS packet_forwarder
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

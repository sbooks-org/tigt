# SPDX-License-Identifier: MIT-0
# Copyright (C) 2026 Simplebooks Foundation
# Copyright (C) 2026 Josh Rodd
cmake_minimum_required(VERSION 3.20)
project(tigt VERSION 0.2.0 DESCRIPTION "tty-in-glass-tty" LANGUAGES C)

if(NOT UNIX)
    message(FATAL_ERROR "tigt currently requires POSIX terminals and curses")
endif()
find_package(Curses REQUIRED)
find_package(Threads REQUIRED)
add_library(tigt::curses INTERFACE IMPORTED)
set_target_properties(tigt::curses PROPERTIES
    INTERFACE_LINK_LIBRARIES "${CURSES_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${CURSES_INCLUDE_DIRS}")
add_library(tigt src/tigt.c src/input.c)
add_library(tigt::tigt ALIAS tigt)
target_compile_features(tigt PUBLIC c_std_11)
target_compile_definitions(tigt PRIVATE _XOPEN_SOURCE_EXTENDED=1)
target_include_directories(tigt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(tigt PUBLIC tigt::curses Threads::Threads)
set_target_properties(tigt PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 2 POSITION_INDEPENDENT_CODE ON)

option(TIGT_BUILD_EXAMPLES "Build standalone C consumer examples" ${PROJECT_IS_TOP_LEVEL})
if(TIGT_BUILD_EXAMPLES)
    add_executable(tigt-demo examples/demo.c)
    target_link_libraries(tigt-demo PRIVATE tigt::tigt)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS tigt EXPORT tigtTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/tigt.h include/tigt_keyboard.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/tigt)
configure_package_config_file(cmake/tigtConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/tigtConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tigt)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/tigtConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(EXPORT tigtTargets NAMESPACE tigt:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tigt)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/tigtConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/tigtConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tigt)

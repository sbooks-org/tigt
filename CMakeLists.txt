# SPDX-License-Identifier: MIT-0
# Copyright (C) 2026 Simplebooks Foundation
# Copyright (C) 2026 Josh Rodd
cmake_minimum_required(VERSION 3.20)
project(tigt VERSION 0.3.0 DESCRIPTION "tty-in-glass-tty" LANGUAGES C)

if(NOT UNIX)
    message(FATAL_ERROR "tigt currently requires POSIX terminals and curses")
endif()
find_package(Curses REQUIRED)
find_package(Threads REQUIRED)
find_package(PNG REQUIRED)
option(TIGT_WITH_LIBCACA "Enable libcaca bitmap-to-ASCII conversion" OFF)
option(TIGT_WITH_KEYBOARD "Enable the in-tree Rust PC keyboard mapper" OFF)
if(TIGT_WITH_LIBCACA)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CACA REQUIRED IMPORTED_TARGET caca)
endif()
add_library(tigt::curses INTERFACE IMPORTED)
set_target_properties(tigt::curses PROPERTIES
    INTERFACE_LINK_LIBRARIES "${CURSES_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${CURSES_INCLUDE_DIRS}")
add_library(tigt src/tigt.c src/terminal.c src/input.c src/graphics.c src/snapshot.c src/video.c src/presenter.c)
add_library(tigt::tigt ALIAS tigt)
target_compile_features(tigt PUBLIC c_std_11)
target_compile_definitions(tigt PRIVATE _XOPEN_SOURCE_EXTENDED=1)
target_include_directories(tigt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(tigt PUBLIC tigt::curses Threads::Threads PNG::PNG)
if(TIGT_WITH_LIBCACA)
    target_compile_definitions(tigt PRIVATE TIGT_HAVE_LIBCACA=1)
    target_link_libraries(tigt PUBLIC PkgConfig::CACA)
endif()
set_target_properties(tigt PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 4 POSITION_INDEPENDENT_CODE ON)

if(TIGT_WITH_KEYBOARD)
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "TIGT_WITH_KEYBOARD currently supports native builds only")
    endif()
    find_program(TIGT_CARGO_EXECUTABLE cargo REQUIRED)
    find_program(TIGT_RUSTC_EXECUTABLE rustc REQUIRED)
    execute_process(COMMAND "${TIGT_RUSTC_EXECUTABLE}" -vV
        RESULT_VARIABLE TIGT_RUSTC_RESULT OUTPUT_VARIABLE TIGT_RUSTC_VERSION
        ERROR_VARIABLE TIGT_RUSTC_ERROR)
    if(NOT TIGT_RUSTC_RESULT EQUAL 0)
        message(FATAL_ERROR "Cannot identify the Rust host: ${TIGT_RUSTC_ERROR}")
    endif()
    string(REGEX MATCH "host: ([^\r\n]+)" TIGT_RUST_HOST_LINE "${TIGT_RUSTC_VERSION}")
    set(TIGT_RUST_HOST "${CMAKE_MATCH_1}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND TIGT_RUST_HOST MATCHES "-apple-darwin$")
        # The C driver already links libSystem.
        set(TIGT_KEYBOARD_NATIVE_LIBRARIES iconv)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TIGT_RUST_HOST MATCHES "-unknown-linux-gnu$")
        set(TIGT_KEYBOARD_NATIVE_LIBRARIES Threads::Threads ${CMAKE_DL_LIBS} m rt util gcc_s)
    else()
        message(FATAL_ERROR
            "TIGT_WITH_KEYBOARD supports native Darwin and GNU/Linux Rust hosts, not '${TIGT_RUST_HOST}' on ${CMAKE_SYSTEM_NAME}")
    endif()
    set(TIGT_KEYBOARD_C_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        list(LENGTH CMAKE_OSX_ARCHITECTURES TIGT_KEYBOARD_ARCH_COUNT)
        if(NOT TIGT_KEYBOARD_ARCH_COUNT EQUAL 1)
            message(FATAL_ERROR "TIGT_WITH_KEYBOARD does not support universal macOS builds")
        endif()
        set(TIGT_KEYBOARD_C_ARCH "${CMAKE_OSX_ARCHITECTURES}")
    endif()
    if(TIGT_KEYBOARD_C_ARCH STREQUAL "arm64")
        set(TIGT_KEYBOARD_C_ARCH aarch64)
    elseif(TIGT_KEYBOARD_C_ARCH STREQUAL "AMD64")
        set(TIGT_KEYBOARD_C_ARCH x86_64)
    endif()
    string(REGEX MATCH "^[^-]+" TIGT_KEYBOARD_RUST_ARCH "${TIGT_RUST_HOST}")
    if(NOT TIGT_KEYBOARD_C_ARCH STREQUAL TIGT_KEYBOARD_RUST_ARCH)
        message(FATAL_ERROR
            "TIGT_WITH_KEYBOARD needs matching native C and Rust architectures: ${TIGT_KEYBOARD_C_ARCH} != ${TIGT_KEYBOARD_RUST_ARCH}")
    endif()
    set(TIGT_KEYBOARD_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/keyboard/target")
    set(TIGT_KEYBOARD_LIBRARY
        "${TIGT_KEYBOARD_TARGET_DIR}/${TIGT_RUST_HOST}/release/libpc_xt_keyboard.a")
    add_custom_command(OUTPUT "${TIGT_KEYBOARD_LIBRARY}"
        COMMAND "${CMAKE_COMMAND}" -E env "RUSTC=${TIGT_RUSTC_EXECUTABLE}"
            "${TIGT_CARGO_EXECUTABLE}" build
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/keyboard/Cargo.toml"
            --locked --offline --release --target "${TIGT_RUST_HOST}"
            --target-dir "${TIGT_KEYBOARD_TARGET_DIR}"
        DEPENDS keyboard/Cargo.toml keyboard/Cargo.lock
            keyboard/src/lib.rs keyboard/src/ffi.rs
        COMMENT "Building the in-tree PC keyboard mapper"
        VERBATIM)
    add_custom_target(tigt-keyboard-build DEPENDS "${TIGT_KEYBOARD_LIBRARY}")
    add_library(tigt::keyboard STATIC IMPORTED GLOBAL)
    set_target_properties(tigt::keyboard PROPERTIES
        IMPORTED_LOCATION "${TIGT_KEYBOARD_LIBRARY}"
        IMPORTED_LINK_INTERFACE_LANGUAGES C
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/keyboard/include"
        INTERFACE_LINK_LIBRARIES "${TIGT_KEYBOARD_NATIVE_LIBRARIES}")
    add_dependencies(tigt::keyboard tigt-keyboard-build)
    target_link_libraries(tigt PUBLIC tigt::keyboard)
endif()

option(TIGT_BUILD_EXAMPLES "Build standalone C consumer examples" ${PROJECT_IS_TOP_LEVEL})
if(TIGT_BUILD_EXAMPLES)
    add_executable(tigt-demo examples/demo.c)
    target_link_libraries(tigt-demo PRIVATE tigt::tigt)
    add_executable(tigt-snapshot examples/snapshot.c)
    target_link_libraries(tigt-snapshot PRIVATE tigt::tigt)
    if(TIGT_WITH_KEYBOARD)
        add_executable(tigt-keyboard examples/keyboard.c)
        target_link_libraries(tigt-keyboard PRIVATE tigt::tigt)
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS tigt EXPORT tigtTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/tigt.h include/tigt_terminal.h include/tigt_mouse.h include/tigt_video.h include/tigt_presenter.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(TIGT_WITH_KEYBOARD)
    install(FILES "${TIGT_KEYBOARD_LIBRARY}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES include/tigt_keyboard.h keyboard/include/pc_xt_keyboard.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES keyboard/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/tigt/pc-xt-keyboard)
endif()
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/tigt)
configure_package_config_file(cmake/tigtConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/tigtConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tigt
    PATH_VARS CMAKE_INSTALL_LIBDIR)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/tigtConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(EXPORT tigtTargets NAMESPACE tigt:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tigt)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/tigtConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/tigtConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tigt)

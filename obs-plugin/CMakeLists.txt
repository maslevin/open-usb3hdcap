# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
cmake_minimum_required(VERSION 3.20)

# Build the OBS source plugin (macOS, OBS 28+ .plugin bundle layout).
#
# Development build, against the installed OBS.app and Homebrew libusb:
#   cmake -S obs-plugin -B build -DLIBOBS_INCLUDE_DIR=<obs-studio>/libobs
# Distribution build (universal arm64 + x86_64, bundled libusb): see packaging/package-macos.sh
#   cmake -S obs-plugin -B build-dist -DUSB3HDCAP_DIST=ON -DLIBOBS_INCLUDE_DIR=... -DLIBUSB_BUNDLE_DIR=...
#
# Headers come from the obs-studio source tag matching the target OBS (fetch-libobs-headers.sh).

option(USB3HDCAP_DIST "Universal binary with bundled libusb, libobs resolved at load time" OFF)
if(USB3HDCAP_DIST)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)  # same minimum as current OBS
endif()

project(usb3hdcap-obs C)

set(USB3HDCAP_VERSION "0.1.0" CACHE STRING "Plugin version")
set(OBS_APP "/Applications/OBS.app" CACHE PATH "Installed OBS.app (development builds link its libobs)")
set(LIBOBS_INCLUDE_DIR "" CACHE PATH "obs-studio/libobs source directory (headers)")
set(LIBUSB_BUNDLE_DIR "" CACHE PATH "Universal libusb from packaging/build-libusb-universal.sh (dist builds)")
if(NOT LIBOBS_INCLUDE_DIR)
  message(FATAL_ERROR "Set LIBOBS_INCLUDE_DIR (run obs-plugin/fetch-libobs-headers.sh first)")
endif()

add_library(usb3hdcap MODULE
  hdcap-source.c
  ../lib/hdcap_usb.c
  ../lib/hdcap_demux.c
  ../lib/hdcap_control.c
  ../lib/hdcap_tables.c
  ../lib/hdcap_modes.c
  ../lib/hdcap_session.c
  ../lib/hdcap_resample.c
  ../lib/hdcap_deint.c)
# libobs headers need SIMDe (brew install simde)
find_path(SIMDE_INCLUDE_DIR simde/x86/sse2.h REQUIRED)
target_include_directories(usb3hdcap PRIVATE ../include "${LIBOBS_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated" "${SIMDE_INCLUDE_DIR}")
target_compile_features(usb3hdcap PRIVATE c_std_11)
target_compile_options(usb3hdcap PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(usb3hdcap PRIVATE USB3HDCAP_VERSION="${USB3HDCAP_VERSION}")

# libobs headers include obsconfig.h, which a normal OBS build generates.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/obsconfig.h"
     "#pragma once\n#define OBS_DATA_PATH \"\"\n#define OBS_PLUGIN_PATH \"\"\n#define OBS_PLUGIN_DESTINATION \"\"\n")

set(RES "$<TARGET_BUNDLE_CONTENT_DIR:usb3hdcap>/Resources")
if(USB3HDCAP_DIST)
  if(NOT LIBUSB_BUNDLE_DIR)
    message(FATAL_ERROR "Set LIBUSB_BUNDLE_DIR (run packaging/build-libusb-universal.sh first)")
  endif()
  target_include_directories(usb3hdcap PRIVATE "${LIBUSB_BUNDLE_DIR}/include/libusb-1.0")
  target_link_libraries(usb3hdcap PRIVATE "${LIBUSB_BUNDLE_DIR}/lib/libusb-1.0.0.dylib")
  # libobs symbols come from the hosting OBS process, whichever architecture it is.
  target_link_options(usb3hdcap PRIVATE -undefined dynamic_lookup)
  set_target_properties(usb3hdcap PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@loader_path/../Frameworks")
  add_custom_command(TARGET usb3hdcap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:usb3hdcap>/Frameworks"
    COMMAND ${CMAKE_COMMAND} -E copy "${LIBUSB_BUNDLE_DIR}/lib/libusb-1.0.0.dylib" "$<TARGET_BUNDLE_CONTENT_DIR:usb3hdcap>/Frameworks/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RES}/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy "${LIBUSB_BUNDLE_DIR}/COPYING.libusb" "${RES}/licenses/")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
  target_link_options(usb3hdcap PRIVATE "-F${OBS_APP}/Contents/Frameworks")
  target_link_libraries(usb3hdcap PRIVATE "-framework libobs" PkgConfig::LIBUSB)
  set_target_properties(usb3hdcap PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
endif()

set_target_properties(usb3hdcap PROPERTIES
  BUNDLE TRUE
  BUNDLE_EXTENSION plugin
  PREFIX ""
  MACOSX_BUNDLE_BUNDLE_NAME usb3hdcap
  MACOSX_BUNDLE_GUI_IDENTIFIER org.open-usb3hdcap.obs-plugin
  MACOSX_BUNDLE_BUNDLE_VERSION "${USB3HDCAP_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${USB3HDCAP_VERSION}")

# Bundle resources: locale (en-GB shares the en-US strings).
add_custom_command(TARGET usb3hdcap POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${RES}/locale"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini" "${RES}/locale/"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini" "${RES}/locale/en-GB.ini"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${RES}/licenses"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE" "${RES}/licenses/LICENSE.open-usb3hdcap"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/../NOTICE" "${RES}/licenses/NOTICE.open-usb3hdcap")

install(TARGETS usb3hdcap LIBRARY DESTINATION "$ENV{HOME}/Library/Application Support/obs-studio/plugins")

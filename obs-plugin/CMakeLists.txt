cmake_minimum_required(VERSION 3.20)
project(usb3hdcap-obs C)

# Build the OBS source plugin against an installed OBS.app (macOS, OBS 28+ layout).
#   cmake -S obs-plugin -B build -DOBS_APP=/Applications/OBS.app -DLIBOBS_INCLUDE_DIR=<obs-studio>/libobs
#   cmake --build build && cmake --install build
# Headers come from the obs-studio source tag matching the installed OBS (see fetch-libobs-headers.sh).

set(OBS_APP "/Applications/OBS.app" CACHE PATH "Installed OBS.app")
set(LIBOBS_INCLUDE_DIR "" CACHE PATH "obs-studio/libobs source directory (headers)")
if(NOT LIBOBS_INCLUDE_DIR)
  message(FATAL_ERROR "Set LIBOBS_INCLUDE_DIR (run obs-plugin/fetch-libobs-headers.sh first)")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

add_library(usb3hdcap MODULE
  hdcap-source.c
  ../lib/hdcap_usb.c
  ../lib/hdcap_demux.c
  ../lib/hdcap_control.c
  ../lib/hdcap_tables.c
  ../lib/hdcap_modes.c)
# libobs headers need SIMDe (brew install simde)
find_path(SIMDE_INCLUDE_DIR simde/x86/sse2.h REQUIRED)
target_include_directories(usb3hdcap PRIVATE ../include "${LIBOBS_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated" "${SIMDE_INCLUDE_DIR}")
target_compile_features(usb3hdcap PRIVATE c_std_11)
target_compile_options(usb3hdcap PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Link against the framework shipped inside OBS.app; resolve it at runtime from the host app.
target_link_options(usb3hdcap PRIVATE "-F${OBS_APP}/Contents/Frameworks")
target_link_libraries(usb3hdcap PRIVATE "-framework libobs" PkgConfig::LIBUSB)

# libobs headers include obsconfig.h, which a normal OBS build generates.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/obsconfig.h"
     "#pragma once\n#define OBS_DATA_PATH \"\"\n#define OBS_PLUGIN_PATH \"\"\n#define OBS_PLUGIN_DESTINATION \"\"\n")

set_target_properties(usb3hdcap PROPERTIES
  BUNDLE TRUE
  BUNDLE_EXTENSION plugin
  PREFIX ""
  MACOSX_BUNDLE_BUNDLE_NAME usb3hdcap
  MACOSX_BUNDLE_GUI_IDENTIFIER org.open-usb3hdcap.obs-plugin
  INSTALL_RPATH "@executable_path/../Frameworks")

# Bundle resources: locale (en-GB shares the en-US strings).
set(RES "$<TARGET_BUNDLE_CONTENT_DIR:usb3hdcap>/Resources")
add_custom_command(TARGET usb3hdcap POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${RES}/locale"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini" "${RES}/locale/"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini" "${RES}/locale/en-GB.ini")

install(TARGETS usb3hdcap LIBRARY DESTINATION "$ENV{HOME}/Library/Application Support/obs-studio/plugins")

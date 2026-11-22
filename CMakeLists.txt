cmake_minimum_required(VERSION 3.28)

project(obs-mac-voice-processing VERSION 1.0.0 LANGUAGES C)

if(NOT APPLE)
  message(FATAL_ERROR "obs-mac-voice-processing runs on macOS only")
endif()

find_package(libobs REQUIRED)

add_library(obs-mac-voice-processing MODULE)

target_sources(
  obs-mac-voice-processing
  PRIVATE src/audio-device-enum.c src/audio-device-enum.h src/plugin-main.c src/voice-processing.c
)

target_compile_features(obs-mac-voice-processing PRIVATE c_std_17)

target_link_libraries(
  obs-mac-voice-processing
  PRIVATE
    OBS::libobs
    "$<LINK_LIBRARY:FRAMEWORK,AudioUnit.framework>"
    "$<LINK_LIBRARY:FRAMEWORK,AudioToolbox.framework>"
    "$<LINK_LIBRARY:FRAMEWORK,CoreAudio.framework>"
    "$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>"
)

# The locale files, in the bundle's Resources, where OBS looks for a plugin's
# data.
file(GLOB locales "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/*.ini")
target_sources(obs-mac-voice-processing PRIVATE ${locales})
set_source_files_properties(${locales} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/locale)

# A plugin bundle, as OBS loads on macOS: libobs is OBS.app's own, found
# through the app's Frameworks.
set_target_properties(
  obs-mac-voice-processing
  PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION plugin
    PREFIX ""
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER moe.leak.obs-mac-voice-processing
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    INSTALL_RPATH "@executable_path/../Frameworks"
)

include(GNUInstallDirs)
install(TARGETS obs-mac-voice-processing LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")

cmake_minimum_required(VERSION 3.20)

# The version lives in src/engine/version.h and is read from there, so the
# binaries and the package metadata cannot drift apart.
file(STRINGS src/engine/version.h AC3SPDIF_VERSION_LINE REGEX "AC3SPDIF_VERSION \"")
string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1" AC3SPDIF_VERSION "${AC3SPDIF_VERSION_LINE}")
file(STRINGS src/engine/version.h AC3SPDIF_APP_ID_LINE REGEX "AC3SPDIF_APP_ID \"")
string(REGEX REPLACE ".*\"([A-Za-z0-9._-]+)\".*" "\\1" AC3SPDIF_APP_ID "${AC3SPDIF_APP_ID_LINE}")

project(ac3spdif VERSION ${AC3SPDIF_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BUILD_APP "Build the tray application (needs GTK 3 and libappindicator)" ON)
option(BUILD_TESTS "Build the unit tests" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswresample)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 glib-2.0)

add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)

# --- engine ------------------------------------------------------------------
file(GLOB ENGINE_SOURCES CONFIGURE_DEPENDS src/engine/*.cpp)
add_library(ac3engine STATIC ${ENGINE_SOURCES})
target_include_directories(ac3engine PUBLIC src)
target_link_libraries(ac3engine PUBLIC
  PkgConfig::PIPEWIRE PkgConfig::ALSA PkgConfig::FFMPEG PkgConfig::GIO pthread)
# The pod builders are C99 compound-literal macros. GCC accepts them in C++
# as an extension; keep the noise down.
target_compile_options(ac3engine PUBLIC -Wno-pedantic)

# --- command line tools ------------------------------------------------------
if(EXISTS ${CMAKE_SOURCE_DIR}/src/cli/main.cpp)
  add_executable(ac3spdif src/cli/main.cpp)
  target_link_libraries(ac3spdif PRIVATE ac3engine)
  install(TARGETS ac3spdif RUNTIME DESTINATION bin)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/probe/main.cpp)
  add_executable(ac3spdif-probe src/probe/main.cpp)
  target_link_libraries(ac3spdif-probe PRIVATE ac3engine)
  install(TARGETS ac3spdif-probe RUNTIME DESTINATION bin)
endif()

# --- tray application --------------------------------------------------------
if(BUILD_APP AND EXISTS ${CMAKE_SOURCE_DIR}/src/app/main.cpp)
  pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)
  # Fedora and Arch ship libappindicator; Debian and Ubuntu ship the Ayatana
  # fork. Same API, different header path and pkg-config name.
  pkg_check_modules(APPINDICATOR IMPORTED_TARGET ayatana-appindicator3-0.1)
  if(APPINDICATOR_FOUND)
    set(APPINDICATOR_DEFINE HAVE_AYATANA_APPINDICATOR)
  else()
    pkg_check_modules(APPINDICATOR IMPORTED_TARGET appindicator3-0.1)
    if(APPINDICATOR_FOUND)
      set(APPINDICATOR_DEFINE HAVE_APPINDICATOR)
    else()
      message(WARNING "No libappindicator found; the app will run without a tray icon")
      set(APPINDICATOR_DEFINE NO_APPINDICATOR)
    endif()
  endif()
  file(GLOB APP_SOURCES CONFIGURE_DEPENDS src/app/*.cpp)
  add_executable(ac3spdif-app ${APP_SOURCES})
  target_compile_definitions(ac3spdif-app PRIVATE ${APPINDICATOR_DEFINE}
    AC3SPDIF_DATADIR="${CMAKE_INSTALL_PREFIX}/share/ac3spdif"
    AC3SPDIF_SOURCE_DATADIR="${CMAKE_SOURCE_DIR}/data")
  target_link_libraries(ac3spdif-app PRIVATE ac3engine PkgConfig::GTK)
  if(APPINDICATOR_FOUND)
    target_link_libraries(ac3spdif-app PRIVATE PkgConfig::APPINDICATOR)
  endif()
  install(TARGETS ac3spdif-app RUNTIME DESTINATION bin)
  install(DIRECTORY data/icons/ DESTINATION share/ac3spdif/icons)
  install(FILES data/icons/ac3spdif.svg DESTINATION share/icons/hicolor/scalable/apps
    RENAME ${AC3SPDIF_APP_ID}.svg)
  install(FILES data/${AC3SPDIF_APP_ID}.desktop DESTINATION share/applications)
  install(FILES data/${AC3SPDIF_APP_ID}.metainfo.xml DESTINATION share/metainfo)
endif()

# --- tests -------------------------------------------------------------------
if(BUILD_TESTS)
  enable_testing()
  file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/test_*.cpp)
  foreach(source ${TEST_SOURCES})
    get_filename_component(name ${source} NAME_WE)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ac3engine)
    # The settings test exercises the app's settings module directly.
    if(name STREQUAL "test_settings")
      target_sources(${name} PRIVATE src/app/settings.cpp)
    endif()
    add_test(NAME ${name} COMMAND ${name})
  endforeach()
endif()

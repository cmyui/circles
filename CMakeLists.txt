cmake_minimum_required(VERSION 3.26)
# The oldest macOS circles runs on, so a build runs on more than the Mac it
# was made on. Set before project(), which reads it.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Oldest macOS to run on")
# OBJC for GLFW's Objective-C, which would otherwise build as Objective-C++.
project(circles LANGUAGES C CXX OBJC OBJCXX)
include(CTest)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Rendering uses Metal.
if(NOT APPLE)
  message(FATAL_ERROR "circles currently supports macOS only")
endif()

# Downloaded on the first configure into build/_deps, checked against their
# hashes, and kept there for later builds:
# - GLFW 3.4, built into the app, so it needs nothing installed;
# - liblzma's headers, from xz 5.8.4, for replays' (.osr) compression. The
#   library is macOS's own; xz's build isn't wanted, so SOURCE_SUBDIR points
#   where there's nothing to build.
include(FetchContent)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_BUILD_DOCS OFF)
set(GLFW_INSTALL OFF)
FetchContent_Declare(glfw
  URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
  URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9
  SYSTEM)
FetchContent_Declare(xz
  URL https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz
  URL_HASH SHA256=0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9
  SOURCE_SUBDIR src/liblzma/api)
FetchContent_MakeAvailable(glfw xz)

# Single-header libraries (miniaudio, stb), compiled once as C. Their headers
# are system includes so our warnings don't apply to them.
add_library(circles_third_party STATIC src/circles/third_party.c)
target_include_directories(circles_third_party SYSTEM PUBLIC third_party)
target_compile_definitions(circles_third_party PRIVATE MA_NO_RUNTIME_LINKING)
target_link_libraries(circles_third_party PUBLIC
  "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")

# Everything but main(), so tests can link the game's rules and file handling.
add_library(circles_core STATIC
  src/circles/app.cc
  src/circles/archive.cc
  src/circles/download_macos.mm
  src/circles/audio.cc
  src/circles/gameplay.cc
  src/circles/gfx.cc
  src/circles/gpu.mm
  src/circles/health.cc
  src/circles/hit_objects.cc
  src/circles/judging.cc
  src/circles/library.cc
  src/circles/md5.cc
  src/circles/menu.cc
  src/circles/mod_select.cc
  src/circles/mods.cc
  src/circles/profiler.cc
  src/circles/profiler_macos.mm
  src/circles/replay.cc
  src/circles/results.cc
  src/circles/settings_menu.cc
  src/circles/skin.cc
  src/circles/slider_body.cc
  src/circles/song_select.cc
  src/circles/stretch.cc
  src/circles/tablet.cc
  src/circles/tablet_macos.mm
  src/circles/window_macos.mm
)
target_include_directories(circles_core PUBLIC src)
set_source_files_properties(src/circles/gpu.mm src/circles/download_macos.mm
  PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_compile_options(circles_core PRIVATE -Wall -Wextra)
# osu!stable computes one floating-point operation at a time; fused
# multiply-adds would change its judgements' arithmetic.
target_compile_options(circles_core PUBLIC -ffp-contract=off)
target_include_directories(circles_core SYSTEM PRIVATE
  ${xz_SOURCE_DIR}/src/liblzma/api)
target_link_libraries(circles_core PUBLIC
  circles_third_party glfw lzma "-framework AppKit"
  "-framework Metal" "-framework QuartzCore" "-framework IOSurface"
  "-framework IOKit")

# An app bundle, so macOS knows circles is a game: Game Mode turns on while
# it's full screen.
# Its Resources hold the shaders, compiled when the game starts, and its
# icon.
add_executable(circles MACOSX_BUNDLE src/circles/main.cc shaders/circles.metal
  macos/circles.icns)
set_source_files_properties(shaders/circles.metal macos/circles.icns
  PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set_target_properties(circles PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/macos/Info.plist)
target_compile_options(circles PRIVATE -Wall -Wextra)
target_link_libraries(circles PRIVATE circles_core)
# Permissions macOS grants the app, such as Input Monitoring, last while its
# signature matches. An ad hoc signature changes with every build; a
# certificate's, such as -DCIRCLES_SIGN_IDENTITY="Apple Development", doesn't.
set(CIRCLES_SIGN_IDENTITY "-" CACHE STRING
  "codesign identity for circles.app; - signs ad hoc")
# Signed again whenever the app or a file in it changes: a resource changed
# alone is copied in without relinking, and must be covered too.
add_custom_command(OUTPUT circles.signed
  COMMAND codesign --force --sign "${CIRCLES_SIGN_IDENTITY}"
    "$<TARGET_BUNDLE_DIR:circles>"
  COMMAND ${CMAKE_COMMAND} -E touch circles.signed
  DEPENDS circles shaders/circles.metal macos/circles.icns macos/Info.plist)
add_custom_target(sign_circles ALL DEPENDS circles.signed)

if(BUILD_TESTING)
  set(_tests rules archive skin hitsounds md5 replay stretch mods hit_objects
  slider_body tablet judging)
  set(_test_targets)
  foreach(name IN LISTS _tests)
    list(APPEND _test_targets test_${name})
    add_executable(test_${name} EXCLUDE_FROM_ALL tests/test_${name}.cc)
    target_include_directories(test_${name} PRIVATE ${PROJECT_SOURCE_DIR})
    target_compile_options(test_${name} PRIVATE -Wall -Wextra -UNDEBUG)
    target_link_libraries(test_${name} PRIVATE circles_core)
    add_test(NAME ${name} COMMAND test_${name})
  endforeach()
  add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG> DEPENDS ${_test_targets} USES_TERMINAL)
endif()

cmake_minimum_required(VERSION 3.26)
project(circles LANGUAGES C CXX)
include(CTest)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Rendering uses Apple's OpenGL 4.1 headers without a function loader.
if(NOT APPLE)
  message(FATAL_ERROR "circles currently supports macOS only")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)

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
  src/circles/audio.cc
  src/circles/gameplay.cc
  src/circles/gfx.cc
  src/circles/library.cc
  src/circles/menu.cc
  src/circles/results.cc
  src/circles/skin.cc
  src/circles/song_select.cc
)
target_include_directories(circles_core PUBLIC src)
target_compile_definitions(circles_core PUBLIC GL_SILENCE_DEPRECATION)
target_compile_options(circles_core PRIVATE -Wall -Wextra)
target_link_libraries(circles_core PUBLIC
  circles_third_party PkgConfig::GLFW "-framework OpenGL")

add_executable(circles src/circles/main.cc)
target_compile_options(circles PRIVATE -Wall -Wextra)
target_link_libraries(circles PRIVATE circles_core)

if(BUILD_TESTING)
  set(_tests rules archive skin hitsounds)
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

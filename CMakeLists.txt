cmake_minimum_required(VERSION 3.24)
project(PDFEditZD VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PDFZ_BUILD_APP      "Build the Qt 6 application (needs Qt 6.4+ Widgets and PrintSupport)" ON)
option(PDFZ_BUILD_TESTS    "Build unit tests" ON)
option(PDFZ_SANITIZE       "Build with address + undefined behaviour sanitizers" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_library(pdfz_warnings INTERFACE)
target_compile_options(pdfz_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion>
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /utf-8>)

if(PDFZ_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Every program lands in <build>/bin, next to the PDFium library (copied there on Windows), so
# they run straight from the build folder.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Programs find libpdfium.so next to themselves once installed or deployed.
if(NOT WIN32)
  set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
endif()

include(cmake/PDFium.cmake)

add_subdirectory(src/core)
add_subdirectory(src/tools/pdfz-info)

if(PDFZ_BUILD_APP)
  find_package(Qt6 6.4 COMPONENTS Core Gui Widgets PrintSupport QUIET)
  if(Qt6_FOUND)
    add_subdirectory(src/app)
  else()
    message(WARNING "Qt 6.4+ (Widgets, PrintSupport) not found: skipping the app (the core library and tools still build)")
  endif()
endif()

if(PDFZ_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

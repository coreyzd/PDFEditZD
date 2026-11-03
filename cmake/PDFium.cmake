# SPDX-License-Identifier: GPL-3.0-only
# PDFium, the PDF engine: a prebuilt shared library from bblanchon/pdfium-binaries (Chromium's
# PDFium built standalone, no V8/XFA). Defines the imported target PDFium::pdfium and
# PDFZ_PDFIUM_RUNTIME (the .dll/.so to ship next to the program).
#
# By default the pinned release below is downloaded once into the build folder and checked
# against its SHA-256. To use a copy you already have (offline builds, another release), unpack
# one of those archives and pass its folder:  -DPDFZ_PDFIUM_DIR=C:/libs/pdfium-win-x64
#
# Windows (MSYS2 UCRT64, MinGW-w64): PDFium is a C API, so the MSVC-built DLL links fine from
# GCC through its import library.

set(PDFZ_PDFIUM_RELEASE "chromium/8125" CACHE STRING "pdfium-binaries release tag to download")
set(PDFZ_PDFIUM_DIR "" CACHE PATH "An unpacked pdfium-binaries archive to use instead of downloading")

# SHA-256 of each archive in the pinned release. Changing PDFZ_PDFIUM_RELEASE needs new ones.
set(_pdfium_sha256_pdfium-win-x64   9a964261760cff52570aabf038ab346d3a5034dd8b30d0d5fe8861235d356b8b)
set(_pdfium_sha256_pdfium-linux-x64 e1c3ce6e270cb5e0c20816b379ad9f2c1309972697c505f508de92bf5e7d6659)

if(PDFZ_PDFIUM_DIR)
  set(_pdfium_root "${PDFZ_PDFIUM_DIR}")
else()
  if(WIN32)
    set(_pdfium_pkg pdfium-win-x64)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(_pdfium_pkg pdfium-linux-x64)
  else()
    message(FATAL_ERROR "No pinned PDFium download for ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}. "
                        "Download the matching archive from https://github.com/bblanchon/pdfium-binaries "
                        "and pass -DPDFZ_PDFIUM_DIR=<its folder>.")
  endif()
  string(REPLACE "/" "-" _pdfium_tag_dir "${PDFZ_PDFIUM_RELEASE}")
  set(_pdfium_root "${CMAKE_BINARY_DIR}/_deps/${_pdfium_pkg}-${_pdfium_tag_dir}")
  if(NOT EXISTS "${_pdfium_root}/include/fpdfview.h")
    set(_url "https://github.com/bblanchon/pdfium-binaries/releases/download/${PDFZ_PDFIUM_RELEASE}/${_pdfium_pkg}.tgz")
    set(_archive "${CMAKE_BINARY_DIR}/_deps/${_pdfium_pkg}-${_pdfium_tag_dir}.tgz")
    set(_hash_args)
    if(PDFZ_PDFIUM_RELEASE STREQUAL "chromium/8125")
      set(_hash_args EXPECTED_HASH "SHA256=${_pdfium_sha256_${_pdfium_pkg}}")
    else()
      message(WARNING "PDFium ${PDFZ_PDFIUM_RELEASE}: no pinned SHA-256, the download isn't verified")
    endif()
    message(STATUS "Downloading PDFium: ${_url}")
    file(DOWNLOAD "${_url}" "${_archive}" ${_hash_args} STATUS _st TLS_VERIFY ON)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      list(GET _st 1 _msg)
      message(FATAL_ERROR "PDFium download failed (${_msg}). Download ${_url} yourself, unpack it and "
                          "pass -DPDFZ_PDFIUM_DIR=<that folder>.")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_pdfium_root}")
    file(REMOVE "${_archive}")
  endif()
endif()

if(NOT EXISTS "${_pdfium_root}/include/fpdfview.h")
  message(FATAL_ERROR "PDFium: no include/fpdfview.h in ${_pdfium_root}")
endif()

# "MAJOR=158 MINOR=0 BUILD=8125 PATCH=0" -> 158.0.8125.0
set(PDFZ_PDFIUM_VERSION "unknown")
if(EXISTS "${_pdfium_root}/VERSION")
  file(STRINGS "${_pdfium_root}/VERSION" _ver_lines)
  set(_parts)
  foreach(key MAJOR MINOR BUILD PATCH)
    foreach(line IN LISTS _ver_lines)
      if(line MATCHES "^${key}=([0-9]+)")
        list(APPEND _parts "${CMAKE_MATCH_1}")
      endif()
    endforeach()
  endforeach()
  list(JOIN _parts "." PDFZ_PDFIUM_VERSION)
endif()

add_library(PDFium::pdfium SHARED IMPORTED GLOBAL)
set_target_properties(PDFium::pdfium PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_pdfium_root}/include")
if(WIN32)
  set(PDFZ_PDFIUM_RUNTIME "${_pdfium_root}/bin/pdfium.dll")
  set_target_properties(PDFium::pdfium PROPERTIES
    IMPORTED_LOCATION "${PDFZ_PDFIUM_RUNTIME}"
    IMPORTED_IMPLIB "${_pdfium_root}/lib/pdfium.dll.lib")
else()
  set(PDFZ_PDFIUM_RUNTIME "${_pdfium_root}/lib/libpdfium.so")
  set_target_properties(PDFium::pdfium PROPERTIES
    IMPORTED_LOCATION "${PDFZ_PDFIUM_RUNTIME}"
    IMPORTED_NO_SONAME ON)  # the .so has no SONAME; link it by path
endif()
set(PDFZ_PDFIUM_LICENSES "${_pdfium_root}/LICENSE;${_pdfium_root}/licenses")
message(STATUS "PDFium ${PDFZ_PDFIUM_VERSION}: ${_pdfium_root}")

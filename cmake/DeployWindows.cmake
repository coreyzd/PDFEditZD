# SPDX-License-Identifier: GPL-3.0-only
# Builds a self-contained PDFEditZD folder on Windows: the app, pdfz-info, every DLL they load
# (PDFium, Qt, the MinGW runtime and the libraries Qt itself uses) and the Qt plugins (Windows
# platform and style, image formats, print support). Run through the `deploy` target
# (src/app/CMakeLists.txt):
#
#   cmake --build build-win --target deploy      ->  build-win/deploy/PDFEditZD/
#
# The folder runs from Explorer, or copied or zipped onto a PC without MSYS2. Nothing is
# installed or registered: no services, no updater, delete the folder to uninstall.
#
# Inputs (-D): OUT (the folder to make), EXES (;-list of programs), PLUGINS_DIR (Qt's
# plugins folder), DLL_DIRS (;-list of folders the DLLs come from), OBJDUMP, EXTRA_FILES
# (;-list copied next to the programs, e.g. LICENSE), LICENSE_DIRS (;-list of third-party
# licence files or folders, copied into licenses/).
cmake_minimum_required(VERSION 3.21)
# Newer CMake: normalize paths (C:\WINDOWS\system32 -> C:/WINDOWS/system32) before the
# exclude patterns below; they accept either separator, so this only quiets the warning.
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()

foreach(v OUT EXES PLUGINS_DIR DLL_DIRS)
  if(NOT ${v})
    message(FATAL_ERROR "DeployWindows.cmake: ${v} is not set")
  endif()
endforeach()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

# The programs.
foreach(exe IN LISTS EXES)
  file(COPY "${exe}" DESTINATION "${OUT}")
endforeach()
foreach(f IN LISTS EXTRA_FILES)
  if(EXISTS "${f}")
    file(COPY "${f}" DESTINATION "${OUT}")
  endif()
endforeach()
foreach(f IN LISTS LICENSE_DIRS)
  if(EXISTS "${f}")
    file(COPY "${f}" DESTINATION "${OUT}/licenses/pdfium")
  endif()
endforeach()

# Qt plugins: only the kinds the app uses. Each category folder is copied whole (they're
# small).
set(_plugin_dirs platforms styles imageformats iconengines printsupport)
set(_plugins)
foreach(cat IN LISTS _plugin_dirs)
  if(NOT IS_DIRECTORY "${PLUGINS_DIR}/${cat}")
    continue()
  endif()
  file(GLOB _dlls "${PLUGINS_DIR}/${cat}/*.dll")
  foreach(dll IN LISTS _dlls)
    file(COPY "${dll}" DESTINATION "${OUT}/plugins/${cat}")
    list(APPEND _plugins "${dll}")
  endforeach()
endforeach()
if(NOT EXISTS "${OUT}/plugins/platforms/qwindows.dll")
  message(FATAL_ERROR "DeployWindows.cmake: no platforms/qwindows.dll in ${PLUGINS_DIR}: PDFEditZD wouldn't start")
endif()

# Qt finds its plugins next to the program, not in the MSYS2 folder it was built for.
file(WRITE "${OUT}/qt.conf" "[Paths]\nPrefix = .\nPlugins = plugins\n")

# Every DLL the programs and plugins load, found in DLL_DIRS (Windows' own stay out).
if(OBJDUMP)
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${OBJDUMP}")
endif()
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES ${EXES}
  MODULES ${_plugins}
  DIRECTORIES ${DLL_DIRS}
  PRE_EXCLUDE_REGEXES "^[Aa][Pp][Ii]-[Mm][Ss]-.*" "^[Ee][Xx][Tt]-[Mm][Ss]-.*"
  POST_EXCLUDE_REGEXES ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*"
  RESOLVED_DEPENDENCIES_VAR _resolved
  UNRESOLVED_DEPENDENCIES_VAR _unresolved
  CONFLICTING_DEPENDENCIES_PREFIX _conflict)

set(_count 0)
foreach(dll IN LISTS _resolved)
  file(COPY "${dll}" DESTINATION "${OUT}")
  math(EXPR _count "${_count} + 1")
endforeach()
foreach(name IN LISTS _conflict_FILENAMES)
  # The same DLL name in two DLL_DIRS: the first folder's copy wins.
  list(GET _conflict_${name} 0 first)
  file(COPY "${first}" DESTINATION "${OUT}")
  math(EXPR _count "${_count} + 1")
endforeach()

# Windows' own DLLs (KERNEL32, USER32, GDI32...) are expected to stay unresolved here.
set(_missing)
foreach(name IN LISTS _unresolved)
  string(TOLOWER "${name}" lower)
  set(_found FALSE)
  foreach(dir IN ITEMS "$ENV{SystemRoot}/System32" "C:/Windows/System32")
    if(EXISTS "${dir}/${name}" OR EXISTS "${dir}/${lower}")
      set(_found TRUE)
    endif()
  endforeach()
  if(NOT _found)
    list(APPEND _missing "${name}")
  endif()
endforeach()
if(_missing)
  message(WARNING "DeployWindows.cmake: not found anywhere: ${_missing}")
endif()
if(NOT EXISTS "${OUT}/pdfium.dll")
  message(FATAL_ERROR "DeployWindows.cmake: pdfium.dll wasn't copied (is it in DLL_DIRS?)")
endif()

list(LENGTH _plugins _pluginCount)
message(STATUS "PDFEditZD deployed to ${OUT}: ${_count} DLL(s), ${_pluginCount} Qt plugin(s)")

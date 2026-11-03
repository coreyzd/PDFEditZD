add_library(pdfz_core STATIC
  engine.cpp
  document.cpp
)
target_include_directories(pdfz_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pdfz_core PUBLIC PDFium::pdfium PRIVATE pdfz_warnings)
target_compile_definitions(pdfz_core PRIVATE PDFZ_PDFIUM_VERSION="${PDFZ_PDFIUM_VERSION}")
# PDFium's headers aren't ours to fix.
get_target_property(_pdfium_inc PDFium::pdfium INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(pdfz_core SYSTEM PUBLIC ${_pdfium_inc})

# Windows: put pdfium.dll next to the programs in <build>/bin, so they run from there.
if(WIN32)
  set(_pdfium_dll_copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/pdfium.dll")
  add_custom_command(OUTPUT "${_pdfium_dll_copy}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PDFZ_PDFIUM_RUNTIME}" "${_pdfium_dll_copy}"
    DEPENDS "${PDFZ_PDFIUM_RUNTIME}" VERBATIM)
  add_custom_target(pdfz_pdfium_dll DEPENDS "${_pdfium_dll_copy}")
  add_dependencies(pdfz_core pdfz_pdfium_dll)
endif()

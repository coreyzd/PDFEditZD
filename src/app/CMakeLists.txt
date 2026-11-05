set(CMAKE_AUTOMOC ON)

# Qt SVG reads SVG files for stamps and signatures (MSYS2: mingw-w64-ucrt-x86_64-qt6-svg).
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets PrintSupport Svg)

add_executable(pdfeditzd
  main.cpp
  MainWindow.cpp
  DocumentView.cpp
  PageRenderer.cpp
  ThumbnailPanel.cpp
  Printing.cpp
  PrintBackend.cpp
  OfficeImport.cpp
  WinPrint.cpp
  TextSearch.cpp
  FindBar.cpp
  DocumentTab.cpp
  InlineTextEdit.cpp
  Markup.cpp
  Forms.cpp
  Signatures.cpp
  Stamps.cpp
  Fonts.cpp
  ContentEdit.cpp
  RichText.cpp
  VectorArt.cpp
  SignatureDialog.cpp
  CommentDialog.cpp
  CommentsPanel.cpp
  ToolIcons.cpp
)
target_compile_definitions(pdfeditzd PRIVATE PDFZ_VERSION="${PROJECT_VERSION}")
if(WIN32)
  set_target_properties(pdfeditzd PROPERTIES WIN32_EXECUTABLE ON)  # a GUI program: no console window
  if(MSVC)
    # MSVC's CRT reads char strings in the ANSI code page; the manifest makes that UTF-8.
    # (MinGW links a default manifest of its own and pdfz passes paths as UTF-16 anyway.)
    target_sources(pdfeditzd PRIVATE pdfeditzd.manifest)
  endif()
endif()

target_link_libraries(pdfeditzd PRIVATE pdfz_core pdfz_warnings
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::PrintSupport Qt6::Svg)
if(WIN32)
  target_link_libraries(pdfeditzd PRIVATE winspool gdi32)  # WinPrint: printing through the Windows API
endif()

install(TARGETS pdfeditzd)
if(NOT WIN32)
  install(FILES "${PDFZ_PDFIUM_RUNTIME}" TYPE LIB)
endif()

# Windows: `cmake --build <build dir> --target deploy` makes <build dir>/deploy/PDFEditZD, a
# folder with pdfeditzd.exe, pdfium.dll, every other DLL it needs and Qt's plugins, which runs
# from Explorer or on a PC without MSYS2 (cmake/DeployWindows.cmake).
if(WIN32)
  get_filename_component(_ccbin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(_pdfium_bin "${PDFZ_PDFIUM_RUNTIME}" DIRECTORY)
  set(_objdump "${CMAKE_OBJDUMP}")
  if(NOT _objdump)
    find_program(_objdump NAMES objdump HINTS "${_ccbin}")
  endif()
  add_custom_target(deploy
    COMMAND "${CMAKE_COMMAND}"
            "-DOUT=${CMAKE_BINARY_DIR}/deploy/PDFEditZD"
            "-DEXES=$<TARGET_FILE:pdfeditzd>$<SEMICOLON>$<TARGET_FILE:pdfz-info>"
            "-DPLUGINS_DIR=${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}"
            "-DDLL_DIRS=${_pdfium_bin}$<SEMICOLON>${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}$<SEMICOLON>${_ccbin}"
            "-DOBJDUMP=${_objdump}"
            "-DEXTRA_FILES=${PROJECT_SOURCE_DIR}/LICENSE"
            "-DLICENSE_DIRS=${PDFZ_PDFIUM_LICENSES}"
            -P "${PROJECT_SOURCE_DIR}/cmake/DeployWindows.cmake"
    DEPENDS pdfeditzd pdfz-info
    COMMENT "Making a self-contained PDFEditZD folder in ${CMAKE_BINARY_DIR}/deploy/PDFEditZD"
    VERBATIM)
endif()

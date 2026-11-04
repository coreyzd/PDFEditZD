add_executable(pdfz_tests
  main.cpp
  test_document.cpp
  test_history.cpp
  test_text.cpp
  test_annotations.cpp
  test_forms.cpp
)
target_include_directories(pdfz_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(pdfz_tests PRIVATE pdfz_core pdfz_warnings)
add_test(NAME pdfz_tests COMMAND pdfz_tests)

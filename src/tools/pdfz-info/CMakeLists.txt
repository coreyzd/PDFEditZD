add_executable(pdfz-info main.cpp)
target_link_libraries(pdfz-info PRIVATE pdfz_core pdfz_warnings)
install(TARGETS pdfz-info)

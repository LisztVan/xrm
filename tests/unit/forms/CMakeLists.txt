add_executable(xrm_forms_tests forms_tests.cpp)
target_link_libraries(xrm_forms_tests PRIVATE xrm_logic Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(xrm_forms_tests)

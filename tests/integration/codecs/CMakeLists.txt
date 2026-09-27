add_executable(xrm_m4_tests m4_tests.cpp)
target_link_libraries(xrm_m4_tests PRIVATE xrm_server_lib Catch2::Catch2WithMain)
target_compile_definitions(xrm_m4_tests PRIVATE
  XRM_M4_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/codecs")
include(Catch)
catch_discover_tests(xrm_m4_tests)

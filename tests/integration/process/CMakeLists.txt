add_executable(xrm_process_tests process_tests.cpp)
target_link_libraries(xrm_process_tests PRIVATE xrm_server_lib Catch2::Catch2WithMain)
target_compile_definitions(xrm_process_tests PRIVATE
  XRM_TEST_FAKE_CORE="$<TARGET_FILE:xrm_fake_core>")
add_dependencies(xrm_process_tests xrm_fake_core)
catch_discover_tests(xrm_process_tests)

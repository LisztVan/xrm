add_executable(xrm_m0_tests m0_tests.cpp)
target_link_libraries(xrm_m0_tests PRIVATE xrm_server_lib Catch2::Catch2WithMain)
target_compile_definitions(xrm_m0_tests PRIVATE
  XRM_TEST_MANAGER="$<TARGET_FILE:xrm>"
  XRM_TEST_SERVER="$<TARGET_FILE:xrmd>"
  XRM_TEST_FAKE_CORE="$<TARGET_FILE:xrm_fake_core>")
add_dependencies(xrm_m0_tests xrm xrmd xrm_fake_core)
catch_discover_tests(xrm_m0_tests)

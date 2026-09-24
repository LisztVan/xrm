add_executable(xrm_tui_acceptance_tests acceptance_tests.cpp)
target_link_libraries(xrm_tui_acceptance_tests PRIVATE
  xrm_model xrm_logic xrm_server_lib Catch2::Catch2WithMain util)
target_compile_definitions(xrm_tui_acceptance_tests PRIVATE
  XRM_TEST_MANAGER="$<TARGET_FILE:xrm>"
  XRM_TEST_SERVER="$<TARGET_FILE:xrmd>"
  XRM_TEST_FAKE_CORE="$<TARGET_FILE:xrm_fake_core>")
add_dependencies(xrm_tui_acceptance_tests xrm xrmd xrm_fake_core)
include(Catch)
catch_discover_tests(xrm_tui_acceptance_tests)


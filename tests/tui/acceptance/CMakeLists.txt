add_executable(xrm_tui_acceptance_tests acceptance_tests.cpp)
target_link_libraries(xrm_tui_acceptance_tests PRIVATE
  xrm_model xrm_logic xrm_server_lib Catch2::Catch2WithMain ftxui::screen util)
target_compile_definitions(xrm_tui_acceptance_tests PRIVATE
  XRM_TEST_MANAGER="$<TARGET_FILE:xrm>"
  XRM_TEST_SERVER="$<TARGET_FILE:xrmd>"
  XRM_TEST_FAKE_CORE="$<TARGET_FILE:xrm_fake_core>"
  XRM_TEST_FAKE_EDITOR="$<TARGET_FILE:xrm_fake_editor>")
add_dependencies(xrm_tui_acceptance_tests xrm xrmd xrm_fake_core xrm_fake_editor)
include(Catch)
catch_discover_tests(xrm_tui_acceptance_tests TEST_SPEC "~[port10808]")
# This fixture deliberately binds the product's default listener port; keep it
# apart from real-core cases that may also exercise that port.
catch_discover_tests(xrm_tui_acceptance_tests TEST_SPEC "[port10808]"
  PROPERTIES RUN_SERIAL TRUE)

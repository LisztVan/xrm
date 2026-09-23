add_executable(xrm_m2_tests m2_tests.cpp)
target_link_libraries(xrm_m2_tests PRIVATE xrm_model Catch2::Catch2WithMain util)
target_compile_definitions(xrm_m2_tests PRIVATE
  XRM_TEST_MANAGER="$<TARGET_FILE:xrm>"
  XRM_TEST_SERVER="$<TARGET_FILE:xrmd>"
  XRM_TEST_FAKE_CORE="$<TARGET_FILE:xrm_fake_core>")
add_dependencies(xrm_m2_tests xrm xrmd xrm_fake_core)
include(Catch)
catch_discover_tests(xrm_m2_tests)

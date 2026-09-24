add_executable(xrm_packaging_tests packaging_tests.cpp)
target_link_libraries(xrm_packaging_tests PRIVATE xrm_model Catch2::Catch2WithMain)
target_compile_definitions(xrm_packaging_tests PRIVATE
  XRM_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  XRM_TEST_MANAGER="$<TARGET_FILE:xrm>"
  XRM_TEST_SERVER="$<TARGET_FILE:xrmd>"
  XRM_TEST_FAKE_CORE="$<TARGET_FILE:xrm_fake_core>")
add_dependencies(xrm_packaging_tests xrm xrmd xrm_fake_core)
include(Catch)
catch_discover_tests(xrm_packaging_tests PROPERTIES LABELS "integration;packaging")

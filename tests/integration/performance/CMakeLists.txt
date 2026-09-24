add_executable(xrm_performance_tests performance_tests.cpp)
target_link_libraries(xrm_performance_tests PRIVATE xrm_model xrm_logic xrm_server_lib Catch2::Catch2WithMain)
target_compile_definitions(xrm_performance_tests PRIVATE
  XRM_PERF_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  XRM_TEST_MANAGER="$<TARGET_FILE:xrm>"
  XRM_TEST_SERVER="$<TARGET_FILE:xrmd>"
  XRM_TEST_FAKE_CORE="$<TARGET_FILE:xrm_fake_core>")
if(XRM_ENABLE_ASAN)
  target_compile_definitions(xrm_performance_tests PRIVATE XRM_ASAN_BUILD=1)
endif()
add_dependencies(xrm_performance_tests xrm xrmd xrm_fake_core)
include(Catch)
catch_discover_tests(xrm_performance_tests)

add_executable(xrm_performance_tests performance_tests.cpp)
target_link_libraries(xrm_performance_tests PRIVATE xrm_model xrm_logic xrm_server_lib Catch2::Catch2WithMain)
target_compile_definitions(xrm_performance_tests PRIVATE
  XRM_PERF_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  XRM_TEST_MANAGER="$<TARGET_FILE:xrm>"
  XRM_TEST_SERVER="$<TARGET_FILE:xrmd>")
add_dependencies(xrm_performance_tests xrm xrmd)
include(Catch)
catch_discover_tests(xrm_performance_tests)

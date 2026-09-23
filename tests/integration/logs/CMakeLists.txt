add_executable(xrm_log_tests log_tests.cpp)
target_link_libraries(xrm_log_tests PRIVATE xrm_server_lib Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(xrm_log_tests)

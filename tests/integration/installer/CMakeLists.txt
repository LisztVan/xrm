add_executable(xrm_installer_tests installer_tests.cpp)
target_compile_definitions(xrm_installer_tests PRIVATE
  XRM_RELEASE_FIXTURE_ROOT="${CMAKE_SOURCE_DIR}/tests/fixtures/releases")
target_link_libraries(xrm_installer_tests PRIVATE xrm_server_lib Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(xrm_installer_tests)

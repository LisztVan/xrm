add_executable(xrm_release_e2e_tests e2e06.cpp)
target_link_libraries(xrm_release_e2e_tests PRIVATE xrm_server_lib xrm_logic Catch2::Catch2)
target_compile_definitions(xrm_release_e2e_tests PRIVATE
  XRM_TEST_MANAGER="$<TARGET_FILE:xrm>"
  XRM_TEST_SERVER="$<TARGET_FILE:xrmd>")
add_dependencies(xrm_release_e2e_tests xrm xrmd)

add_test(NAME E2E-06 COMMAND xrm_release_e2e_tests "[E2E-06]")
set_tests_properties(E2E-06 PROPERTIES LABELS "real_core" SKIP_RETURN_CODE 77)

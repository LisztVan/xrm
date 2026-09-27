find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(cli_rewrite_args
  --xrm $<TARGET_FILE:xrm>
  --xrmd $<TARGET_FILE:xrmd>)
if(XRM_TEST_XRAY)
  list(APPEND cli_rewrite_args --xray ${XRM_TEST_XRAY})
endif()
add_test(NAME cli_rewrite_r10_r11
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_rewrite_test.py ${cli_rewrite_args})
set_tests_properties(cli_rewrite_r10_r11 PROPERTIES TIMEOUT 60 LABELS "cli;integration")
if(XRM_TEST_XRAY)
  add_test(NAME cli_usability_repair
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/usability_repair_test.py
      --xrm $<TARGET_FILE:xrm> --xrmd $<TARGET_FILE:xrmd>
      --xray ${XRM_TEST_XRAY}
      --evidence ${CMAKE_CURRENT_BINARY_DIR}/usability-repair-evidence)
  set_tests_properties(cli_usability_repair PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
    LABELS "cli;integration;usability")
  add_test(NAME cli_core_error_stages
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core_error_stage_test.py
      --build ${CMAKE_BINARY_DIR} --xray ${XRM_TEST_XRAY}
      --evidence ${CMAKE_CURRENT_BINARY_DIR}/core-error-stage-evidence)
  set_tests_properties(cli_core_error_stages PROPERTIES TIMEOUT 60
    LABELS "cli;integration;usability")
endif()
add_executable(xrm_human_message_tests human_message_tests.cpp)
target_link_libraries(xrm_human_message_tests PRIVATE xrm_model Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(xrm_human_message_tests)

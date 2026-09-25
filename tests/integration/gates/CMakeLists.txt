add_executable(xrm_real_core_tests m1.cpp rc04.cpp rc05.cpp rc07.cpp rc08.cpp rc09.cpp rc10_m4.cpp)
target_link_libraries(xrm_real_core_tests PRIVATE xrm_server_lib xrm_logic Catch2::Catch2)
target_compile_definitions(xrm_real_core_tests PRIVATE
  XRM_TEST_MANAGER="$<TARGET_FILE:xrm>"
  XRM_TEST_SERVER="$<TARGET_FILE:xrmd>"
  XRM_REQUIRE_REAL_XRAY=$<BOOL:${XRM_REQUIRE_REAL_XRAY}>)
if(XRM_TEST_XRAY)
  target_compile_definitions(xrm_real_core_tests PRIVATE
    XRM_TEST_XRAY_CONFIGURED="${XRM_TEST_XRAY}")
endif()
add_dependencies(xrm_real_core_tests xrm xrmd)

foreach(case RC-01 RC-02 RC-03 RC-04 RC-05 RC-06 RC-07 RC-08 RC-09 RC-10)
  add_test(NAME ${case} COMMAND xrm_real_core_tests "[${case}]")
  set_tests_properties(${case} PROPERTIES
    LABELS "real_core"
    SKIP_RETURN_CODE 77)
endforeach()
add_test(NAME VLESS-WS-XHTTP COMMAND xrm_real_core_tests "[VLESS-WS-XHTTP]")
set_tests_properties(VLESS-WS-XHTTP PROPERTIES
  LABELS "real_core;product_repair"
  SKIP_RETURN_CODE 77)
add_test(NAME E2E-07 COMMAND xrm_real_core_tests "[E2E-07]")
# E2E-07 intentionally repairs an old Profile's fixed 127.0.0.1:10808
# listener, so it cannot share that port with another running case.
set_tests_properties(E2E-07 PROPERTIES LABELS "real_core;product_repair"
  SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME GROUP-TUI-E2E COMMAND xrm_real_core_tests "[GROUP-TUI-E2E]")
set_tests_properties(GROUP-TUI-E2E PROPERTIES LABELS "real_core;integration"
  SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

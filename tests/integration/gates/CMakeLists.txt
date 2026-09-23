add_executable(xrm_real_core_tests m1.cpp rc04.cpp rc05.cpp)
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

foreach(case RC-01 RC-02 RC-03 RC-04 RC-05 RC-06 RC-10)
  add_test(NAME ${case} COMMAND xrm_real_core_tests "[${case}]")
  set_tests_properties(${case} PROPERTIES
    LABELS "real_core"
    SKIP_RETURN_CODE 77)
endforeach()

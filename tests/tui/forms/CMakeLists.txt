add_executable(xrm_tui_forms_tests native_form_tests.cpp ../../../src/tui/forms/native_form.cpp)
target_link_libraries(xrm_tui_forms_tests PRIVATE xrm_tui_components nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(xrm_tui_forms_tests)

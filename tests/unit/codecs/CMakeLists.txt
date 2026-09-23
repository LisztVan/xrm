add_executable(xrm_codec_tests vless.cpp native.cpp)
target_compile_definitions(xrm_codec_tests PRIVATE
  XRM_CODEC_FIXTURE_ROOT="${CMAKE_SOURCE_DIR}/tests/fixtures/codecs")
target_link_libraries(xrm_codec_tests PRIVATE xrm_logic Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(xrm_codec_tests)

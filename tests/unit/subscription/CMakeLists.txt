add_executable(xrm_subscription_tests
  parser_tests.cpp
  reconcile_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/subscription/parser.cpp
  ${PROJECT_SOURCE_DIR}/src/subscription/reconcile.cpp)
target_link_libraries(xrm_subscription_tests PRIVATE xrm_logic Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(xrm_subscription_tests)

add_executable(xrm_contract_tests
  contract/contract_tests.cpp
  contract/include_common.cpp
  contract/include_node.cpp
  contract/include_runtime.cpp
  contract/include_wire.cpp
  contract/include_dtos.cpp
  contract/include_store.cpp
  contract/include_process.cpp
  contract/include_rpc_client.cpp
  contract/include_rpc_server.cpp)
target_link_libraries(xrm_contract_tests PRIVATE xrm_contract Catch2::Catch2WithMain)
include(CTest)
include(Catch)
catch_discover_tests(xrm_contract_tests)

add_executable(xrm_fake_core support/fake_core.cpp)

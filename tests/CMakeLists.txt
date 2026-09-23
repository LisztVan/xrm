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
  contract/include_rpc_server.cpp
  contract/m1/include_profile.cpp
  contract/m1/include_catalog.cpp
  contract/m1/include_profile_service.cpp
  contract/m1/include_codec.cpp
  contract/m1/include_assembler.cpp
  contract/m1/include_writer.cpp
  contract/m1/include_core_manager.cpp
  contract/m1/include_process_adapter.cpp
  contract/m1/interfaces.cpp
  contract/m2/interfaces.cpp)
target_link_libraries(xrm_contract_tests PRIVATE xrm_contract Catch2::Catch2WithMain)
include(CTest)
include(Catch)
catch_discover_tests(xrm_contract_tests)

add_executable(xrm_fake_core support/fake_core.cpp)

add_subdirectory(integration/store)
add_subdirectory(integration/process)
add_subdirectory(integration/rpc_transport)
add_subdirectory(integration/m0)
add_subdirectory(integration/runtime_writer)
add_subdirectory(unit/codecs)
add_subdirectory(unit/modules)
add_subdirectory(integration/profile)
add_subdirectory(unit/assembly/basic)
add_subdirectory(integration/lifecycle/basic)
add_subdirectory(integration/m1)
add_subdirectory(integration/gates)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/http/CMakeLists.txt")
  add_subdirectory(integration/http)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/subscription/CMakeLists.txt")
  add_subdirectory(unit/subscription)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/subscription/CMakeLists.txt")
  add_subdirectory(integration/subscription)
endif()

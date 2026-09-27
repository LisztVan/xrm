include(Catch)

add_executable(xrm_fake_core support/fake_core.cpp)
add_executable(xrm_fake_editor support/fake_editor.cpp)

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
  contract/m2/interfaces.cpp
  contract/m3/interfaces.cpp
  contract/m4/interfaces.cpp
  contract/m4/include_node.cpp
  contract/m4/include_codec.cpp
  contract/m4/include_forms.cpp
  contract/m4/include_installer.cpp)
target_link_libraries(xrm_contract_tests PRIVATE xrm_contract Catch2::Catch2WithMain)
catch_discover_tests(xrm_contract_tests)

# Fast domain and component checks. The target graph, not this listing, orders
# compilation; this grouping makes the test tree easier to navigate.
add_subdirectory(unit/advanced_forms)
add_subdirectory(unit/assembly/basic)
add_subdirectory(unit/assembly/groups)
add_subdirectory(unit/codecs)
add_subdirectory(unit/forms)
add_subdirectory(unit/metrics)
add_subdirectory(unit/modules)
add_subdirectory(unit/node_tests)
add_subdirectory(unit/routing_presets)
add_subdirectory(unit/subscription)
add_subdirectory(tui/components)
add_subdirectory(tui/forms)

# Server, CLI, native runtime, and complete user workflows.
add_subdirectory(integration/bootstrap)
add_subdirectory(integration/codecs)
add_subdirectory(integration/control)
add_subdirectory(integration/geodata)
add_subdirectory(integration/group_bindings)
add_subdirectory(integration/http)
add_subdirectory(integration/installer)
add_subdirectory(integration/inventory)
add_subdirectory(integration/lifecycle/basic)
add_subdirectory(integration/lifecycle/advanced)
add_subdirectory(integration/logs)
add_subdirectory(integration/module_actions)
add_subdirectory(integration/packaging)
add_subdirectory(integration/performance)
add_subdirectory(integration/process)
add_subdirectory(integration/profile)
add_subdirectory(integration/real_core)
add_subdirectory(integration/release)
add_subdirectory(integration/rpc_transport)
add_subdirectory(integration/runtime_writer)
add_subdirectory(integration/store)
add_subdirectory(integration/subscription)
add_subdirectory(integration/workflows)

add_subdirectory(cli)
add_subdirectory(tui/acceptance)
add_subdirectory(tui/rewrite)

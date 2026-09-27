# The link graph is the build order: data/contract/domain, daemon/client,
# reusable TUI controls, frontends, then executables. Ninja may compile
# independent targets in parallel.

add_library(xrm_model STATIC
  src/model/json_models.cpp
  src/model/module_catalog.cpp)
target_include_directories(xrm_model PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(xrm_model PUBLIC nlohmann_json::nlohmann_json)

add_library(xrm_contract STATIC src/contract/wire.cpp)
target_include_directories(xrm_contract PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(xrm_contract PUBLIC xrm_model)

add_library(xrm_logic STATIC
  src/geodata/resource_validation.cpp
  src/profile/advanced_forms.cpp
  src/profile/forms.cpp
  src/profile/routing_presets.cpp
  src/subscription/parser.cpp
  src/subscription/reconcile.cpp
  src/xray/codecs/codec.cpp
  src/xray/codecs/hysteria.cpp
  src/xray/codecs/shadowsocks.cpp
  src/xray/codecs/stream.cpp
  src/xray/codecs/trojan.cpp
  src/xray/codecs/vless.cpp
  src/xray/codecs/vmess.cpp
  src/xray/runtime_assembler.cpp)
target_include_directories(xrm_logic PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(xrm_logic PUBLIC xrm_model)

add_library(xrm_server_lib STATIC
  src/geodata/cache.cpp
  src/infra/files.cpp
  src/infra/http.cpp
  src/infra/process.cpp
  src/infra/store.cpp
  src/profile/groups.cpp
  src/profile/module_actions.cpp
  src/profile/profile_service.cpp
  src/server/api_router.cpp
  src/server/application.cpp
  src/server/log_buffer.cpp
  src/server/node_test_service.cpp
  src/server/operations.cpp
  src/server/rpc_server.cpp
  src/subscription/subscription_service.cpp
  src/xray/control.cpp
  src/xray/core_manager.cpp
  src/xray/installer.cpp
  src/xray/metrics.cpp
  src/xray/process_adapter.cpp
  src/xray/runtime_writer.cpp)
target_include_directories(xrm_server_lib PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(xrm_server_lib PUBLIC
  xrm_model xrm_contract xrm_logic SQLite3::SQLite3 CURL::libcurl
  uv_a archive_static OpenSSL::Crypto)
target_compile_definitions(xrm_server_lib PRIVATE
  XRM_PRESETS_DIR="${PROJECT_SOURCE_DIR}/presets")

add_library(xrm_client_lib STATIC src/client/rpc_client.cpp)
target_include_directories(xrm_client_lib PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(xrm_client_lib PUBLIC xrm_contract uv_a)

add_library(xrm_tui_components STATIC
  src/tui/components/widgets.cpp
  src/tui/components/log_viewer.cpp)
target_include_directories(xrm_tui_components PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(xrm_tui_components PUBLIC
  ftxui::component ftxui::dom ftxui::screen)

add_library(xrm_tui_forms STATIC src/tui/forms/native_form.cpp)
target_link_libraries(xrm_tui_forms PUBLIC xrm_tui_components xrm_model)

add_library(xrm_tui_frontend STATIC
  src/tui/adapters/rpc.cpp
  src/tui/app.cpp
  src/tui/pages/config.cpp
  src/tui/pages/modules.cpp
  src/tui/pages/nodes.cpp
  src/tui/pages/profiles.cpp
  src/tui/shell/actions.cpp
  src/tui/shell/workspace.cpp)
target_link_libraries(xrm_tui_frontend PUBLIC
  xrm_tui_forms xrm_client_lib xrm_logic)

add_library(xrm_cli_frontend STATIC
  src/cli/commands.cpp
  src/cli/core_commands.cpp
  src/cli/geodata_commands.cpp
  src/cli/node_commands.cpp
  src/cli/print.cpp
  src/cli/profile_commands.cpp
  src/cli/routing_preset_commands.cpp
  src/cli/runtime_commands.cpp
  src/cli/settings_commands.cpp
  src/cli/subscription_commands.cpp)
target_include_directories(xrm_cli_frontend PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(xrm_cli_frontend PUBLIC
  xrm_client_lib xrm_logic CLI11::CLI11 ftxui::screen)
target_compile_definitions(xrm_cli_frontend PRIVATE XRM_VERSION="${PROJECT_VERSION}")

add_executable(xrmd apps/xrmd_main.cpp)
target_link_libraries(xrmd PRIVATE xrm_server_lib CLI11::CLI11)
target_compile_definitions(xrmd PRIVATE XRM_VERSION="${PROJECT_VERSION}")

add_executable(xrm apps/xrm_main.cpp)
target_link_libraries(xrm PRIVATE xrm_cli_frontend xrm_tui_frontend)

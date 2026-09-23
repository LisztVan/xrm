cmake_minimum_required(VERSION 3.28)
project(xrm VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)
include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(XRM_BUILD_TESTING "Build XRM tests" ON)
option(XRM_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(XRM_REQUIRE_REAL_XRAY "Require XRM_TEST_XRAY to name an executable Xray core" OFF)
set(XRM_TEST_XRAY "" CACHE FILEPATH "Optional real Xray binary for real-core tests")

if(XRM_ENABLE_ASAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  else()
    message(FATAL_ERROR "XRM_ENABLE_ASAN requires GCC or Clang")
  endif()
endif()

find_package(SQLite3 REQUIRED)

set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libuv
  URL https://github.com/libuv/libuv/archive/refs/tags/v1.51.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(libuv)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(CLI11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.5.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(CLI11)

set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ftxui_dep
  URL https://codeload.github.com/ArthurSonzogni/FTXUI/tar.gz/refs/tags/v7.0.1
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(ftxui_dep)

if(XRM_BUILD_TESTING)
  FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

add_library(xrm_model STATIC
  src/model/json_models.cpp
  src/model/module_catalog.cpp)
target_include_directories(xrm_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(xrm_model PUBLIC nlohmann_json::nlohmann_json)

add_library(xrm_logic STATIC
  src/xray/codecs/codec.cpp
  src/xray/codecs/stream.cpp
  src/xray/codecs/vless.cpp
  src/xray/runtime_assembler.cpp)
target_include_directories(xrm_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(xrm_logic PUBLIC xrm_model)

add_library(xrm_contract STATIC src/contract/wire.cpp)
target_include_directories(xrm_contract PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(xrm_contract PUBLIC xrm_model)

add_library(xrm_server_lib STATIC
  src/infra/process.cpp
  src/server/application.cpp
  src/server/api_router.cpp
  src/server/rpc_server.cpp
  src/infra/store.cpp
  src/infra/files.cpp
  src/server/log_buffer.cpp
  src/server/operations.cpp
  src/profile/profile_service.cpp
  src/xray/runtime_writer.cpp
  src/xray/core_manager.cpp
  src/xray/process_adapter.cpp)
target_include_directories(xrm_server_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(xrm_server_lib PUBLIC xrm_model xrm_contract xrm_logic SQLite3::SQLite3 uv_a)
target_compile_definitions(xrm_server_lib PRIVATE
  XRM_PRESETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/presets")

add_library(xrm_client_lib STATIC src/client/rpc_client.cpp)
target_include_directories(xrm_client_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(xrm_client_lib PUBLIC xrm_contract uv_a)

add_executable(xrmd apps/xrmd_main.cpp)
target_link_libraries(xrmd PRIVATE xrm_server_lib CLI11::CLI11)
target_compile_definitions(xrmd PRIVATE XRM_VERSION="${PROJECT_VERSION}")

add_executable(xrm
  apps/xrm_main.cpp
  src/cli/commands.cpp
  src/cli/profile_commands.cpp
  src/cli/runtime_commands.cpp
  src/cli/node_commands.cpp
  src/cli/core_commands.cpp
  src/cli/print.cpp
  src/tui/app.cpp)
target_include_directories(xrm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(xrm PRIVATE xrm_client_lib CLI11::CLI11 ftxui::component ftxui::dom ftxui::screen)

if(XRM_BUILD_TESTING)
  add_subdirectory(tests)
endif()

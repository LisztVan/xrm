#pragma once

#include "model/common.hpp"

#include <CLI/CLI.hpp>

namespace xrm::cli {

struct Request {
  std::string socket;
  bool json = false;
  std::string method;
  Json params = Json::object();
  std::string node_name;
  std::string node_outbound;
  std::string node_search;
  std::size_t node_offset = 0;
  std::size_t node_limit = 100;
  std::string core_path;
  std::string core_name;
  std::string core_id;
  std::string profile_name;
  std::string profile_id;
  std::string module_key;
  std::string module_text;
  std::string source = "source";
  std::string output_path;
  std::string target_id;
  std::string operation_id;
  std::size_t log_cursor = 0;
  std::size_t log_limit = 65536;
  bool included = true;
};

void add_node_commands(CLI::App& app, Request& request);
void add_core_commands(CLI::App& app, Request& request);
void add_profile_commands(CLI::App& app, Request& request);
void add_runtime_commands(CLI::App& app, Request& request);
int execute_request(const Request& request);
int print_result(const Request& request, const Result<Json>& result);
std::string default_socket_path();

}  // namespace xrm::cli

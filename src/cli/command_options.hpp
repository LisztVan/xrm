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
  std::string subscription_id;
  std::string subscription_name;
  std::string subscription_url;
  std::string subscription_user_agent;
  std::string subscription_proxy;
  std::vector<std::string> subscription_headers;
  bool subscription_paused = false;
  bool subscription_resume = false;
  std::string node_id;
  std::string node_alias;
  std::string node_labels;
  bool keep_nodes_manual = false;
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
void add_subscription_commands(CLI::App& app, Request& request);
void add_core_commands(CLI::App& app, Request& request);
void add_profile_commands(CLI::App& app, Request& request);
void add_runtime_commands(CLI::App& app, Request& request);
int execute_request(const Request& request);
int print_result(const Request& request, const Result<Json>& result);
std::string default_socket_path();

}  // namespace xrm::cli

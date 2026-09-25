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
  std::vector<std::string> history_node_ids;
  std::string history_token;
  bool history_confirmed = false;
  bool keep_nodes_manual = false;
  std::size_t node_offset = 0;
  std::size_t node_limit = 100;
  std::string core_path;
  std::string core_name;
  std::string core_id;
  std::string core_version;
  std::string core_asset;
  bool core_include_prerelease = false;
  bool core_allow_unverified_checksum = false;
  std::string profile_name;
  std::string profile_id;
  std::string module_key;
  std::string module_show_key;
  std::string module_text;
  std::string metrics_listen;
  std::string form_key;
  std::string form_value;
  std::string form_min;
  std::string form_max;
  std::string form_cron;
  std::string form_json;
  std::vector<std::string> form_values;
  std::string source = "source";
  std::string output_path;
  std::string target_id;
  std::string group_id;
  std::string group_name;
  std::string group_tag;
  std::string group_mode = "automatic";
  std::string group_strategy = "leastPing";
  std::vector<std::string> group_members;
  std::string group_json;
  std::string override_balancer;
  std::string override_outbound;
  bool runtime_only = false;
  bool clear_override = false;
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

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
};

void add_node_commands(CLI::App& app, Request& request);
void add_core_commands(CLI::App& app, Request& request);
int execute_request(const Request& request);
int print_result(const Request& request, const Result<Json>& result);
std::string default_socket_path();

}  // namespace xrm::cli

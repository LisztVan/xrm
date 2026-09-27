#include "cli/command_options.hpp"

namespace xrm::cli {

void add_subscription_commands(CLI::App& app, Request& request) {
  auto* sub = app.add_subcommand("sub", "Manage subscription sources and refresh node inventory");
  auto* add = sub->add_subcommand("add", "Add a subscription source");
  add->add_option("--name", request.subscription_name)->required();
  add->add_option("--url", request.subscription_url)->required();
  add->add_option("--user-agent", request.subscription_user_agent, "HTTP User-Agent");
  add->add_option("--proxy", request.subscription_proxy, "Explicit HTTP/SOCKS download proxy");
  add->add_option("--header", request.subscription_headers, "Custom HTTP header KEY=VALUE (repeatable)");
  add->add_flag("--paused", request.subscription_paused, "Do not refresh automatically");
  add->callback([&request] {
    request.method = "subscription.add";
    request.params = {{"name", request.subscription_name}, {"url", request.subscription_url}};
    if (!request.subscription_user_agent.empty()) request.params["user_agent"] = request.subscription_user_agent;
    if (!request.subscription_proxy.empty()) request.params["proxy_url"] = request.subscription_proxy;
    request.params["auto_refresh"] = !request.subscription_paused;
    Json headers = Json::object();
    for (const auto& header : request.subscription_headers) {
      const auto separator = header.find('=');
      if (separator == std::string::npos || separator == 0)
        throw CLI::ValidationError("--header格式必须为KEY=VALUE");
      headers[header.substr(0, separator)] = header.substr(separator + 1);
    }
    if (!headers.empty()) request.params["headers"] = std::move(headers);
  });

  auto* list = sub->add_subcommand("list", "List sources without URL credentials");
  list->callback([&request] { request.method = "subscription.list"; request.params = Json::object(); });

  auto* show = sub->add_subcommand("show", "Show a source summary; --json includes its editable URL");
  show->add_option("source_id", request.subscription_id, "Exact subscription name or source ID from sub list")->required();
  show->callback([&request] { request.method = "subscription.get"; request.params = {{"id", request.subscription_id}}; });

  auto* edit = sub->add_subcommand("edit", "Edit source name and/or URL");
  edit->add_option("source_id", request.subscription_id, "Exact subscription name or source ID from sub list")->required();
  edit->add_option("--name", request.subscription_name);
  edit->add_option("--url", request.subscription_url);
  edit->add_option("--header", request.subscription_headers, "Replace custom HTTP headers (repeatable)");
  edit->add_option("--proxy", request.subscription_proxy, "Explicit HTTP/SOCKS download proxy");
  edit->add_flag("--paused", request.subscription_paused, "Pause automatic refresh");
  edit->add_flag("--resume", request.subscription_resume, "Enable automatic refresh");
  edit->callback([&request] {
    if (request.subscription_name.empty() && request.subscription_url.empty() &&
        request.subscription_headers.empty() && request.subscription_proxy.empty() &&
        !request.subscription_paused && !request.subscription_resume)
      throw CLI::ValidationError("sub edit至少需要一个变更选项");
    request.method = "subscription.replace";
    request.params = {{"id", request.subscription_id}};
    if (!request.subscription_name.empty()) request.params["name"] = request.subscription_name;
    if (!request.subscription_url.empty()) request.params["url"] = request.subscription_url;
    if (!request.subscription_headers.empty()) {
      Json headers = Json::object();
      for (const auto& header : request.subscription_headers) {
        const auto separator = header.find('=');
        if (separator == std::string::npos || separator == 0)
          throw CLI::ValidationError("--header格式必须为KEY=VALUE");
        headers[header.substr(0, separator)] = header.substr(separator + 1);
      }
      request.params["headers"] = std::move(headers);
    }
    if (!request.subscription_proxy.empty()) request.params["proxy_url"] = request.subscription_proxy;
    if (request.subscription_paused) request.params["auto_refresh"] = false;
    if (request.subscription_resume) request.params["auto_refresh"] = true;
  });

  auto* update = sub->add_subcommand("update", "Refresh one source or all sources; never applies runtime files");
  update->add_option("source_id", request.subscription_id, "Exact subscription name or source ID; omit for all sources");
  update->callback([&request] {
    request.method = "subscription.refresh";
    request.params = Json::object();
    if (!request.subscription_id.empty()) request.params["source_ids"] = Json::array({request.subscription_id});
  });

  auto* remove = sub->add_subcommand("remove", "Remove a source while preserving its nodes");
  remove->add_option("source_id", request.subscription_id, "Exact subscription name or source ID from sub list")->required();
  remove->add_flag("--keep-nodes", request.keep_nodes_manual,
                   "Convert its current nodes to independent manual nodes");
  remove->callback([&request] {
    request.method = "subscription.remove";
    request.params = {{"id", request.subscription_id}, {"keep_as_manual", request.keep_nodes_manual}};
  });
}

}  // namespace xrm::cli

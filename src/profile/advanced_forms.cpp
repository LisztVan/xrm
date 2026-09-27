// XRM: Maps advanced Profile form edits into native module values.

#include "profile/forms.hpp"

namespace xrm {
namespace {

AppError invalid(std::string message, std::string subject) {
  return AppError{
      ErrorCode::invalid_argument, std::move(message), std::move(subject),
      "请使用对应节点的 typed 表单；原生 outbound 请使用原生编辑器"};
}

Result<Node> editable_stream_node(const Node &node, std::string_view field) {
  if (!std::holds_alternative<StreamNode>(node.settings))
    return std::unexpected(
        invalid("原生 outbound 不支持 typed 节点子表单", std::string(field)));
  return node;
}

Result<void> require_object_options(const Json &options,
                                    std::string_view field) {
  if (!options.is_object())
    return std::unexpected(
        invalid("该子表单值必须是 Xray 原生对象", std::string(field)));
  return {};
}

Result<void> require_optional_object(const std::optional<Json> &value,
                                     std::string_view field) {
  if (value && !value->is_object())
    return std::unexpected(
        invalid("该子表单只接受原生对象或清除操作", std::string(field)));
  return {};
}

} // namespace

Result<Node> edit_node_transport(const Node &node, Transport transport) {
  auto editable = editable_stream_node(node, "transport");
  if (!editable)
    return std::unexpected(editable.error());
  const bool protocol_is_hysteria = std::holds_alternative<Hysteria2Settings>(
      std::get<StreamNode>(editable->settings).protocol);
  const bool transport_is_hysteria =
      std::holds_alternative<HysteriaTransport>(transport);
  if (protocol_is_hysteria != transport_is_hysteria)
    return std::unexpected(invalid(
        "Hysteria2 节点必须使用 hysteria transport，其他 typed 协议不能使用它",
        "streamSettings.network"));
  if (const auto *hysteria = std::get_if<HysteriaTransport>(&transport)) {
    auto valid =
        require_object_options(hysteria->options, "transport.hysteriaSettings");
    if (!valid)
      return std::unexpected(valid.error());
  }
  std::get<StreamNode>(editable->settings).transport = std::move(transport);
  return editable;
}

Result<Node> edit_node_security(const Node &node, Security security) {
  auto editable = editable_stream_node(node, "security");
  if (!editable)
    return std::unexpected(editable.error());
  const auto options_valid = std::visit(
      [](const auto &value) -> Result<void> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, TlsSecurity> ||
                      std::is_same_v<T, RealitySecurity>)
          return require_object_options(value.options, "security.options");
        return {};
      },
      security);
  if (!options_valid)
    return std::unexpected(options_valid.error());
  std::get<StreamNode>(editable->settings).security = std::move(security);
  return editable;
}

Result<Node> edit_node_sockopt(const Node &node, std::optional<Json> sockopt) {
  auto editable = editable_stream_node(node, "streamSettings.sockopt");
  if (!editable)
    return std::unexpected(editable.error());
  auto valid = require_optional_object(sockopt, "streamSettings.sockopt");
  if (!valid)
    return std::unexpected(valid.error());
  std::get<StreamNode>(editable->settings).sockopt = std::move(sockopt);
  return editable;
}

Result<Node> edit_node_finalmask(const Node &node,
                                 std::optional<Json> finalmask) {
  auto editable = editable_stream_node(node, "streamSettings.finalmask");
  if (!editable)
    return std::unexpected(editable.error());
  auto valid = require_optional_object(finalmask, "streamSettings.finalmask");
  if (!valid)
    return std::unexpected(valid.error());
  std::get<StreamNode>(editable->settings).finalmask = std::move(finalmask);
  return editable;
}

Result<Node> edit_node_mux(const Node &node, std::optional<Json> mux) {
  auto editable = editable_stream_node(node, "mux");
  if (!editable)
    return std::unexpected(editable.error());
  auto valid = require_optional_object(mux, "mux");
  if (!valid)
    return std::unexpected(valid.error());
  std::get<StreamNode>(editable->settings).mux = std::move(mux);
  return editable;
}

} // namespace xrm

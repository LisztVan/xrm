/// @file
/// @brief Validates geodata resource files before use.

#include "geodata/resource_validation.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>

namespace xrm {
namespace {

AppError invalid(std::string message, std::string subject) {
  return {ErrorCode::invalid_argument, std::move(message), std::move(subject),
          std::nullopt};
}

bool safe_label(const std::string &label) {
  return !label.empty() &&
         std::all_of(label.begin(), label.end(), [](unsigned char ch) {
           // Official geosite.dat contains labels such as CATEGORY-SCHOLAR-!CN.
           // A rule reference uses ':' as its separator; other visible ASCII is
           // retained until the selected Xray binary checks the actual tag.
           return ch >= 0x21 && ch <= 0x7e && ch != ':' && ch != '/' &&
                  ch != '\\';
         });
}

struct WireField {
  std::uint64_t number = 0;
  unsigned type = 0;
  std::uint64_t integer = 0;
  std::string_view bytes;
};

class WireReader {
public:
  explicit WireReader(std::string_view input) : input_(input) {}
  bool done() const { return position_ == input_.size(); }

  Result<WireField> next() {
    auto key = integer();
    if (!key)
      return std::unexpected(key.error());
    WireField field{*key >> 3, static_cast<unsigned>(*key & 7), 0, {}};
    if (field.number == 0)
      return std::unexpected(
          invalid("无效的 Geodata protobuf 字段编号", "asset"));
    if (field.type == 0) {
      auto value = integer();
      if (!value)
        return std::unexpected(value.error());
      field.integer = *value;
    } else if (field.type == 1 || field.type == 5) {
      const std::size_t length = field.type == 1 ? 8 : 4;
      if (length > input_.size() - position_)
        return std::unexpected(
            invalid("Geodata protobuf 固定字段被截断", "asset"));
      field.bytes = input_.substr(position_, length);
      position_ += length;
    } else if (field.type == 2) {
      auto length = integer();
      if (!length)
        return std::unexpected(length.error());
      if (*length > input_.size() - position_)
        return std::unexpected(
            invalid("Geodata protobuf 长度字段被截断", "asset"));
      field.bytes = input_.substr(position_, static_cast<std::size_t>(*length));
      position_ += static_cast<std::size_t>(*length);
    } else {
      return std::unexpected(
          invalid("Geodata protobuf 使用了不支持的 wire type", "asset"));
    }
    return field;
  }

private:
  Result<std::uint64_t> integer() {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 70; shift += 7) {
      if (position_ == input_.size())
        return std::unexpected(
            invalid("Geodata protobuf varint 被截断", "asset"));
      const auto byte = static_cast<unsigned char>(input_[position_++]);
      if (shift == 63 && (byte & 0x7f) > 1)
        return std::unexpected(
            invalid("Geodata protobuf varint 溢出", "asset"));
      value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
      if ((byte & 0x80) == 0)
        return value;
    }
    return std::unexpected(invalid("Geodata protobuf varint 过长", "asset"));
  }

  std::string_view input_;
  std::size_t position_ = 0;
};

Result<void> inspect_cidr(std::string_view bytes) {
  WireReader reader(bytes);
  std::size_t width = 0;
  std::optional<std::uint64_t> prefix;
  while (!reader.done()) {
    auto field = reader.next();
    if (!field)
      return std::unexpected(field.error());
    if (field->number == 1 && field->type == 2)
      width = field->bytes.size();
    if (field->number == 2 && field->type == 0)
      prefix = field->integer;
  }
  if ((width != 4 && width != 16) || !prefix || *prefix > width * 8)
    return std::unexpected(invalid("GeoIP 条目不是有效 CIDR", "asset"));
  return {};
}

Result<void> inspect_domain(std::string_view bytes) {
  WireReader reader(bytes);
  std::optional<std::uint64_t> type;
  std::string_view value;
  while (!reader.done()) {
    auto field = reader.next();
    if (!field)
      return std::unexpected(field.error());
    if (field->number == 1 && field->type == 0)
      type = field->integer;
    if (field->number == 2 && field->type == 2)
      value = field->bytes;
  }
  if (value.empty() || (type && *type > 3))
    return std::unexpected(invalid("GeoSite 条目不是有效 Domain", "asset"));
  return {};
}

Result<std::string> inspect_entry(GeoAssetKind kind, std::string_view bytes) {
  WireReader reader(bytes);
  std::string code;
  std::size_t values = 0;
  while (!reader.done()) {
    auto field = reader.next();
    if (!field)
      return std::unexpected(field.error());
    if (field->number == 1 && field->type == 2)
      code = std::string(field->bytes);
    if (field->number != 2 || field->type != 2)
      continue;
    auto checked = kind == GeoAssetKind::geoip ? inspect_cidr(field->bytes)
                                               : inspect_domain(field->bytes);
    if (!checked)
      return std::unexpected(checked.error());
    ++values;
  }
  if (!safe_label(code) || values == 0)
    return std::unexpected(invalid("Geodata 标签或内容无效", code));
  std::transform(code.begin(), code.end(), code.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return code;
}

} // namespace

Result<GeoAssetInspection> inspect_geodata_bytes(GeoAssetKind kind,
                                                 std::string_view bytes) {
  if (bytes.empty())
    return std::unexpected(invalid("Geodata 资源为空", "asset"));
  WireReader reader(bytes);
  GeoAssetInspection inspection{kind, {}, 0};
  while (!reader.done()) {
    auto field = reader.next();
    if (!field)
      return std::unexpected(field.error());
    if (field->number != 1 || field->type != 2)
      continue;
    auto code = inspect_entry(kind, field->bytes);
    if (!code)
      return std::unexpected(code.error());
    inspection.labels.insert(std::move(*code));
    ++inspection.entries;
  }
  if (inspection.entries == 0)
    return std::unexpected(invalid("Geodata 资源没有有效标签", "asset"));
  return inspection;
}

Result<FileSet>
geodata_validation_files(GeoAssetKind kind, std::string file_name,
                         const std::set<std::string> &required_labels) {
  if (file_name.empty() || file_name == "." || file_name == ".." ||
      file_name.find('/') != std::string::npos ||
      file_name.find('\\') != std::string::npos ||
      file_name.find(':') != std::string::npos || !file_name.ends_with(".dat"))
    return std::unexpected(
        invalid("Geodata 文件名必须是资源目录内的 .dat 文件名", file_name));
  if ((kind == GeoAssetKind::geoip && file_name == "geosite.dat") ||
      (kind == GeoAssetKind::geosite && file_name == "geoip.dat"))
    return std::unexpected(
        invalid("GeoIP 与 GeoSite 文件类型不匹配", file_name));
  if (required_labels.empty())
    return std::unexpected(
        invalid("至少指定一个待验证标签；空规则不能证明资源可用", file_name));

  Json rules = Json::array();
  for (const auto &label : required_labels) {
    if (!safe_label(label))
      return std::unexpected(invalid("Geodata 标签包含不支持的字符", label));
    Json rule{{"type", "field"}, {"outboundTag", "direct"}};
    const auto reference = "ext:" + file_name + ":" + label;
    if (kind == GeoAssetKind::geoip)
      rule["ip"] = Json::array({reference});
    else
      rule["domain"] = Json::array({reference});
    rules.push_back(std::move(rule));
  }
  Json fixture{{"outbounds", Json::array({Json{{"tag", "direct"},
                                               {"protocol", "freedom"},
                                               {"settings", Json::object()}}})},
               {"routing",
                Json{{"domainStrategy", "AsIs"}, {"rules", std::move(rules)}}}};
  return FileSet{
      OutputFile{"00_geodata_validation.json", fixture.dump(2) + "\n"}};
}

} // namespace xrm

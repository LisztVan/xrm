// XRM: Defines the supported native Xray module catalog.

#include "model/module_catalog.hpp"

#include <array>

namespace xrm {
namespace {

const std::array<ModuleDescriptor, 15> catalog{{
    {"log", "Log", "00_log.json", false},
    {"dns", "DNS", "01_dns.json", false},
    {"inbounds", "Inbounds", "02_inbounds.json", false},
    {"outbounds", "Outbounds", "03_outbounds.json", false},
    {"routing", "Routing", "04_routing.json", false},
    {"observatory", "Observatory", "05_observatory.json", false},
    {"api", "API", "06_api.json", false},
    {"metrics", "Metrics", "07_metrics.json", false},
    {"stats", "Stats", "08_stats.json", false},
    {"policy", "Policy", "09_policy.json", false},
    {"fakedns", "FakeDNS", "10_fakedns.json", false},
    {"geodata", "Geodata", "11_geodata.json", false},
    {"env", "Environment", "12_env.json", false},
    {"version", "Version", "13_version.json", false},
    {"burstObservatory", "Burst Observatory", "14_burstObservatory.json",
     false},
}};

} // namespace

std::span<const ModuleDescriptor> module_catalog() { return catalog; }

const ModuleDescriptor *find_module(std::string_view key) {
  for (const auto &descriptor : catalog) {
    if (descriptor.key == key)
      return &descriptor;
  }
  return nullptr;
}

std::string node_tag(std::string_view node_id) {
  return "xrm.node." + std::string(node_id);
}

} // namespace xrm

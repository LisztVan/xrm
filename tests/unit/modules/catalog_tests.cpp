// XRM: Tests catalog in the domain and editor behavior suite.

#include <catch2/catch_test_macros.hpp>

#include "model/module_catalog.hpp"

#include <array>
#include <string_view>

TEST_CASE("MOD-01 native catalog has all fixed keys and filenames",
          "[MOD-01]") {
  struct Expected {
    std::string_view key;
    std::string_view filename;
  };
  constexpr std::array expected{
      Expected{"log", "00_log.json"},
      Expected{"dns", "01_dns.json"},
      Expected{"inbounds", "02_inbounds.json"},
      Expected{"outbounds", "03_outbounds.json"},
      Expected{"routing", "04_routing.json"},
      Expected{"observatory", "05_observatory.json"},
      Expected{"api", "06_api.json"},
      Expected{"metrics", "07_metrics.json"},
      Expected{"stats", "08_stats.json"},
      Expected{"policy", "09_policy.json"},
      Expected{"fakedns", "10_fakedns.json"},
      Expected{"geodata", "11_geodata.json"},
      Expected{"env", "12_env.json"},
      Expected{"version", "13_version.json"},
      Expected{"burstObservatory", "14_burstObservatory.json"},
  };

  const auto catalog = xrm::module_catalog();
  REQUIRE(catalog.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    INFO(expected[i].key);
    CHECK(catalog[i].key == expected[i].key);
    CHECK(catalog[i].default_filename == expected[i].filename);
    CHECK(xrm::find_module(expected[i].key) == &catalog[i]);
    CHECK_FALSE(catalog[i].label.empty());
  }
  CHECK(xrm::find_module("groups") == nullptr);
  CHECK(xrm::find_module("tls") == nullptr);
  CHECK(xrm::find_module("sniffing") == nullptr);
}

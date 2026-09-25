#include "geodata/resource_validation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace xrm;

namespace {

std::string varint(std::size_t value) {
  std::string out;
  while (value >= 128) {
    out.push_back(static_cast<char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
  return out;
}

std::string field(int number, std::string bytes) {
  return varint(static_cast<std::size_t>((number << 3) | 2)) +
         varint(bytes.size()) + bytes;
}

std::string test_geoip() {
  // v26.9.9 common/geodata/geodat.proto: GeoIPList.entry=1,
  // GeoIP.code=1/cidr=2, CIDR.ip=1/prefix=2.
  const auto cidr = field(1, std::string{"\x7f\0\0\0", 4}) + "\x10\x08";
  return field(1, field(1, "XRM-TEST") + field(2, cidr));
}

std::string test_geosite() {
  // GeoSiteList.entry=1, GeoSite.code=1/domain=2, Domain.type=1/value=2.
  const auto domain = std::string{"\x08\x03", 2} + field(2, "xrm.example");
  return field(1, field(1, "XRM-TEST") + field(2, domain));
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const char* root = std::getenv("TMPDIR");
    auto pattern = (std::filesystem::path(root ? root : "/tmp") / "xrm-geodata-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    if (char* created = ::mkdtemp(writable.data())) path = created;
  }
  ~TemporaryDirectory() {
    if (!path.empty()) std::filesystem::remove_all(path);
  }
  std::filesystem::path path;
};

void write(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  REQUIRE(out.good());
}

int core_test(const std::filesystem::path& xray, const std::filesystem::path& confdir,
              const std::filesystem::path& asset_dir) {
  const auto pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::setenv("XRAY_LOCATION_ASSET", asset_dir.c_str(), 1);
    ::execl(xray.c_str(), xray.c_str(), "run", "-test", "-confdir", confdir.c_str(), nullptr);
    ::_exit(127);
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

TEST_CASE("P5 real Xray validates matching geodata kind and label, rejects missing labels") {
  const char* binary = std::getenv("XRM_TEST_XRAY");
  if (!binary || !std::filesystem::exists(binary)) SKIP("XRM_TEST_XRAY is unavailable");
  TemporaryDirectory temp;
  REQUIRE_FALSE(temp.path.empty());
  const auto confdir = temp.path / "conf";
  const auto assets = temp.path / "assets";
  std::filesystem::create_directories(confdir);
  std::filesystem::create_directories(assets);

  auto ip_inspection = inspect_geodata_bytes(GeoAssetKind::geoip, test_geoip());
  auto site_inspection = inspect_geodata_bytes(GeoAssetKind::geosite, test_geosite());
  REQUIRE(ip_inspection);
  REQUIRE(site_inspection);
  CHECK(ip_inspection->labels.contains("xrm-test"));
  CHECK(site_inspection->labels.contains("xrm-test"));
  write(assets / "geoip.dat", test_geoip());
  write(assets / "geosite.dat", test_geosite());
  for (const auto kind : {GeoAssetKind::geoip, GeoAssetKind::geosite}) {
    const auto file = kind == GeoAssetKind::geoip ? "geoip.dat" : "geosite.dat";
    auto valid = geodata_validation_files(kind, file, {"xrm-test"});
    REQUIRE(valid);
    write(confdir / valid->front().relative_name, valid->front().bytes);
    CHECK(core_test(binary, confdir, assets) == 0);

    auto missing = geodata_validation_files(kind, file, {"not-present"});
    REQUIRE(missing);
    write(confdir / missing->front().relative_name, missing->front().bytes);
    CHECK(core_test(binary, confdir, assets) != 0);
  }

  // The same .dat extension does not make GeoSite bytes a valid GeoIP source.
  CHECK_FALSE(inspect_geodata_bytes(GeoAssetKind::geoip, test_geosite()));
  // v26.9.9 `-test` checks the label envelope but can ignore an invalid CIDR
  // payload. The explicit kind inspection above is essential before publish.
}

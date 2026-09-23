#include "model/module_catalog.hpp"
#include "profile/profile_service.hpp"
#include "xray/codecs/codec.hpp"
#include "xray/core_manager.hpp"
#include "xray/runtime_assembler.hpp"
#include "xray/runtime_writer.hpp"
#include "xray/process_adapter.hpp"

#include <type_traits>
#include <utility>

static_assert(std::is_default_constructible_v<xrm::ProfileSettings>);
static_assert(std::is_default_constructible_v<xrm::ModuleDocument>);
static_assert(std::is_default_constructible_v<xrm::BuildInput>);
static_assert(std::is_default_constructible_v<xrm::PreparedFiles>);
static_assert(!std::is_copy_constructible_v<xrm::CoreManager>);
static_assert(std::is_same_v<decltype(xrm::assemble(std::declval<const xrm::BuildInput&>())),
                             xrm::Result<xrm::PreparedFiles>>);
static_assert(std::is_same_v<decltype(xrm::parse_vless_uri(std::string_view{})),
                             xrm::Result<xrm::DecodedNode>>);
static_assert(std::is_same_v<decltype(xrm::write_outbound(
                                  std::declval<const xrm::Node&>(), std::string_view{})),
                             xrm::Result<xrm::Json>>);
static_assert(std::is_same_v<decltype(xrm::check_command(
                                  std::declval<const xrm::RunSpec&>())),
                             xrm::ProcessSpec>);

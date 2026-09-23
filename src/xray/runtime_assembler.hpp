#pragma once

#include "model/runtime.hpp"

namespace xrm {

Result<PreparedFiles> assemble(const BuildInput& input);
Result<OutputFile> emit_native_module(const ModuleDocument& document);
Result<RuntimeIndex> resolve_scope(const BuildInput& input);
Result<std::vector<OutputFile>> write_outbounds(const BuildInput& input,
                                               const RuntimeIndex& index);
Result<OutputFile> write_routing(const ModuleDocument& document,
                                 const BuildInput& input,
                                 const RuntimeIndex& index);
Result<OutputFile> write_observation(const ModuleDocument& document,
                                     const BuildInput& input,
                                     const RuntimeIndex& index);

}  // namespace xrm

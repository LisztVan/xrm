/// @file
/// @brief Declares pure runtime FileSet assembly.

#pragma once

#include "model/runtime.hpp"

namespace xrm {

/// @brief Transforms saved input into native confdir files and a runtime index.
///
/// Pure transformation: no disk, network, database, or process action occurs
/// here. Each enabled native root retains one output owner.
Result<PreparedFiles> assemble(const BuildInput &input);
Result<OutputFile> emit_native_module(const ModuleDocument &document);
Result<RuntimeIndex> resolve_scope(const BuildInput &input);
Result<std::vector<OutputFile>> write_outbounds(const BuildInput &input,
                                                const RuntimeIndex &index);
Result<OutputFile> write_routing(const ModuleDocument &document,
                                 const BuildInput &input,
                                 const RuntimeIndex &index);
Result<OutputFile> write_observation(const ModuleDocument &document,
                                     const BuildInput &input,
                                     const RuntimeIndex &index);

} // namespace xrm

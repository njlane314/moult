#pragma once

#include "moult/core/engine.hpp"

#include <filesystem>
#include <string>

namespace moult::core {

struct WriteOutputsResult {
    bool ok = true;
    std::vector<std::string> written_files;
    std::vector<std::string> errors;
};

WriteOutputsResult write_outputs(const EngineRunResult& result,
                                  const std::filesystem::path& output_dir,
                                  bool include_facts_in_plan = false);

std::string sarif_from_plan(const Plan& plan);

} // namespace moult::core

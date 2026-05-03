#pragma once

#include "moult/core/engine.hpp"

#include <string>
#include <vector>

namespace moult::clang_adapter {

class ClangCppModernizationAdapter final : public core::SemanticAdapter {
public:
    ClangCppModernizationAdapter();
    explicit ClangCppModernizationAdapter(std::vector<std::string> command_line_args);

    void analyze(const core::SourceStore& sources,
                 core::FactStore& facts,
                 core::DiagnosticSink& diagnostics,
                 const core::RunOptions& options) const override;

    [[nodiscard]] const std::vector<std::string>& command_line_args() const noexcept {
        return command_line_args_;
    }

private:
    std::vector<std::string> command_line_args_;
};

} // namespace moult::clang_adapter

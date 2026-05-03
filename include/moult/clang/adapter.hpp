#pragma once

#include "moult/core/engine.hpp"

#include <map>
#include <string>
#include <vector>

namespace moult::clang_adapter {

class ClangCppModernizationAdapter final : public core::SemanticAdapter {
public:
    using CompileCommandMap = std::map<std::string, std::vector<std::string>>;

    ClangCppModernizationAdapter();
    explicit ClangCppModernizationAdapter(std::vector<std::string> command_line_args);
    ClangCppModernizationAdapter(std::vector<std::string> fallback_command_line_args,
                                  CompileCommandMap compile_commands);

    void analyze(const core::SourceStore& sources,
                 core::FactStore& facts,
                 core::DiagnosticSink& diagnostics,
                 const core::RunOptions& options) const override;

    [[nodiscard]] const std::vector<std::string>& command_line_args() const noexcept {
        return command_line_args_;
    }

private:
    std::vector<std::string> command_line_args_;
    CompileCommandMap compile_commands_;
};

} // namespace moult::clang_adapter

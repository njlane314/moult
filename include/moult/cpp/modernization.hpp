#pragma once

#include "moult/core/engine.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace moult::cpp_modernization {

inline constexpr std::string_view target_name = "cpp-modernisation";
inline constexpr std::string_view legacy_target_name = "cpp-modernization";

class TextualCppModernizationAdapter final : public core::SemanticAdapter {
public:
    void analyze(const core::SourceStore& sources,
                 core::FactStore& facts,
                 core::DiagnosticSink& diagnostics,
                 const core::RunOptions& options) const override;
};

class CppModernizationCapsule final : public core::Capsule {
public:
    [[nodiscard]] std::string id() const override;
    [[nodiscard]] std::string name() const override;
    [[nodiscard]] std::string version() const override;
    [[nodiscard]] std::vector<std::string> targets() const override;
    void register_rules(core::RuleRegistry& registry) const override;
};

} // namespace moult::cpp_modernization

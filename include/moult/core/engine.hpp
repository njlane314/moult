#pragma once

#include "moult/core/capsule.hpp"

#include <memory>
#include <vector>

namespace moult::core {

class SemanticAdapter {
public:
    virtual ~SemanticAdapter() = default;

    // A Clang adapter, tree-sitter adapter, build-system scanner, or proprietary analyzer
    // populates facts here. The core deliberately does not know parser-specific classes.
    virtual void analyze(const SourceStore& sources,
                         FactStore& facts,
                         DiagnosticSink& diagnostics,
                         const RunOptions& options) const = 0;
};

struct EngineRunResult {
    Plan plan;
    FactStore facts;
    DiagnosticSink diagnostics;
};

class Engine {
public:
    void set_adapter(std::shared_ptr<const SemanticAdapter> adapter);
    void add_capsule(std::shared_ptr<const Capsule> capsule);

    [[nodiscard]] EngineRunResult run(const SourceStore& sources, const RunOptions& options) const;

private:
    std::shared_ptr<const SemanticAdapter> adapter_;
    std::vector<std::shared_ptr<const Capsule>> capsules_;
};

} // namespace moult::core

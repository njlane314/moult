#pragma once

#include "moult/core/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moult::core {

// Language-specific adapters populate FactStore. Capsules should depend on facts,
// not on one parser's concrete AST classes.
struct Fact {
    std::string id;
    std::string kind;      // e.g. "cxx.call", "symbol.reference", "build.target"
    std::string subject;   // stable symbol/site/build-target identifier
    std::string predicate; // e.g. "resolves_to", "declared_in", "calls"
    std::string object;    // stable target identifier or scalar value
    std::optional<SourceRange> range;
    Attributes attributes;
};

class FactStore {
public:
    const Fact& add(Fact fact);
    const Fact& add(std::string kind,
                    std::string subject,
                    std::string predicate,
                    std::string object,
                    std::optional<SourceRange> range = std::nullopt,
                    Attributes attributes = {});

    [[nodiscard]] const std::vector<Fact>& all() const noexcept { return facts_; }
    [[nodiscard]] std::vector<const Fact*> by_kind(std::string_view kind) const;
    [[nodiscard]] std::vector<const Fact*> by_subject(std::string_view subject) const;
    [[nodiscard]] std::vector<const Fact*> by_predicate(std::string_view predicate) const;
    [[nodiscard]] std::vector<const Fact*> where(std::function<bool(const Fact&)> pred) const;
    [[nodiscard]] const Fact* find_id(std::string_view id) const;
    [[nodiscard]] bool empty() const noexcept { return facts_.empty(); }

private:
    std::vector<Fact> facts_;
};

std::string fact_to_json(const Fact& fact);
std::string facts_to_json_array(const std::vector<Fact>& facts);

} // namespace moult::core

#include "moult/core/facts.hpp"
#include "moult/core/json.hpp"

#include <sstream>

namespace moult::core {

const Fact& FactStore::add(Fact fact) {
    if (fact.id.empty()) {
        std::vector<std::string_view> parts{fact.kind, fact.subject, fact.predicate, fact.object};
        if (fact.range) {
            const std::string begin = std::to_string(fact.range->begin);
            const std::string end = std::to_string(fact.range->end);
            parts.push_back(fact.range->file);
            parts.push_back(begin);
            parts.push_back(end);
            fact.id = stable_id("fact", parts);
        } else {
            fact.id = stable_id("fact", parts);
        }
    }
    facts_.push_back(std::move(fact));
    return facts_.back();
}

const Fact& FactStore::add(std::string kind,
                           std::string subject,
                           std::string predicate,
                           std::string object,
                           std::optional<SourceRange> range,
                           Attributes attributes) {
    return add(Fact{"", std::move(kind), std::move(subject), std::move(predicate), std::move(object), std::move(range), std::move(attributes)});
}

std::vector<const Fact*> FactStore::by_kind(std::string_view kind) const {
    return where([&](const Fact& f) { return f.kind == kind; });
}

std::vector<const Fact*> FactStore::by_subject(std::string_view subject) const {
    return where([&](const Fact& f) { return f.subject == subject; });
}

std::vector<const Fact*> FactStore::by_predicate(std::string_view predicate) const {
    return where([&](const Fact& f) { return f.predicate == predicate; });
}

std::vector<const Fact*> FactStore::where(std::function<bool(const Fact&)> pred) const {
    std::vector<const Fact*> out;
    for (const auto& fact : facts_) {
        if (pred(fact)) out.push_back(&fact);
    }
    return out;
}

const Fact* FactStore::find_id(std::string_view id) const {
    for (const auto& fact : facts_) {
        if (fact.id == id) return &fact;
    }
    return nullptr;
}

static std::string range_to_json(const SourceRange& r) {
    std::ostringstream os;
    JsonObjectWriter obj(os);
    obj.string_field("file", r.file);
    obj.number_field("begin", r.begin);
    obj.number_field("end", r.end);
    obj.finish();
    return os.str();
}

std::string fact_to_json(const Fact& fact) {
    std::ostringstream os;
    JsonObjectWriter obj(os);
    obj.string_field("id", fact.id);
    obj.string_field("kind", fact.kind);
    obj.string_field("subject", fact.subject);
    obj.string_field("predicate", fact.predicate);
    obj.string_field("object", fact.object);
    if (fact.range) obj.raw_field("range", range_to_json(*fact.range));
    if (!fact.attributes.empty()) obj.object_string_map_field("attributes", fact.attributes);
    obj.finish();
    return os.str();
}

std::string facts_to_json_array(const std::vector<Fact>& facts) {
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& fact : facts) {
        if (!first) os << ",";
        first = false;
        os << fact_to_json(fact);
    }
    os << "]";
    return os.str();
}

} // namespace moult::core

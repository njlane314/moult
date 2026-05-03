#include "moult/core/edits.hpp"
#include "moult/core/json.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace moult::core {

static bool overlap(const SourceRange& a, const SourceRange& b) noexcept {
    return a.file == b.file && !(a.end <= b.begin || b.end <= a.begin);
}

static bool same_edit(const TextEdit& a, const TextEdit& b) noexcept {
    return a.range.file == b.range.file && a.range.begin == b.range.begin && a.range.end == b.range.end &&
           a.replacement == b.replacement;
}

EditAddResult EditSet::add(TextEdit edit, const SourceStore* sources) {
    if (!edit.range.valid()) {
        return EditAddResult{EditAddOutcome::Invalid, std::nullopt, edit.id};
    }
    if (sources) {
        const auto* source = sources->get(edit.range.file);
        if (!source || !source->contains(edit.range)) {
            return EditAddResult{EditAddOutcome::Invalid, std::nullopt, edit.id};
        }
    }
    if (edit.id.empty()) {
        const std::string begin = std::to_string(edit.range.begin);
        const std::string end = std::to_string(edit.range.end);
        edit.id = stable_id("edit", {edit.rule_id, edit.range.file, begin, end, edit.replacement});
    }

    for (const auto& existing : edits_) {
        if (same_edit(existing, edit)) {
            return EditAddResult{EditAddOutcome::Duplicate, std::nullopt, existing.id};
        }
        if (overlap(existing.range, edit.range)) {
            EditConflict conflict{existing.id,
                                  edit.id,
                                  existing.range,
                                  edit.range,
                                  "overlapping edits in the same file"};
            conflicts_.push_back(conflict);
            return EditAddResult{EditAddOutcome::Conflict, conflict, edit.id};
        }
    }

    edits_.push_back(std::move(edit));
    return EditAddResult{EditAddOutcome::Accepted, std::nullopt, edits_.back().id};
}

std::vector<TextEdit> EditSet::sorted() const {
    auto out = edits_;
    std::sort(out.begin(), out.end(), [](const TextEdit& a, const TextEdit& b) {
        if (a.range.file != b.range.file) return a.range.file < b.range.file;
        if (a.range.begin != b.range.begin) return a.range.begin < b.range.begin;
        if (a.range.end != b.range.end) return a.range.end < b.range.end;
        return a.id < b.id;
    });
    return out;
}

std::map<std::string, std::vector<TextEdit>> EditSet::by_file() const {
    std::map<std::string, std::vector<TextEdit>> out;
    for (const auto& edit : sorted()) out[edit.range.file].push_back(edit);
    return out;
}

std::map<std::string, std::string> EditSet::apply_to_memory(const SourceStore& sources) const {
    if (has_conflicts()) throw std::runtime_error("cannot apply edit set with conflicts");
    std::map<std::string, std::string> result;
    for (auto [file, edits] : by_file()) {
        const SourceBuffer* source = sources.get(file);
        if (!source) throw std::runtime_error("edit references missing source file: " + file);
        std::string text = source->text();
        std::sort(edits.begin(), edits.end(), [](const TextEdit& a, const TextEdit& b) {
            return a.range.begin > b.range.begin;
        });
        for (const auto& edit : edits) {
            if (edit.range.end > text.size() || edit.range.begin > edit.range.end) {
                throw std::runtime_error("edit range out of bounds for file: " + file);
            }
            text.replace(edit.range.begin, edit.range.end - edit.range.begin, edit.replacement);
        }
        result[file] = std::move(text);
    }
    return result;
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

std::string edit_to_json(const TextEdit& edit) {
    std::ostringstream os;
    JsonObjectWriter obj(os);
    obj.string_field("id", edit.id);
    obj.string_field("rule_id", edit.rule_id);
    obj.string_field("evidence_id", edit.evidence_id);
    obj.string_field("confidence", to_string(edit.confidence));
    obj.raw_field("range", range_to_json(edit.range));
    obj.string_field("replacement", edit.replacement);
    if (!edit.attributes.empty()) obj.object_string_map_field("attributes", edit.attributes);
    obj.finish();
    return os.str();
}

std::string conflict_to_json(const EditConflict& conflict) {
    std::ostringstream os;
    JsonObjectWriter obj(os);
    obj.string_field("existing_edit_id", conflict.existing_edit_id);
    obj.string_field("proposed_edit_id", conflict.proposed_edit_id);
    obj.raw_field("existing_range", range_to_json(conflict.existing_range));
    obj.raw_field("proposed_range", range_to_json(conflict.proposed_range));
    obj.string_field("message", conflict.message);
    obj.finish();
    return os.str();
}

std::string edits_to_json_array(const std::vector<TextEdit>& edits) {
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& edit : edits) {
        if (!first) os << ",";
        first = false;
        os << edit_to_json(edit);
    }
    os << "]";
    return os.str();
}

std::string conflicts_to_json_array(const std::vector<EditConflict>& conflicts) {
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& conflict : conflicts) {
        if (!first) os << ",";
        first = false;
        os << conflict_to_json(conflict);
    }
    os << "]";
    return os.str();
}

} // namespace moult::core

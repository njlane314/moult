#pragma once

#include "moult/core/source.hpp"
#include "moult/core/types.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace moult::core {

enum class EditAddOutcome {
    Accepted,
    Duplicate,
    Conflict,
    Invalid
};

struct TextEdit {
    std::string id;
    SourceRange range;
    std::string replacement;
    std::string rule_id;
    std::string evidence_id;
    Confidence confidence = Confidence::Medium;
    Attributes attributes;
};

struct EditConflict {
    std::string existing_edit_id;
    std::string proposed_edit_id;
    SourceRange existing_range;
    SourceRange proposed_range;
    std::string message;
};

struct EditAddResult {
    EditAddOutcome outcome = EditAddOutcome::Invalid;
    std::optional<EditConflict> conflict;
    std::string edit_id;
};

class EditSet {
public:
    EditAddResult add(TextEdit edit, const SourceStore* sources = nullptr);

    [[nodiscard]] const std::vector<TextEdit>& edits() const noexcept { return edits_; }
    [[nodiscard]] const std::vector<EditConflict>& conflicts() const noexcept { return conflicts_; }
    [[nodiscard]] bool has_conflicts() const noexcept { return !conflicts_.empty(); }
    [[nodiscard]] bool empty() const noexcept { return edits_.empty(); }
    [[nodiscard]] std::vector<TextEdit> sorted() const;
    [[nodiscard]] std::map<std::string, std::vector<TextEdit>> by_file() const;

    // Applies accepted edits to in-memory sources and returns new text per modified file.
    // Throws if an edit references a missing file/range or if conflicts were recorded.
    [[nodiscard]] std::map<std::string, std::string> apply_to_memory(const SourceStore& sources) const;

private:
    std::vector<TextEdit> edits_;
    std::vector<EditConflict> conflicts_;
};

std::string edit_to_json(const TextEdit& edit);
std::string conflict_to_json(const EditConflict& conflict);
std::string edits_to_json_array(const std::vector<TextEdit>& edits);
std::string conflicts_to_json_array(const std::vector<EditConflict>& conflicts);

} // namespace moult::core

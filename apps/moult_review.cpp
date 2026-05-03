#include "moult_review.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace moult::cli {
namespace {

struct JsonValue {
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    Type type = Type::Null;
    bool boolean = false;
    std::size_t number = 0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        JsonValue value = parse_value();
        skip_ws();
        if (pos_ != text_.size()) throw std::runtime_error("unexpected trailing JSON data");
        return value;
    }

private:
    void skip_ws() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    char peek() {
        skip_ws();
        if (pos_ >= text_.size()) throw std::runtime_error("unexpected end of JSON");
        return text_[pos_];
    }

    bool consume(char c) {
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char c) {
        if (!consume(c)) throw std::runtime_error(std::string("expected '") + c + "'");
    }

    bool consume_literal(std::string_view literal) {
        skip_ws();
        if (text_.substr(pos_, literal.size()) == literal) {
            pos_ += literal.size();
            return true;
        }
        return false;
    }

    JsonValue parse_value() {
        const char c = peek();
        if (c == '"') return JsonValue{JsonValue::Type::String, false, 0, parse_string(), {}, {}};
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (std::isdigit(static_cast<unsigned char>(c))) return parse_number();
        if (consume_literal("true")) return JsonValue{JsonValue::Type::Bool, true, 0, {}, {}, {}};
        if (consume_literal("false")) return JsonValue{JsonValue::Type::Bool, false, 0, {}, {}, {}};
        if (consume_literal("null")) return JsonValue{};
        throw std::runtime_error("unsupported JSON value");
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) throw std::runtime_error("unterminated JSON escape");
            const char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                    if (pos_ + 4 > text_.size()) throw std::runtime_error("short JSON unicode escape");
                    out.push_back('?');
                    pos_ += 4;
                    break;
                default: throw std::runtime_error("unknown JSON escape");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }

    JsonValue parse_number() {
        skip_ws();
        std::size_t value = 0;
        bool any = false;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            any = true;
            value = value * 10 + static_cast<std::size_t>(text_[pos_] - '0');
            ++pos_;
        }
        if (!any) throw std::runtime_error("expected JSON number");
        return JsonValue{JsonValue::Type::Number, false, value, {}, {}, {}};
    }

    JsonValue parse_array() {
        expect('[');
        JsonValue value;
        value.type = JsonValue::Type::Array;
        if (consume(']')) return value;
        do {
            value.array.push_back(parse_value());
        } while (consume(','));
        expect(']');
        return value;
    }

    JsonValue parse_object() {
        expect('{');
        JsonValue value;
        value.type = JsonValue::Type::Object;
        if (consume('}')) return value;
        do {
            std::string key = parse_string();
            expect(':');
            value.object.emplace(std::move(key), parse_value());
        } while (consume(','));
        expect('}');
        return value;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

std::string json_escape(std::string_view input) {
    std::ostringstream out;
    for (unsigned char c : input) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
                } else {
                    out << static_cast<char>(c);
                }
                break;
        }
    }
    return out.str();
}

std::string json_to_string(const JsonValue& value) {
    std::ostringstream out;
    switch (value.type) {
        case JsonValue::Type::Null:
            out << "null";
            break;
        case JsonValue::Type::Bool:
            out << (value.boolean ? "true" : "false");
            break;
        case JsonValue::Type::Number:
            out << value.number;
            break;
        case JsonValue::Type::String:
            out << '"' << json_escape(value.string) << '"';
            break;
        case JsonValue::Type::Array: {
            out << '[';
            bool first = true;
            for (const auto& item : value.array) {
                if (!first) out << ',';
                first = false;
                out << json_to_string(item);
            }
            out << ']';
            break;
        }
        case JsonValue::Type::Object: {
            out << '{';
            bool first = true;
            for (const auto& [key, item] : value.object) {
                if (!first) out << ',';
                first = false;
                out << '"' << json_escape(key) << "\":" << json_to_string(item);
            }
            out << '}';
            break;
        }
    }
    return out.str();
}

const JsonValue* field(const JsonValue& object, std::string_view name) {
    if (object.type != JsonValue::Type::Object) return nullptr;
    const auto it = object.object.find(std::string(name));
    return it == object.object.end() ? nullptr : &it->second;
}

std::string string_field(const JsonValue& object, std::string_view name, std::string fallback = {}) {
    const JsonValue* value = field(object, name);
    return value && value->type == JsonValue::Type::String ? value->string : fallback;
}

std::size_t number_field(const JsonValue& object, std::string_view name, std::size_t fallback = 0) {
    const JsonValue* value = field(object, name);
    return value && value->type == JsonValue::Type::Number ? value->number : fallback;
}

bool bool_field(const JsonValue& object, std::string_view name, bool fallback = false) {
    const JsonValue* value = field(object, name);
    return value && value->type == JsonValue::Type::Bool ? value->boolean : fallback;
}

const JsonValue* array_field(const JsonValue& object, std::string_view name) {
    const JsonValue* value = field(object, name);
    return value && value->type == JsonValue::Type::Array ? value : nullptr;
}

std::map<std::string, std::string> string_attributes(const JsonValue& object) {
    std::map<std::string, std::string> out;
    const JsonValue* attrs = field(object, "attributes");
    if (!attrs || attrs->type != JsonValue::Type::Object) return out;
    for (const auto& [key, value] : attrs->object) {
        if (value.type == JsonValue::Type::String) out.emplace(key, value.string);
    }
    return out;
}

bool read_text_file(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool write_text_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

std::string compact(std::string value, std::size_t max_len = 92) {
    for (char& c : value) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    while (value.find("  ") != std::string::npos) {
        value.erase(value.find("  "), 1);
    }
    if (value.size() > max_len) value = value.substr(0, max_len - 3) + "...";
    return value;
}

struct SourceRange {
    bool present = false;
    std::string file;
    std::size_t begin = 0;
    std::size_t end = 0;
};

SourceRange range_from(const JsonValue& object, std::string_view name = "range") {
    const JsonValue* range = field(object, name);
    if (!range || range->type != JsonValue::Type::Object) return {};
    return SourceRange{true, string_field(*range, "file"), number_field(*range, "begin"), number_field(*range, "end")};
}

struct SourceLine {
    std::size_t line = 0;
    std::size_t column = 0;
    std::string text;
};

std::vector<std::size_t> line_starts_for(std::string_view text) {
    std::vector<std::size_t> starts{0};
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && i + 1 < text.size()) starts.push_back(i + 1);
    }
    return starts;
}

std::size_t line_index_for_offset(const std::vector<std::size_t>& starts, std::size_t offset) {
    auto it = std::upper_bound(starts.begin(), starts.end(), offset);
    if (it == starts.begin()) return 0;
    return static_cast<std::size_t>(std::distance(starts.begin(), it) - 1);
}

std::optional<SourceLine> source_line_for(const SourceRange& range) {
    if (!range.present || range.file.empty()) return std::nullopt;
    std::string text;
    if (!read_text_file(range.file, text) || range.begin > text.size()) return std::nullopt;

    std::size_t line_start = 0;
    std::size_t line = 1;
    for (std::size_t i = 0; i < range.begin && i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    }
    std::size_t line_end = text.find('\n', range.begin);
    if (line_end == std::string::npos) line_end = text.size();
    return SourceLine{line, range.begin - line_start + 1, text.substr(line_start, line_end - line_start)};
}

std::string format_range(const SourceRange& range) {
    if (!range.present) return "no source range";
    std::ostringstream out;
    out << range.file;
    if (auto line = source_line_for(range)) out << ':' << line->line << ':' << line->column;
    out << " (bytes " << range.begin << '-' << range.end << ')';
    return out.str();
}

struct Guard {
    std::string name;
    std::string status;
    std::string message;
};

struct Evidence {
    std::string id;
    std::string rule_id;
    std::string subject;
    std::string rationale;
    SourceRange range;
    std::vector<std::string> fact_ids;
    std::vector<Guard> guards;
};

Evidence evidence_from_json(const JsonValue& object) {
    Evidence evidence;
    evidence.id = string_field(object, "id");
    evidence.rule_id = string_field(object, "rule_id");
    evidence.subject = string_field(object, "subject");
    evidence.rationale = string_field(object, "rationale");
    evidence.range = range_from(object);
    if (const JsonValue* facts = array_field(object, "fact_ids")) {
        for (const JsonValue& fact : facts->array) {
            if (fact.type == JsonValue::Type::String) evidence.fact_ids.push_back(fact.string);
        }
    }
    if (const JsonValue* guards = array_field(object, "guards")) {
        for (const JsonValue& guard : guards->array) {
            if (guard.type != JsonValue::Type::Object) continue;
            evidence.guards.push_back(Guard{
                string_field(guard, "name"),
                string_field(guard, "status"),
                string_field(guard, "message")});
        }
    }
    return evidence;
}

enum class ItemKind {
    Edit,
    Finding,
    Conflict,
    Diagnostic
};

std::string_view kind_label(ItemKind kind) {
    switch (kind) {
        case ItemKind::Edit: return "edit";
        case ItemKind::Finding: return "manual";
        case ItemKind::Conflict: return "conflict";
        case ItemKind::Diagnostic: return "diagnostic";
    }
    return "item";
}

struct ReviewItem {
    ItemKind kind = ItemKind::Edit;
    std::string id;
    std::string rule_id;
    std::string title;
    std::string message;
    std::string severity;
    std::string confidence;
    SourceRange range;
    std::string replacement;
    std::string evidence_id;
};

enum class Decision {
    Unset,
    Accepted,
    Rejected
};

std::string_view decision_label(Decision decision) {
    switch (decision) {
        case Decision::Unset: return "unset";
        case Decision::Accepted: return "accepted";
        case Decision::Rejected: return "rejected";
    }
    return "unset";
}

Decision decision_from_string(std::string_view value) {
    if (value == "accepted") return Decision::Accepted;
    if (value == "rejected") return Decision::Rejected;
    return Decision::Unset;
}

struct ReviewPlan {
    std::filesystem::path plan_path;
    std::filesystem::path state_path;
    JsonValue root;
    std::string target;
    std::string action;
    bool has_errors = false;
    std::size_t accepted_edit_count = 0;
    std::size_t conflict_count = 0;
    std::vector<ReviewItem> items;
    std::map<std::string, Evidence> evidence;
    std::map<std::string, Decision> decisions;
};

bool finding_is_planned_edit(const JsonValue& finding) {
    const auto attrs = string_attributes(finding);
    const auto it = attrs.find("edit_status");
    return it != attrs.end() && it->second == "planned";
}

ReviewPlan load_plan(const std::filesystem::path& input_path) {
    std::error_code ec;
    std::filesystem::path plan_path = input_path;
    if (std::filesystem::is_directory(plan_path, ec)) plan_path /= "plan.json";

    std::string text;
    if (!read_text_file(plan_path, text)) throw std::runtime_error("failed to read plan: " + plan_path.string());

    ReviewPlan plan;
    plan.plan_path = plan_path;
    plan.state_path = plan_path.parent_path() / "review.json";
    plan.root = JsonParser(text).parse();
    if (plan.root.type != JsonValue::Type::Object) throw std::runtime_error("plan.json must be an object");
    plan.target = string_field(plan.root, "target");
    plan.action = string_field(plan.root, "action");
    plan.has_errors = bool_field(plan.root, "has_errors");
    plan.accepted_edit_count = number_field(plan.root, "accepted_edit_count");
    plan.conflict_count = number_field(plan.root, "conflict_count");

    if (const JsonValue* evidence_array = array_field(plan.root, "evidence")) {
        for (const JsonValue& value : evidence_array->array) {
            if (value.type != JsonValue::Type::Object) continue;
            Evidence evidence = evidence_from_json(value);
            if (!evidence.id.empty()) plan.evidence.emplace(evidence.id, std::move(evidence));
        }
    }

    if (const JsonValue* edits = array_field(plan.root, "edits")) {
        for (const JsonValue& edit : edits->array) {
            if (edit.type != JsonValue::Type::Object) continue;
            ReviewItem item;
            item.kind = ItemKind::Edit;
            item.id = string_field(edit, "id");
            item.rule_id = string_field(edit, "rule_id");
            item.confidence = string_field(edit, "confidence");
            item.evidence_id = string_field(edit, "evidence_id");
            item.range = range_from(edit);
            item.replacement = string_field(edit, "replacement");
            item.title = item.rule_id;
            item.message = item.replacement.empty() ? "remove selected source range" : "replace with: " + compact(item.replacement, 120);
            if (!item.id.empty()) plan.items.push_back(std::move(item));
        }
    }

    if (const JsonValue* findings = array_field(plan.root, "findings")) {
        for (const JsonValue& finding : findings->array) {
            if (finding.type != JsonValue::Type::Object || finding_is_planned_edit(finding)) continue;
            ReviewItem item;
            item.kind = ItemKind::Finding;
            item.id = string_field(finding, "id");
            item.rule_id = string_field(finding, "rule_id");
            item.title = string_field(finding, "title");
            item.message = string_field(finding, "message");
            item.severity = string_field(finding, "severity");
            item.confidence = string_field(finding, "confidence");
            item.evidence_id = string_field(finding, "evidence_id");
            item.range = range_from(finding);
            if (!item.id.empty()) plan.items.push_back(std::move(item));
        }
    }

    if (const JsonValue* conflicts = array_field(plan.root, "edit_conflicts")) {
        std::size_t index = 0;
        for (const JsonValue& conflict : conflicts->array) {
            if (conflict.type != JsonValue::Type::Object) continue;
            ReviewItem item;
            item.kind = ItemKind::Conflict;
            item.id = "conflict:" + std::to_string(index++);
            item.title = "edit conflict";
            item.message = string_field(conflict, "message", "overlapping or invalid edit");
            item.severity = "error";
            item.range = range_from(conflict, "proposed_range");
            plan.items.push_back(std::move(item));
        }
    }

    if (const JsonValue* diagnostics = array_field(plan.root, "diagnostics")) {
        std::size_t index = 0;
        for (const JsonValue& diagnostic : diagnostics->array) {
            if (diagnostic.type != JsonValue::Type::Object) continue;
            ReviewItem item;
            item.kind = ItemKind::Diagnostic;
            item.id = "diagnostic:" + std::to_string(index++);
            item.rule_id = string_field(diagnostic, "code");
            item.title = string_field(diagnostic, "code");
            item.message = string_field(diagnostic, "message");
            item.severity = string_field(diagnostic, "severity");
            item.range = range_from(diagnostic);
            plan.items.push_back(std::move(item));
        }
    }

    return plan;
}

void load_state(ReviewPlan& plan) {
    std::string text;
    if (!read_text_file(plan.state_path, text)) return;
    JsonValue root;
    try {
        root = JsonParser(text).parse();
    } catch (...) {
        return;
    }
    const JsonValue* decisions = array_field(root, "decisions");
    if (!decisions) return;
    for (const JsonValue& item : decisions->array) {
        if (item.type != JsonValue::Type::Object) continue;
        const std::string id = string_field(item, "id");
        if (!id.empty()) plan.decisions[id] = decision_from_string(string_field(item, "decision"));
    }
}

bool save_state(const ReviewPlan& plan) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"plan\": \"" << json_escape(plan.plan_path.string()) << "\",\n";
    out << "  \"decisions\": [\n";
    bool first = true;
    for (const auto& [id, decision] : plan.decisions) {
        if (decision == Decision::Unset) continue;
        if (!first) out << ",\n";
        first = false;
        out << "    {\"id\": \"" << json_escape(id) << "\", \"decision\": \"" << decision_label(decision) << "\"}";
    }
    out << "\n  ]\n";
    out << "}\n";
    return write_text_file(plan.state_path, out.str());
}

bool export_reviewed_plan(const ReviewPlan& plan, std::filesystem::path out_path) {
    if (out_path.empty()) out_path = plan.plan_path.parent_path() / "plan.reviewed.json";
    JsonValue root = plan.root;
    JsonValue* edits = nullptr;
    if (root.type == JsonValue::Type::Object) {
        auto it = root.object.find("edits");
        if (it != root.object.end() && it->second.type == JsonValue::Type::Array) edits = &it->second;
    }
    if (edits) {
        std::vector<JsonValue> filtered;
        for (const JsonValue& edit : edits->array) {
            const std::string id = string_field(edit, "id");
            const auto decision = plan.decisions.find(id);
            if (decision != plan.decisions.end() && decision->second == Decision::Rejected) continue;
            filtered.push_back(edit);
        }
        edits->array = std::move(filtered);
        if (auto count = root.object.find("accepted_edit_count"); count != root.object.end()) {
            count->second.type = JsonValue::Type::Number;
            count->second.number = edits->array.size();
        }
    }
    return write_text_file(out_path, json_to_string(root) + "\n");
}

enum class Filter {
    All,
    Edits,
    Manual,
    Conflicts,
    Diagnostics
};

std::string_view filter_label(Filter filter) {
    switch (filter) {
        case Filter::All: return "all";
        case Filter::Edits: return "edits";
        case Filter::Manual: return "manual";
        case Filter::Conflicts: return "conflicts";
        case Filter::Diagnostics: return "diagnostics";
    }
    return "all";
}

Filter next_filter(Filter filter) {
    switch (filter) {
        case Filter::All: return Filter::Edits;
        case Filter::Edits: return Filter::Manual;
        case Filter::Manual: return Filter::Conflicts;
        case Filter::Conflicts: return Filter::Diagnostics;
        case Filter::Diagnostics: return Filter::All;
    }
    return Filter::All;
}

bool passes_filter(const ReviewItem& item, Filter filter) {
    switch (filter) {
        case Filter::All: return true;
        case Filter::Edits: return item.kind == ItemKind::Edit;
        case Filter::Manual: return item.kind == ItemKind::Finding;
        case Filter::Conflicts: return item.kind == ItemKind::Conflict;
        case Filter::Diagnostics: return item.kind == ItemKind::Diagnostic;
    }
    return true;
}

std::vector<std::size_t> visible_items(const ReviewPlan& plan, Filter filter) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        if (passes_filter(plan.items[i], filter)) out.push_back(i);
    }
    return out;
}

Decision decision_for(const ReviewPlan& plan, const ReviewItem& item) {
    const auto it = plan.decisions.find(item.id);
    return it == plan.decisions.end() ? Decision::Unset : it->second;
}

std::size_t count_kind(const ReviewPlan& plan, ItemKind kind) {
    return static_cast<std::size_t>(std::count_if(plan.items.begin(), plan.items.end(), [&](const ReviewItem& item) {
        return item.kind == kind;
    }));
}

std::vector<std::string> split_lines_preserve_newlines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t next = text.find('\n', pos);
        if (next == std::string_view::npos) {
            lines.emplace_back(text.substr(pos));
            break;
        }
        lines.emplace_back(text.substr(pos, next - pos + 1));
        pos = next + 1;
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
}

enum class DiffOpKind {
    Equal,
    Delete,
    Insert
};

struct DiffOp {
    DiffOpKind kind = DiffOpKind::Equal;
    std::string line;
};

std::vector<DiffOp> line_diff(std::string_view old_text, std::string_view new_text) {
    const auto old_lines = split_lines_preserve_newlines(old_text);
    const auto new_lines = split_lines_preserve_newlines(new_text);
    const std::size_t rows = old_lines.size() + 1;
    const std::size_t cols = new_lines.size() + 1;
    std::vector<std::size_t> dp(rows * cols, 0);
    auto cell = [&](std::size_t row, std::size_t col) -> std::size_t& {
        return dp[row * cols + col];
    };

    for (std::size_t i = old_lines.size(); i-- > 0;) {
        for (std::size_t j = new_lines.size(); j-- > 0;) {
            cell(i, j) = old_lines[i] == new_lines[j] ? cell(i + 1, j + 1) + 1
                                                       : std::max(cell(i + 1, j), cell(i, j + 1));
        }
    }

    std::vector<DiffOp> ops;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < old_lines.size() || j < new_lines.size()) {
        if (i < old_lines.size() && j < new_lines.size() && old_lines[i] == new_lines[j]) {
            ops.push_back(DiffOp{DiffOpKind::Equal, old_lines[i]});
            ++i;
            ++j;
        } else if (i < old_lines.size() && (j == new_lines.size() || cell(i + 1, j) >= cell(i, j + 1))) {
            ops.push_back(DiffOp{DiffOpKind::Delete, old_lines[i]});
            ++i;
        } else {
            ops.push_back(DiffOp{DiffOpKind::Insert, new_lines[j]});
            ++j;
        }
    }
    return ops;
}

std::size_t line_count(std::string_view text) {
    return split_lines_preserve_newlines(text).size();
}

std::string unified_range(std::size_t start, std::size_t count) {
    if (count == 0) return std::to_string(start == 0 ? 0 : start - 1) + ",0";
    if (count == 1) return std::to_string(start);
    return std::to_string(start) + "," + std::to_string(count);
}

std::vector<std::string> selected_edit_patch(const ReviewItem& item) {
    if (item.kind != ItemKind::Edit || !item.range.present) return {};
    std::string original;
    if (!read_text_file(item.range.file, original)) return {};
    if (item.range.begin > item.range.end || item.range.end > original.size()) return {};

    const auto starts = line_starts_for(original);
    const std::size_t change_start = line_index_for_offset(starts, item.range.begin);
    const std::size_t change_end_offset = item.range.end == 0 ? 0 : item.range.end - 1;
    const std::size_t change_end = line_index_for_offset(starts, std::min(change_end_offset, original.size()));
    constexpr std::size_t context = 2;
    const std::size_t window_start_line = change_start > context ? change_start - context : 0;
    const std::size_t window_end_line = std::min(starts.size() - 1, change_end + context);
    const std::size_t window_begin = starts[window_start_line];
    const std::size_t window_end = window_end_line + 1 < starts.size() ? starts[window_end_line + 1] : original.size();

    std::string old_window = original.substr(window_begin, window_end - window_begin);
    std::string new_window = old_window;
    new_window.replace(item.range.begin - window_begin, item.range.end - item.range.begin, item.replacement);

    std::vector<std::string> lines;
    lines.push_back("diff --git a/" + item.range.file + " b/" + item.range.file);
    lines.push_back("--- a/" + item.range.file);
    lines.push_back("+++ b/" + item.range.file);
    lines.push_back("@@ -" + unified_range(window_start_line + 1, line_count(old_window)) + " +" +
                    unified_range(window_start_line + 1, line_count(new_window)) + " @@");
    for (const auto& op : line_diff(old_window, new_window)) {
        const char prefix = op.kind == DiffOpKind::Equal ? ' ' : op.kind == DiffOpKind::Delete ? '-' : '+';
        std::string line = op.line;
        if (!line.empty() && line.back() == '\n') line.pop_back();
        lines.push_back(std::string(1, prefix) + compact(line, 116));
    }
    return lines;
}

struct RawTerminal {
    RawTerminal() {
#ifndef _WIN32
        active_ = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &original_) == 0;
        if (active_) {
            termios raw = original_;
            raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        }
#endif
    }

    ~RawTerminal() {
#ifndef _WIN32
        if (active_) tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
#endif
    }

private:
#ifndef _WIN32
    bool active_ = false;
    termios original_{};
#endif
};

enum class Key {
    Up,
    Down,
    Left,
    Right,
    Other
};

struct InputKey {
    Key key = Key::Other;
    char ch = '\0';
};

InputKey read_key() {
#ifdef _WIN32
    const int c = _getch();
    if (c == 224 || c == 0) {
        const int arrow = _getch();
        if (arrow == 72) return {Key::Up, 0};
        if (arrow == 80) return {Key::Down, 0};
        if (arrow == 75) return {Key::Left, 0};
        if (arrow == 77) return {Key::Right, 0};
    }
    return {Key::Other, static_cast<char>(c)};
#else
    char c = '\0';
    if (::read(STDIN_FILENO, &c, 1) != 1) return {};
    if (c == '\x1b') {
        char seq[2] = {};
        if (::read(STDIN_FILENO, &seq[0], 1) != 1) return {Key::Other, c};
        if (::read(STDIN_FILENO, &seq[1], 1) != 1) return {Key::Other, c};
        if (seq[0] == '[') {
            if (seq[1] == 'A') return {Key::Up, 0};
            if (seq[1] == 'B') return {Key::Down, 0};
            if (seq[1] == 'C') return {Key::Right, 0};
            if (seq[1] == 'D') return {Key::Left, 0};
        }
        return {Key::Other, c};
    }
    return {Key::Other, c};
#endif
}

int terminal_lines() {
    const char* lines = std::getenv("LINES");
    if (!lines) return 28;
    const int parsed = std::atoi(lines);
    return parsed < 16 ? 16 : parsed;
}

std::string colour(std::string_view code, std::string_view value) {
    return "\x1b[" + std::string(code) + "m" + std::string(value) + "\x1b[0m";
}

std::string dim(std::string_view value) {
    return colour("2", value);
}

std::string bold(std::string_view value) {
    return colour("1", value);
}

std::string pad_right(std::string value, std::size_t width) {
    if (value.size() > width) value = compact(std::move(value), width);
    if (value.size() < width) value.append(width - value.size(), ' ');
    return value;
}

std::string colour_kind(ItemKind kind, std::string value) {
    switch (kind) {
        case ItemKind::Edit: return colour("36", value);
        case ItemKind::Finding: return colour("35", value);
        case ItemKind::Conflict: return colour("31", value);
        case ItemKind::Diagnostic: return colour("33", value);
    }
    return value;
}

std::string colour_confidence(std::string_view value) {
    std::string_view token = value;
    while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
    if (token == "high") return colour("32", value);
    if (token == "medium") return colour("33", value);
    if (token == "low") return colour("31", value);
    if (token == "error") return colour("31", value);
    if (token == "warning") return colour("33", value);
    if (token == "note") return colour("36", value);
    return std::string(value);
}

std::string colour_decision(Decision decision) {
    switch (decision) {
        case Decision::Accepted: return colour("32", decision_label(decision));
        case Decision::Rejected: return colour("31", decision_label(decision));
        case Decision::Unset: return colour("2", decision_label(decision));
    }
    return std::string(decision_label(decision));
}

std::string colour_diff_line(std::string_view line) {
    if (line.rfind("diff --git", 0) == 0) return colour("1;36", line);
    if (line.rfind("@@", 0) == 0) return colour("35", line);
    if (line.rfind("+++", 0) == 0 || line.rfind("---", 0) == 0) return colour("36", line);
    if (!line.empty() && line.front() == '+') return colour("32", line);
    if (!line.empty() && line.front() == '-') return colour("31", line);
    return dim(line);
}

std::string badge_for(const ReviewItem& item, Decision decision) {
    if (decision == Decision::Accepted) return colour("32", "A");
    if (decision == Decision::Rejected) return colour("31", "R");
    if (item.kind == ItemKind::Conflict) return colour("31", "!");
    if (item.kind == ItemKind::Diagnostic) return colour("33", "D");
    if (item.kind == ItemKind::Finding) return colour("35", "M");
    return colour("36", "E");
}

void render(const ReviewPlan& plan, Filter filter, std::size_t selected, std::string_view status) {
    const std::vector<std::size_t> visible = visible_items(plan, filter);
    const std::size_t selected_item = visible.empty() ? 0 : visible[std::min(selected, visible.size() - 1)];

    std::cout << "\x1b[2J\x1b[H";
    std::cout << bold("Moult Review") << "  " << dim(plan.plan_path.string()) << "\n";
    std::cout << dim("target=") << colour("36", plan.target)
              << "  " << dim("action=") << plan.action
              << "  " << dim("status=") << (plan.has_errors ? colour("31", "errors") : colour("32", "ok"))
              << "  " << dim("filter=") << colour("1;34", filter_label(filter)) << "\n";
    std::cout << colour("36", "edits=" + std::to_string(count_kind(plan, ItemKind::Edit)))
              << "  " << colour("35", "manual=" + std::to_string(count_kind(plan, ItemKind::Finding)))
              << "  " << colour("31", "conflicts=" + std::to_string(count_kind(plan, ItemKind::Conflict)))
              << "  " << colour("33", "diagnostics=" + std::to_string(count_kind(plan, ItemKind::Diagnostic))) << "\n";
    std::cout << dim("keys:") << " " << colour("1", "j/k") << " or arrows move  "
              << colour("1", "tab") << " filter  "
              << colour("32", "a") << " accept  "
              << colour("31", "r") << " reject  "
              << colour("33", "u") << " unset  "
              << colour("36", "w") << " save  "
              << colour("36", "x") << " export  "
              << colour("1", "q") << " quit\n";
    std::cout << dim(std::string(100, '-')) << "\n";

    const int height = terminal_lines();
    const std::size_t list_rows = static_cast<std::size_t>(std::max(5, height - 23));
    const std::size_t first = selected >= list_rows ? selected - list_rows + 1 : 0;
    for (std::size_t row = 0; row < list_rows; ++row) {
        const std::size_t visible_index = first + row;
        if (visible_index >= visible.size()) {
            std::cout << "\n";
            continue;
        }
        const ReviewItem& item = plan.items[visible[visible_index]];
        const bool active = visible_index == selected;
        const std::string marker = active ? colour("7", ">") : " ";
        const Decision decision = decision_for(plan, item);
        const std::string signal = item.confidence.empty() ? item.severity : item.confidence;
        std::cout << marker << ' ' << badge_for(item, decision) << ' '
                  << colour_kind(item.kind, pad_right(std::string(kind_label(item.kind)), 10)) << ' '
                  << pad_right(item.rule_id.empty() ? item.title : item.rule_id, 28) << ' '
                  << colour_confidence(pad_right(signal, 10)) << ' '
                  << dim(compact(format_range(item.range), 62)) << "\n";
    }

    std::cout << dim(std::string(100, '-')) << "\n";
    if (!visible.empty()) {
        const ReviewItem& item = plan.items[selected_item];
        std::cout << colour_kind(item.kind, std::string(kind_label(item.kind))) << "  " << bold(item.id)
                  << "  " << dim("decision=") << colour_decision(decision_for(plan, item)) << "\n";
        std::cout << dim("title:") << ' ' << compact(item.title.empty() ? item.rule_id : item.title, 120) << "\n";
        std::cout << dim("where:") << ' ' << colour("36", compact(format_range(item.range), 120)) << "\n";
        if (auto source = source_line_for(item.range)) {
            std::cout << dim("code :") << ' ' << compact(source->text, 120) << "\n";
        }
        if (!item.replacement.empty()) std::cout << dim("edit :") << ' ' << colour("32", compact(item.replacement, 120)) << "\n";
        if (!item.message.empty()) std::cout << dim("note :") << ' ' << compact(item.message, 120) << "\n";
        if (!item.evidence_id.empty()) {
            const auto it = plan.evidence.find(item.evidence_id);
            if (it != plan.evidence.end()) {
                const Evidence& evidence = it->second;
                std::cout << dim("evid :") << ' ' << compact(evidence.subject + " - " + evidence.rationale, 120) << "\n";
                if (!evidence.guards.empty()) {
                    std::cout << dim("guard:");
                    for (const Guard& guard : evidence.guards) {
                        std::cout << ' ' << guard.name << '='
                                  << (guard.status == "passed" ? colour("32", guard.status) : colour("33", guard.status));
                    }
                    std::cout << "\n";
                }
            }
        }
        if (item.kind == ItemKind::Edit) {
            const auto patch = selected_edit_patch(item);
            if (!patch.empty()) {
                std::cout << colour("1;36", "patch") << "\n";
                std::size_t printed = 0;
                for (const auto& line : patch) {
                    if (printed++ >= 9) {
                        std::cout << dim("...") << "\n";
                        break;
                    }
                    std::cout << colour_diff_line(compact(line, 120)) << "\n";
                }
            }
        }
    } else {
        std::cout << dim("no items for this filter") << "\n";
    }
    if (!status.empty()) std::cout << colour("1;36", status) << "\n";
    std::cout.flush();
}

void set_decision(ReviewPlan& plan, const std::vector<std::size_t>& visible, std::size_t selected, Decision decision) {
    if (visible.empty()) return;
    ReviewItem& item = plan.items[visible[std::min(selected, visible.size() - 1)]];
    if (decision == Decision::Unset) {
        plan.decisions.erase(item.id);
    } else {
        plan.decisions[item.id] = decision;
    }
}

} // namespace

int run_review_tui(const std::filesystem::path& input_path) {
    ReviewPlan plan;
    try {
        plan = load_plan(input_path);
        load_state(plan);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }

#ifndef _WIN32
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::cerr << "moult review requires an interactive terminal\n";
        return 2;
    }
#endif

    RawTerminal raw;
    Filter filter = Filter::All;
    std::size_t selected = 0;
    std::string status;

    for (;;) {
        const std::vector<std::size_t> visible = visible_items(plan, filter);
        if (!visible.empty() && selected >= visible.size()) selected = visible.size() - 1;
        render(plan, filter, selected, status);
        status.clear();

        const InputKey key = read_key();
        if (key.key == Key::Down || key.ch == 'j') {
            if (selected + 1 < visible.size()) ++selected;
        } else if (key.key == Key::Up || key.ch == 'k') {
            if (selected > 0) --selected;
        } else if (key.ch == '\t') {
            filter = next_filter(filter);
            selected = 0;
        } else if (key.ch == 'a') {
            set_decision(plan, visible, selected, Decision::Accepted);
        } else if (key.ch == 'r') {
            set_decision(plan, visible, selected, Decision::Rejected);
        } else if (key.ch == 'u') {
            set_decision(plan, visible, selected, Decision::Unset);
        } else if (key.ch == 'w') {
            status = save_state(plan) ? "saved " + plan.state_path.string() : "failed to save " + plan.state_path.string();
        } else if (key.ch == 'x') {
            const auto out_path = plan.plan_path.parent_path() / "plan.reviewed.json";
            const bool state_ok = save_state(plan);
            const bool export_ok = export_reviewed_plan(plan, out_path);
            status = state_ok && export_ok ? "saved review.json and exported " + out_path.string()
                                           : "failed to export reviewed plan";
        } else if (key.ch == 'q' || key.ch == '\x03') {
            std::cout << "\x1b[2J\x1b[H";
            return 0;
        }
    }
}

} // namespace moult::cli

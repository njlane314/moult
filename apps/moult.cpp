#include "moult/core/api.hpp"
#include "moult/cpp/modernization.hpp"
#include "moult_review.hpp"
#ifdef MOULT_HAVE_CLANG_ADAPTER
#include "moult/clang/adapter.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class Command {
    Scan,
    Plan,
    Apply,
    Report,
    Diff,
    Review
};

struct CliOptions {
    Command command = Command::Plan;
    moult::core::PlanAction action = moult::core::PlanAction::Plan;
    std::string target = std::string(moult::cpp_modernization::target_name);
    std::optional<std::filesystem::path> output_dir;
    std::optional<std::filesystem::path> compile_commands_path;
    moult::core::Confidence minimum_confidence = moult::core::Confidence::High;
    bool include_facts = false;
    bool dry_run = false;
    bool backup = false;
    std::string adapter = "textual";
    std::vector<std::string> clang_args;
    std::vector<std::filesystem::path> inputs;
};

struct CompileCommandEntry {
    std::filesystem::path source_file;
    std::filesystem::path directory;
    std::vector<std::string> args;
};

void print_usage(std::ostream& out) {
    out << "usage: moult <scan|plan> [options] [file-or-directory]...\n"
        << "       moult apply [options] <plan.json>\n"
        << "       moult report <plan.json>\n"
        << "       moult diff <plan.json>\n"
        << "       moult review <plan.json-or-output-directory>\n"
        << "\n"
        << "options:\n"
        << "  --target <name>              target to run (default: cpp-modernisation)\n"
        << "  --adapter <name>             textual or clang"
#ifdef MOULT_HAVE_CLANG_ADAPTER
        << " (default: clang)\n"
#else
        << " (default: textual; clang not built)\n"
#endif
        << "  --clang-arg <arg>            extra argument for libclang; repeat as needed\n"
        << "  --compile-commands <path>    compile_commands.json path or build directory; used as input if no paths are given\n"
        << "  --out <directory>            write plan.json, facts.json, evidence.jsonl, findings.sarif\n"
        << "  --min-confidence <level>     low, medium, high, or proven (default: high)\n"
        << "  --include-facts              include facts inside plan.json/stdout JSON\n"
        << "  --dry-run                    for apply, list files that would change\n"
        << "  --backup                     for apply, write <file>.moult.bak before changing files\n"
        << "  --format json                accepted for compatibility; JSON is the only format today\n"
        << "  --help                       show this help\n";
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool is_cpp_source_like(const std::filesystem::path& path) {
    const std::string ext = lowercase(path.extension().string());
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".h" || ext == ".hh" ||
           ext == ".hpp" || ext == ".hxx" || ext == ".ipp" || ext == ".ixx";
}

bool collect_input_files(const std::filesystem::path& input, std::vector<std::filesystem::path>& files) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(input, ec)) {
        files.push_back(input);
        return true;
    }
    if (!std::filesystem::is_directory(input, ec)) return false;

    for (std::filesystem::recursive_directory_iterator it(input, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (is_cpp_source_like(it->path())) files.push_back(it->path());
    }
    return !ec;
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

const JsonValue& object_field(const JsonValue& object, std::string_view name) {
    if (object.type != JsonValue::Type::Object) throw std::runtime_error("expected JSON object");
    const auto it = object.object.find(std::string(name));
    if (it == object.object.end()) throw std::runtime_error("missing JSON field: " + std::string(name));
    return it->second;
}

const JsonValue* optional_object_field(const JsonValue& object, std::string_view name) {
    if (object.type != JsonValue::Type::Object) return nullptr;
    const auto it = object.object.find(std::string(name));
    return it == object.object.end() ? nullptr : &it->second;
}

std::string json_string_field(const JsonValue& object, std::string_view name) {
    const JsonValue& value = object_field(object, name);
    if (value.type != JsonValue::Type::String) throw std::runtime_error("expected string field: " + std::string(name));
    return value.string;
}

std::string optional_json_string_field(const JsonValue& object, std::string_view name, std::string fallback = {}) {
    const JsonValue* value = optional_object_field(object, name);
    if (!value) return fallback;
    if (value->type != JsonValue::Type::String) throw std::runtime_error("expected string field: " + std::string(name));
    return value->string;
}

std::size_t json_number_field(const JsonValue& object, std::string_view name) {
    const JsonValue& value = object_field(object, name);
    if (value.type != JsonValue::Type::Number) throw std::runtime_error("expected number field: " + std::string(name));
    return value.number;
}

bool json_bool_field(const JsonValue& object, std::string_view name) {
    const JsonValue& value = object_field(object, name);
    if (value.type != JsonValue::Type::Bool) throw std::runtime_error("expected bool field: " + std::string(name));
    return value.boolean;
}

const JsonValue& json_array_field(const JsonValue& object, std::string_view name) {
    const JsonValue& value = object_field(object, name);
    if (value.type != JsonValue::Type::Array) throw std::runtime_error("expected array field: " + std::string(name));
    return value;
}

std::map<std::string, std::string> json_string_attributes(const JsonValue& object) {
    std::map<std::string, std::string> out;
    const JsonValue* attrs = optional_object_field(object, "attributes");
    if (!attrs || attrs->type != JsonValue::Type::Object) return out;
    for (const auto& [key, value] : attrs->object) {
        if (value.type == JsonValue::Type::String) out.emplace(key, value.string);
    }
    return out;
}

std::vector<std::string> split_command_line(std::string_view command) {
    std::vector<std::string> out;
    std::string current;
    char quote = '\0';
    bool escaping = false;
    for (char c : command) {
        if (escaping) {
            current.push_back(c);
            escaping = false;
            continue;
        }
        if (c == '\\') {
            escaping = true;
            continue;
        }
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else {
                current.push_back(c);
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) out.push_back(std::move(current));
    return out;
}

bool same_path_loose(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    const auto ca = std::filesystem::weakly_canonical(std::filesystem::absolute(a), ec);
    if (ec) return a == b || a.filename() == b.filename();
    const auto cb = std::filesystem::weakly_canonical(std::filesystem::absolute(b), ec);
    if (ec) return a == b || a.filename() == b.filename();
    return ca == cb;
}

std::filesystem::path absolute_from_directory(const std::filesystem::path& path, const std::filesystem::path& directory) {
    if (path.empty() || path.is_absolute() || directory.empty()) return path;
    return directory / path;
}

bool is_compile_source_argument(const std::string& arg,
                                const std::filesystem::path& source_file,
                                const std::filesystem::path& directory) {
    if (arg.empty() || arg[0] == '-') return false;
    const std::filesystem::path argument_path = arg;
    if (same_path_loose(argument_path, source_file)) return true;
    if (!directory.empty() && same_path_loose(absolute_from_directory(argument_path, directory), source_file)) return true;
    return false;
}

std::string absolute_joined_path_argument(std::string_view prefix,
                                          const std::string& arg,
                                          const std::filesystem::path& directory) {
    if (directory.empty() || arg.size() <= prefix.size()) return arg;
    const std::filesystem::path path(arg.substr(prefix.size()));
    if (path.is_absolute()) return arg;
    return std::string(prefix) + (directory / path).string();
}

std::string absolute_separate_path_argument(const std::string& arg, const std::filesystem::path& directory) {
    if (directory.empty()) return arg;
    const std::filesystem::path path(arg);
    return path.is_absolute() ? arg : (directory / path).string();
}

bool option_has_separate_path_argument(std::string_view arg) {
    return arg == "-I" || arg == "-isystem" || arg == "-iquote" || arg == "-idirafter" || arg == "-include" ||
           arg == "-imacros" || arg == "-F" || arg == "-iframework" || arg == "-isysroot";
}

std::vector<std::string> sanitize_compile_command(std::vector<std::string> args,
                                                  const std::filesystem::path& source_file,
                                                  const std::filesystem::path& directory) {
    std::vector<std::string> out;
    if (args.empty()) return out;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-c") continue;
        if (arg == "-o" && i + 1 < args.size()) {
            ++i;
            continue;
        }
        if (arg.rfind("-o", 0) == 0 && arg.size() > 2) continue;
        if (is_compile_source_argument(arg, source_file, directory)) continue;
        if (option_has_separate_path_argument(arg) && i + 1 < args.size()) {
            out.push_back(arg);
            out.push_back(absolute_separate_path_argument(args[++i], directory));
            continue;
        }
        if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
            out.push_back(absolute_joined_path_argument("-I", arg, directory));
            continue;
        }
        if (arg.rfind("-F", 0) == 0 && arg.size() > 2) {
            out.push_back(absolute_joined_path_argument("-F", arg, directory));
            continue;
        }
        if (arg.rfind("--sysroot=", 0) == 0) {
            out.push_back(absolute_joined_path_argument("--sysroot=", arg, directory));
            continue;
        }
        out.push_back(arg);
    }
    return out;
}

std::vector<CompileCommandEntry> parse_compile_commands_file(const std::filesystem::path& path) {
    std::string text;
    if (!read_text_file(path, text)) throw std::runtime_error("failed to read compile commands: " + path.string());
    JsonValue root = JsonParser(text).parse();
    if (root.type != JsonValue::Type::Array) throw std::runtime_error("compile_commands.json must be an array");

    const std::filesystem::path database_directory = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    std::vector<CompileCommandEntry> out;
    for (const JsonValue& entry : root.array) {
        if (entry.type != JsonValue::Type::Object) continue;
        const std::string directory = optional_object_field(entry, "directory") &&
                                              optional_object_field(entry, "directory")->type == JsonValue::Type::String
                                          ? optional_object_field(entry, "directory")->string
                                          : std::string();
        std::filesystem::path command_directory = directory.empty() ? database_directory : std::filesystem::path(directory);
        if (!command_directory.is_absolute()) command_directory = database_directory / command_directory;
        const std::filesystem::path file_field = json_string_field(entry, "file");
        const std::filesystem::path source_file = file_field.is_absolute() ? file_field : command_directory / file_field;

        std::vector<std::string> args;
        if (const JsonValue* arguments = optional_object_field(entry, "arguments")) {
            if (arguments->type != JsonValue::Type::Array) throw std::runtime_error("compile command arguments must be an array");
            for (const JsonValue& arg : arguments->array) {
                if (arg.type != JsonValue::Type::String) throw std::runtime_error("compile command argument must be a string");
                args.push_back(arg.string);
            }
        } else if (const JsonValue* command = optional_object_field(entry, "command")) {
            if (command->type != JsonValue::Type::String) throw std::runtime_error("compile command must be a string");
            args = split_command_line(command->string);
        } else {
            continue;
        }

        out.push_back(CompileCommandEntry{
            source_file,
            command_directory,
            sanitize_compile_command(std::move(args), source_file, command_directory)});
    }
    return out;
}

std::vector<std::filesystem::path> source_files_from_compile_commands(const std::vector<CompileCommandEntry>& entries) {
    std::vector<std::filesystem::path> out;
    out.reserve(entries.size());
    for (const auto& entry : entries) out.push_back(entry.source_file);
    return out;
}

class SourceLineCache {
public:
    std::optional<moult::core::LineColumn> line_column(const std::string& file, std::size_t offset) {
        auto it = sources_.find(file);
        if (it == sources_.end()) {
            std::string text;
            if (read_text_file(file, text)) {
                it = sources_.emplace(file, moult::core::SourceBuffer(file, std::move(text))).first;
            } else {
                missing_.insert(file);
                return std::nullopt;
            }
        }
        if (!it->second.contains(offset)) return std::nullopt;
        return it->second.line_column(offset);
    }

private:
    std::map<std::string, moult::core::SourceBuffer> sources_;
    std::set<std::string> missing_;
};

std::string compact_text(std::string value) {
    for (char& c : value) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    constexpr std::size_t max_len = 96;
    if (value.size() > max_len) value = value.substr(0, max_len - 3) + "...";
    return value;
}

std::string format_range(const JsonValue& range_obj, SourceLineCache& line_cache) {
    const std::string file = json_string_field(range_obj, "file");
    const std::size_t begin = json_number_field(range_obj, "begin");
    const std::size_t end = json_number_field(range_obj, "end");
    std::ostringstream out;
    out << file;
    if (auto lc = line_cache.line_column(file, begin)) {
        out << ":" << lc->line << ":" << lc->column;
    }
    out << " (bytes " << begin << "-" << end << ")";
    return out.str();
}

std::string format_optional_range(const JsonValue& object, SourceLineCache& line_cache) {
    const JsonValue* range = optional_object_field(object, "range");
    if (!range || range->type != JsonValue::Type::Object) return "no source range";
    return format_range(*range, line_cache);
}

bool finding_is_planned_edit(const JsonValue& finding) {
    const auto attrs = json_string_attributes(finding);
    const auto it = attrs.find("edit_status");
    return it != attrs.end() && it->second == "planned";
}

void print_report_section_empty(std::ostream& out, std::string_view label) {
    out << "\n" << label << "\n";
    out << "  none\n";
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
    std::size_t old_line = 1;
    std::size_t new_line = 1;
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
            if (old_lines[i] == new_lines[j]) {
                cell(i, j) = cell(i + 1, j + 1) + 1;
            } else {
                cell(i, j) = std::max(cell(i + 1, j), cell(i, j + 1));
            }
        }
    }

    std::vector<DiffOp> ops;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < old_lines.size() || j < new_lines.size()) {
        if (i < old_lines.size() && j < new_lines.size() && old_lines[i] == new_lines[j]) {
            ops.push_back(DiffOp{DiffOpKind::Equal, old_lines[i], i + 1, j + 1});
            ++i;
            ++j;
        } else if (i < old_lines.size() && (j == new_lines.size() || cell(i + 1, j) >= cell(i, j + 1))) {
            ops.push_back(DiffOp{DiffOpKind::Delete, old_lines[i], i + 1, j + 1});
            ++i;
        } else {
            ops.push_back(DiffOp{DiffOpKind::Insert, new_lines[j], i + 1, j + 1});
            ++j;
        }
    }
    return ops;
}

bool diff_op_is_change(const DiffOp& op) {
    return op.kind == DiffOpKind::Delete || op.kind == DiffOpKind::Insert;
}

void write_diff_line(std::ostream& out, char prefix, const std::string& line) {
    out << prefix << line;
    if (line.empty() || line.back() != '\n') out << "\n";
}

std::string unified_range(std::size_t start, std::size_t count) {
    if (count == 0) return std::to_string(start == 0 ? 0 : start - 1) + ",0";
    if (count == 1) return std::to_string(start);
    return std::to_string(start) + "," + std::to_string(count);
}

void write_unified_diff(std::ostream& out,
                        const std::string& file,
                        std::string_view old_text,
                        std::string_view new_text,
                        std::size_t context_lines = 3) {
    const auto ops = line_diff(old_text, new_text);
    out << "diff --git a/" << file << " b/" << file << "\n";
    out << "--- a/" << file << "\n";
    out << "+++ b/" << file << "\n";

    std::size_t cursor = 0;
    while (cursor < ops.size()) {
        while (cursor < ops.size() && !diff_op_is_change(ops[cursor])) ++cursor;
        if (cursor >= ops.size()) break;

        std::size_t hunk_start = cursor > context_lines ? cursor - context_lines : 0;
        std::size_t last_change = cursor;
        std::size_t scan = cursor + 1;
        while (scan < ops.size()) {
            if (diff_op_is_change(ops[scan])) last_change = scan;
            if (scan > last_change + context_lines) break;
            ++scan;
        }
        const std::size_t hunk_end = std::min(ops.size(), last_change + context_lines + 1);

        std::size_t old_start = 0;
        std::size_t new_start = 0;
        std::size_t old_count = 0;
        std::size_t new_count = 0;
        for (std::size_t i = hunk_start; i < hunk_end; ++i) {
            if (ops[i].kind != DiffOpKind::Insert) {
                if (old_count == 0) old_start = ops[i].old_line;
                ++old_count;
            }
            if (ops[i].kind != DiffOpKind::Delete) {
                if (new_count == 0) new_start = ops[i].new_line;
                ++new_count;
            }
        }
        if (old_count == 0) old_start = ops[hunk_start].old_line;
        if (new_count == 0) new_start = ops[hunk_start].new_line;

        out << "@@ -" << unified_range(old_start, old_count)
            << " +" << unified_range(new_start, new_count) << " @@\n";
        for (std::size_t i = hunk_start; i < hunk_end; ++i) {
            switch (ops[i].kind) {
                case DiffOpKind::Equal: write_diff_line(out, ' ', ops[i].line); break;
                case DiffOpKind::Delete: write_diff_line(out, '-', ops[i].line); break;
                case DiffOpKind::Insert: write_diff_line(out, '+', ops[i].line); break;
            }
        }

        cursor = hunk_end;
    }
}

#ifdef MOULT_HAVE_CLANG_ADAPTER
moult::clang_adapter::ClangCppModernizationAdapter::CompileCommandMap compile_command_map_from_entries(
    const std::vector<CompileCommandEntry>& entries) {
    moult::clang_adapter::ClangCppModernizationAdapter::CompileCommandMap out;
    for (const auto& entry : entries) {
        out[entry.source_file.string()] = entry.args;
    }
    return out;
}
#endif

std::optional<std::filesystem::path> find_compile_commands(const std::vector<std::filesystem::path>& inputs) {
    std::vector<std::filesystem::path> candidates;
    auto add_candidate = [&](std::filesystem::path path) {
        candidates.push_back(std::move(path));
    };

    add_candidate("compile_commands.json");
    add_candidate(std::filesystem::path("build") / "compile_commands.json");
    for (const auto& input : inputs) {
        std::error_code ec;
        std::filesystem::path base = std::filesystem::is_directory(input, ec) ? input : input.parent_path();
        if (base.empty()) base = ".";
        for (std::filesystem::path cur = base; !cur.empty(); cur = cur.parent_path()) {
            add_candidate(cur / "compile_commands.json");
            add_candidate(cur / "build" / "compile_commands.json");
            if (cur == cur.root_path() || cur == ".") break;
        }
    }

    std::set<std::filesystem::path> seen;
    for (const auto& candidate : candidates) {
        std::error_code ec;
        const auto normalised = std::filesystem::weakly_canonical(candidate, ec);
        const auto key = ec ? candidate : normalised;
        if (!seen.insert(key).second) continue;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    return std::nullopt;
}

std::vector<moult::core::TextEdit> edits_from_plan_json(const std::filesystem::path& plan_path) {
    std::string text;
    if (!read_text_file(plan_path, text)) throw std::runtime_error("failed to read plan: " + plan_path.string());
    JsonValue root = JsonParser(text).parse();
    const JsonValue& edits = object_field(root, "edits");
    if (edits.type != JsonValue::Type::Array) throw std::runtime_error("plan edits field must be an array");

    std::vector<moult::core::TextEdit> out;
    for (const JsonValue& edit_obj : edits.array) {
        const JsonValue& range_obj = object_field(edit_obj, "range");
        moult::core::TextEdit edit;
        edit.id = json_string_field(edit_obj, "id");
        edit.rule_id = json_string_field(edit_obj, "rule_id");
        edit.evidence_id = json_string_field(edit_obj, "evidence_id");
        edit.confidence = moult::core::confidence_from_string(json_string_field(edit_obj, "confidence"));
        edit.range = moult::core::SourceRange{
            json_string_field(range_obj, "file"),
            json_number_field(range_obj, "begin"),
            json_number_field(range_obj, "end")};
        edit.replacement = json_string_field(edit_obj, "replacement");
        out.push_back(std::move(edit));
    }
    return out;
}

int apply_plan(const CliOptions& cli) {
    if (cli.inputs.size() != 1) {
        std::cerr << "apply requires exactly one plan.json path\n";
        return 2;
    }

    std::vector<moult::core::TextEdit> edits;
    try {
        edits = edits_from_plan_json(cli.inputs.front());
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
    if (edits.empty()) {
        std::cout << "no edits to apply\n";
        return 0;
    }

    std::set<std::string> files;
    for (const auto& edit : edits) files.insert(edit.range.file);

    moult::core::SourceStore sources;
    for (const auto& file : files) {
        if (!sources.load_file(file)) {
            std::cerr << "failed to load file for apply: " << file << "\n";
            return 1;
        }
    }

    moult::core::EditSet edit_set;
    for (auto edit : edits) {
        const auto result = edit_set.add(std::move(edit), &sources);
        if (result.outcome == moult::core::EditAddOutcome::Duplicate) continue;
        if (result.outcome != moult::core::EditAddOutcome::Accepted) {
            std::cerr << "cannot apply edit " << result.edit_id << ": invalid or conflicting range\n";
            return 1;
        }
    }

    std::map<std::string, std::string> modified;
    try {
        modified = edit_set.apply_to_memory(sources);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    for (const auto& [file, text] : modified) {
        if (cli.dry_run) {
            std::cout << "would update " << file << "\n";
            continue;
        }
        if (cli.backup) {
            std::error_code ec;
            std::filesystem::copy_file(file, file + ".moult.bak", std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "failed to write backup for " << file << ": " << ec.message() << "\n";
                return 1;
            }
        }
        if (!write_text_file(file, text)) {
            std::cerr << "failed to write " << file << "\n";
            return 1;
        }
        std::cout << "updated " << file << "\n";
    }

    return 0;
}

int report_plan(const CliOptions& cli) {
    if (cli.inputs.size() != 1) {
        std::cerr << "report requires exactly one plan.json path\n";
        return 2;
    }

    std::string text;
    if (!read_text_file(cli.inputs.front(), text)) {
        std::cerr << "failed to read plan: " << cli.inputs.front() << "\n";
        return 1;
    }

    JsonValue root;
    try {
        root = JsonParser(text).parse();
        if (root.type != JsonValue::Type::Object) throw std::runtime_error("plan.json must be an object");
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    SourceLineCache line_cache;
    try {
        const JsonValue& edits = json_array_field(root, "edits");
        const JsonValue& findings = json_array_field(root, "findings");
        const JsonValue& conflicts = json_array_field(root, "edit_conflicts");
        const JsonValue& diagnostics = json_array_field(root, "diagnostics");

        std::size_t review_count = 0;
        for (const auto& finding : findings.array) {
            if (finding.type == JsonValue::Type::Object && !finding_is_planned_edit(finding)) ++review_count;
        }

        std::cout << "Moult Report\n";
        std::cout << "Target: " << json_string_field(root, "target") << "\n";
        std::cout << "Action: " << json_string_field(root, "action") << "\n";
        std::cout << "Status: " << (json_bool_field(root, "has_errors") ? "errors" : "ok") << "\n";
        std::cout << "Accepted edits: " << json_number_field(root, "accepted_edit_count") << "\n";
        std::cout << "Manual-review findings: " << review_count << "\n";
        std::cout << "Conflicts: " << json_number_field(root, "conflict_count") << "\n";
        std::cout << "Diagnostics: " << diagnostics.array.size() << "\n";

        if (edits.array.empty()) {
            print_report_section_empty(std::cout, "Planned Edits");
        } else {
            std::cout << "\nPlanned Edits\n";
            std::size_t index = 1;
            for (const auto& edit : edits.array) {
                if (edit.type != JsonValue::Type::Object) continue;
                const JsonValue& range = object_field(edit, "range");
                const std::string replacement = json_string_field(edit, "replacement");
                std::cout << "  " << index++ << ". " << json_string_field(edit, "rule_id") << " ["
                          << json_string_field(edit, "confidence") << "]\n";
                std::cout << "     " << format_range(range, line_cache) << "\n";
                std::cout << "     replacement: "
                          << (replacement.empty() ? "<remove text>" : compact_text(replacement)) << "\n";
            }
        }

        if (review_count == 0) {
            print_report_section_empty(std::cout, "Manual Review");
        } else {
            std::cout << "\nManual Review\n";
            std::size_t index = 1;
            for (const auto& finding : findings.array) {
                if (finding.type != JsonValue::Type::Object || finding_is_planned_edit(finding)) continue;
                std::cout << "  " << index++ << ". " << json_string_field(finding, "rule_id") << " ["
                          << json_string_field(finding, "severity") << ", "
                          << json_string_field(finding, "confidence") << "]\n";
                std::cout << "     " << format_optional_range(finding, line_cache) << "\n";
                std::cout << "     " << json_string_field(finding, "title") << "\n";
                std::cout << "     " << compact_text(json_string_field(finding, "message")) << "\n";
            }
        }

        if (conflicts.array.empty()) {
            print_report_section_empty(std::cout, "Conflicts");
        } else {
            std::cout << "\nConflicts\n";
            std::size_t index = 1;
            for (const auto& conflict : conflicts.array) {
                if (conflict.type != JsonValue::Type::Object) continue;
                std::cout << "  " << index++ << ". " << optional_json_string_field(conflict, "message", "edit conflict")
                          << "\n";
                if (const JsonValue* range = optional_object_field(conflict, "proposed_range")) {
                    if (range->type == JsonValue::Type::Object) std::cout << "     " << format_range(*range, line_cache) << "\n";
                }
            }
        }

        if (diagnostics.array.empty()) {
            print_report_section_empty(std::cout, "Diagnostics");
        } else {
            std::cout << "\nDiagnostics\n";
            std::size_t index = 1;
            for (const auto& diagnostic : diagnostics.array) {
                if (diagnostic.type != JsonValue::Type::Object) continue;
                std::cout << "  " << index++ << ". " << json_string_field(diagnostic, "severity") << " "
                          << json_string_field(diagnostic, "code") << "\n";
                std::cout << "     " << format_optional_range(diagnostic, line_cache) << "\n";
                std::cout << "     " << compact_text(json_string_field(diagnostic, "message")) << "\n";
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "failed to render report: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}

int diff_plan(const CliOptions& cli) {
    if (cli.inputs.size() != 1) {
        std::cerr << "diff requires exactly one plan.json path\n";
        return 2;
    }

    std::string text;
    if (!read_text_file(cli.inputs.front(), text)) {
        std::cerr << "failed to read plan: " << cli.inputs.front() << "\n";
        return 1;
    }

    JsonValue root;
    try {
        root = JsonParser(text).parse();
        if (root.type != JsonValue::Type::Object) throw std::runtime_error("plan.json must be an object");
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    std::vector<moult::core::TextEdit> edits;
    try {
        edits = edits_from_plan_json(cli.inputs.front());
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    moult::core::SourceStore sources;
    std::set<std::string> files;
    for (const auto& edit : edits) files.insert(edit.range.file);
    for (const auto& file : files) {
        if (!sources.load_file(file)) {
            std::cerr << "failed to load file for diff: " << file << "\n";
            return 1;
        }
    }

    std::map<std::string, std::string> modified;
    if (!edits.empty()) {
        moult::core::EditSet edit_set;
        for (auto edit : edits) {
            const auto result = edit_set.add(std::move(edit), &sources);
            if (result.outcome == moult::core::EditAddOutcome::Duplicate) continue;
            if (result.outcome != moult::core::EditAddOutcome::Accepted) {
                std::cerr << "cannot diff edit " << result.edit_id << ": invalid or conflicting range\n";
                return 1;
            }
        }
        try {
            modified = edit_set.apply_to_memory(sources);
        } catch (const std::exception& ex) {
            std::cerr << ex.what() << "\n";
            return 1;
        }
    }

    if (modified.empty()) {
        std::cout << "# No accepted edits to diff.\n";
    } else {
        bool first = true;
        for (const auto& [file, new_text] : modified) {
            const auto* source = sources.get(file);
            if (!source) continue;
            if (!first) std::cout << "\n";
            first = false;
            write_unified_diff(std::cout, file, source->text(), new_text);
        }
    }

    SourceLineCache line_cache;
    const JsonValue& findings = json_array_field(root, "findings");
    std::size_t review_count = 0;
    for (const auto& finding : findings.array) {
        if (finding.type == JsonValue::Type::Object && !finding_is_planned_edit(finding)) ++review_count;
    }

    std::cout << "\n# Manual Review Suggestions\n";
    if (review_count == 0) {
        std::cout << "# none\n";
        return 0;
    }

    std::size_t index = 1;
    for (const auto& finding : findings.array) {
        if (finding.type != JsonValue::Type::Object || finding_is_planned_edit(finding)) continue;
        std::cout << "# " << index++ << ". " << json_string_field(finding, "rule_id") << " ["
                  << json_string_field(finding, "severity") << ", "
                  << json_string_field(finding, "confidence") << "]\n";
        std::cout << "#    " << format_optional_range(finding, line_cache) << "\n";
        std::cout << "#    " << compact_text(json_string_field(finding, "title")) << "\n";
        std::cout << "#    " << compact_text(json_string_field(finding, "message")) << "\n";
    }

    return 0;
}

std::optional<CliOptions> parse_args(int argc, char** argv, int& exit_code) {
    if (argc < 2) {
        print_usage(std::cerr);
        exit_code = 2;
        return std::nullopt;
    }

    const std::string command = argv[1];
    CliOptions options;
#ifdef MOULT_HAVE_CLANG_ADAPTER
    options.adapter = "clang";
#endif
    if (command == "--help" || command == "-h") {
        print_usage(std::cout);
        exit_code = 0;
        return std::nullopt;
    }
    if (command == "scan") {
        options.command = Command::Scan;
        options.action = moult::core::PlanAction::Scan;
    } else if (command == "plan") {
        options.command = Command::Plan;
        options.action = moult::core::PlanAction::Plan;
    } else if (command == "apply") {
        options.command = Command::Apply;
        options.action = moult::core::PlanAction::Apply;
    } else if (command == "report") {
        options.command = Command::Report;
    } else if (command == "diff") {
        options.command = Command::Diff;
    } else if (command == "review") {
        options.command = Command::Review;
    } else {
        std::cerr << "unknown command: " << command << "\n";
        print_usage(std::cerr);
        exit_code = 2;
        return std::nullopt;
    }

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](std::string_view option) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << option << "\n";
                exit_code = 2;
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            exit_code = 0;
            return std::nullopt;
        }
        if (arg == "--target") {
            auto value = require_value("--target");
            if (!value) return std::nullopt;
            options.target = *value;
            continue;
        }
        if (arg == "--adapter") {
            auto value = require_value("--adapter");
            if (!value) return std::nullopt;
            options.adapter = *value;
            continue;
        }
        if (arg == "--clang-arg") {
            auto value = require_value("--clang-arg");
            if (!value) return std::nullopt;
            options.clang_args.push_back(*value);
            continue;
        }
        if (arg == "--compile-commands") {
            auto value = require_value("--compile-commands");
            if (!value) return std::nullopt;
            std::filesystem::path path(*value);
            std::error_code ec;
            if (std::filesystem::is_directory(path, ec)) path /= "compile_commands.json";
            options.compile_commands_path = path;
            continue;
        }
        if (arg == "--out") {
            auto value = require_value("--out");
            if (!value) return std::nullopt;
            options.output_dir = std::filesystem::path(*value);
            continue;
        }
        if (arg == "--min-confidence") {
            auto value = require_value("--min-confidence");
            if (!value) return std::nullopt;
            try {
                options.minimum_confidence = moult::core::confidence_from_string(*value);
            } catch (const std::exception& ex) {
                std::cerr << ex.what() << "\n";
                exit_code = 2;
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--include-facts") {
            options.include_facts = true;
            continue;
        }
        if (arg == "--dry-run") {
            options.dry_run = true;
            continue;
        }
        if (arg == "--backup") {
            options.backup = true;
            continue;
        }
        if (arg == "--format") {
            auto value = require_value("--format");
            if (!value) return std::nullopt;
            if (*value != "json") {
                std::cerr << "unsupported format: " << *value << "\n";
                exit_code = 2;
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--") {
            while (++i < argc) options.inputs.emplace_back(argv[i]);
            break;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option: " << arg << "\n";
            exit_code = 2;
            return std::nullopt;
        }
        options.inputs.emplace_back(arg);
    }

    const bool artefact_command = options.command == Command::Apply || options.command == Command::Report ||
                                  options.command == Command::Diff || options.command == Command::Review;
    if (options.inputs.empty() && (artefact_command || !options.compile_commands_path)) {
        std::cerr << (artefact_command ? "plan.json path or output directory is required\n"
                                      : "at least one input file, directory, or compile database is required\n");
        print_usage(std::cerr);
        exit_code = 2;
        return std::nullopt;
    }
    if (artefact_command) return options;

    if (options.target != moult::cpp_modernization::target_name &&
        options.target != moult::cpp_modernization::legacy_target_name) {
        std::cerr << "unsupported target: " << options.target << "\n";
        exit_code = 2;
        return std::nullopt;
    }
    if (options.target == moult::cpp_modernization::legacy_target_name) {
        options.target = std::string(moult::cpp_modernization::target_name);
    }
    if (options.adapter != "textual" && options.adapter != "clang") {
        std::cerr << "unsupported adapter: " << options.adapter << "\n";
        exit_code = 2;
        return std::nullopt;
    }
#ifndef MOULT_HAVE_CLANG_ADAPTER
    if (options.adapter == "clang") {
        std::cerr << "clang adapter was not built\n";
        exit_code = 2;
        return std::nullopt;
    }
#endif

    return options;
}

} // namespace

int main(int argc, char** argv) {
    int exit_code = 0;
    auto parsed = parse_args(argc, argv, exit_code);
    if (!parsed) return exit_code;
    const CliOptions& cli = *parsed;

    if (cli.command == Command::Apply) return apply_plan(cli);
    if (cli.command == Command::Report) return report_plan(cli);
    if (cli.command == Command::Diff) return diff_plan(cli);
    if (cli.command == Command::Review) return moult::cli::run_review_tui(cli.inputs.front());

    std::vector<CompileCommandEntry> compile_command_entries;
    if (cli.compile_commands_path) {
        try {
            compile_command_entries = parse_compile_commands_file(*cli.compile_commands_path);
        } catch (const std::exception& ex) {
            std::cerr << ex.what() << "\n";
            return 1;
        }
    }

    std::vector<std::filesystem::path> files;
    if (cli.inputs.empty()) {
        files = source_files_from_compile_commands(compile_command_entries);
    } else {
        for (const auto& input : cli.inputs) {
            if (!collect_input_files(input, files)) {
                std::cerr << "failed to read input path: " << input << "\n";
                return 1;
            }
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    if (files.empty()) {
        std::cerr << "no C++ source-like files found\n";
        return 1;
    }

    moult::core::SourceStore sources;
    for (const auto& file : files) {
        if (!sources.load_file(file)) {
            std::cerr << "failed to load file: " << file << "\n";
            return 1;
        }
    }

    moult::core::Engine engine;
    if (cli.adapter == "textual") {
        engine.set_adapter(std::make_shared<moult::cpp_modernization::TextualCppModernizationAdapter>());
    }
#ifdef MOULT_HAVE_CLANG_ADAPTER
    else {
        std::vector<std::string> clang_args{"-x", "c++", "-std=c++14"};
        clang_args.insert(clang_args.end(), cli.clang_args.begin(), cli.clang_args.end());
        if (!cli.compile_commands_path) {
            const auto compile_commands_path = find_compile_commands(cli.inputs);
            if (compile_commands_path) {
                try {
                    compile_command_entries = parse_compile_commands_file(*compile_commands_path);
                } catch (const std::exception& ex) {
                    std::cerr << ex.what() << "\n";
                    return 1;
                }
            }
        }
        moult::clang_adapter::ClangCppModernizationAdapter::CompileCommandMap compile_commands;
        try {
            compile_commands = compile_command_map_from_entries(compile_command_entries);
        } catch (const std::exception& ex) {
            std::cerr << ex.what() << "\n";
            return 1;
        }
        engine.set_adapter(std::make_shared<moult::clang_adapter::ClangCppModernizationAdapter>(
            std::move(clang_args), std::move(compile_commands)));
    }
#endif
    engine.add_capsule(std::make_shared<moult::cpp_modernization::CppModernizationCapsule>());

    moult::core::RunOptions options;
    options.action = cli.action;
    options.target = cli.target;
    options.minimum_confidence = cli.minimum_confidence;

    auto result = engine.run(sources, options);
    if (cli.output_dir) {
        auto written = moult::core::write_outputs(result, *cli.output_dir, cli.include_facts);
        for (const auto& path : written.written_files) std::cout << "wrote " << path << "\n";
        for (const auto& error : written.errors) std::cerr << error << "\n";
        return written.ok && !result.plan.has_errors() ? 0 : 1;
    }

    std::cout << moult::core::plan_to_json(result.plan, cli.include_facts ? &result.facts : nullptr) << "\n";
    return result.plan.has_errors() ? 1 : 0;
}

#include "moult/core/api.hpp"
#include "moult/cpp/modernization.hpp"
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
    Apply
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

void print_usage(std::ostream& out) {
    out << "usage: moult <scan|plan> [options] <file-or-directory>...\n"
        << "       moult apply [options] <plan.json>\n"
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
        << "  --compile-commands <path>    compile_commands.json path or build directory\n"
        << "  --out <directory>            write plan.json, facts.json, evidence.jsonl, findings.sarif\n"
        << "  --min-confidence <level>     low, medium, high, or proven (default: high)\n"
        << "  --include-facts              include facts inside plan.json/stdout JSON\n"
        << "  --dry-run                    for apply, report files that would change\n"
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

std::size_t json_number_field(const JsonValue& object, std::string_view name) {
    const JsonValue& value = object_field(object, name);
    if (value.type != JsonValue::Type::Number) throw std::runtime_error("expected number field: " + std::string(name));
    return value.number;
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

std::vector<std::string> sanitize_compile_command(std::vector<std::string> args, const std::filesystem::path& source_file) {
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
        if (same_path_loose(std::filesystem::path(arg), source_file)) continue;
        out.push_back(arg);
    }
    return out;
}

#ifdef MOULT_HAVE_CLANG_ADAPTER
moult::clang_adapter::ClangCppModernizationAdapter::CompileCommandMap parse_compile_commands_file(
    const std::filesystem::path& path) {
    std::string text;
    if (!read_text_file(path, text)) throw std::runtime_error("failed to read compile commands: " + path.string());
    JsonValue root = JsonParser(text).parse();
    if (root.type != JsonValue::Type::Array) throw std::runtime_error("compile_commands.json must be an array");

    moult::clang_adapter::ClangCppModernizationAdapter::CompileCommandMap out;
    for (const JsonValue& entry : root.array) {
        if (entry.type != JsonValue::Type::Object) continue;
        const std::string directory = optional_object_field(entry, "directory") &&
                                              optional_object_field(entry, "directory")->type == JsonValue::Type::String
                                          ? optional_object_field(entry, "directory")->string
                                          : std::string();
        const std::filesystem::path file_field = json_string_field(entry, "file");
        const std::filesystem::path source_file =
            file_field.is_absolute() ? file_field : std::filesystem::path(directory) / file_field;

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

        out[source_file.string()] = sanitize_compile_command(std::move(args), source_file);
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
        const auto normalized = std::filesystem::weakly_canonical(candidate, ec);
        const auto key = ec ? candidate : normalized;
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

    if (options.inputs.empty()) {
        std::cerr << (options.command == Command::Apply ? "plan.json path is required\n"
                                                        : "at least one input file or directory is required\n");
        print_usage(std::cerr);
        exit_code = 2;
        return std::nullopt;
    }
    if (options.command == Command::Apply) return options;

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

    std::vector<std::filesystem::path> files;
    for (const auto& input : cli.inputs) {
        if (!collect_input_files(input, files)) {
            std::cerr << "failed to read input path: " << input << "\n";
            return 1;
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
        moult::clang_adapter::ClangCppModernizationAdapter::CompileCommandMap compile_commands;
        const auto compile_commands_path = cli.compile_commands_path ? cli.compile_commands_path : find_compile_commands(cli.inputs);
        if (compile_commands_path) {
            try {
                compile_commands = parse_compile_commands_file(*compile_commands_path);
            } catch (const std::exception& ex) {
                std::cerr << ex.what() << "\n";
                return 1;
            }
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

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
#include <utility>
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
    bool exclude_vendored = false;
    bool report_summary_only = false;
    std::size_t report_limit = 12;
    std::string adapter = "textual";
    std::vector<std::string> clang_args;
    std::vector<std::string> include_patterns;
    std::vector<std::string> exclude_patterns;
    std::vector<std::string> report_rule_patterns;
    std::vector<std::string> report_file_patterns;
    std::vector<std::filesystem::path> inputs;
};

struct CompileCommandEntry {
    std::filesystem::path source_file;
    std::filesystem::path directory;
    std::vector<std::string> args;
    moult::core::SourceLanguage language = moult::core::SourceLanguage::Unknown;
};

struct SourceInput {
    std::filesystem::path path;
    moult::core::SourceLanguage language = moult::core::SourceLanguage::Unknown;
};

struct DirectoryLanguageProfile {
    bool has_c_source = false;
    bool has_cxx_source = false;
};

void print_usage(std::ostream& out) {
    out << "usage: moult <scan|plan> [options] [file-or-directory]...\n"
        << "       moult apply [options] <plan.json>\n"
        << "       moult report [options] <plan.json>\n"
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
        << "  --include <glob>             keep matching source paths; repeat as needed\n"
        << "  --exclude <glob>             skip matching source paths; repeat as needed\n"
        << "  --exclude-vendored           skip common vendored dependency directories during scans\n"
        << "  --summary-only               for report, omit item-by-item sections\n"
        << "  --limit <count>              for report, grouped summary rows to show (default: 12)\n"
        << "  --rule <glob>                for report, show matching rule IDs only; repeat as needed\n"
        << "  --file <glob>                for report, show matching source paths only; repeat as needed\n"
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

bool is_cxx_input_language(moult::core::SourceLanguage language) {
    return moult::core::is_cxx_language(language);
}

bool needs_content_language_inference(const std::filesystem::path& path) {
    return lowercase(path.extension().string()) == ".h";
}

bool is_c_source_extension(const std::filesystem::path& path) {
    return lowercase(path.extension().string()) == ".c";
}

bool is_unambiguous_cxx_source_extension(const std::filesystem::path& path) {
    const std::string ext = lowercase(path.extension().string());
    return ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".c++" || ext == ".ixx";
}

std::optional<SourceInput> source_input_from_path(const std::filesystem::path& path) {
    const auto language = moult::core::infer_source_language(path);
    if (!is_cxx_input_language(language)) return std::nullopt;
    return SourceInput{path, needs_content_language_inference(path) ? moult::core::SourceLanguage::Unknown : language};
}

bool collect_input_files(const std::filesystem::path& input, std::vector<SourceInput>& files) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(input, ec)) {
        if (auto source_input = source_input_from_path(input)) files.push_back(*source_input);
        return true;
    }
    if (!std::filesystem::is_directory(input, ec)) return false;

    std::vector<std::filesystem::path> candidates;
    std::map<std::filesystem::path, DirectoryLanguageProfile> directory_profiles;
    for (std::filesystem::recursive_directory_iterator it(input, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto path = it->path();
        candidates.push_back(path);
        auto& profile = directory_profiles[path.parent_path()];
        profile.has_c_source = profile.has_c_source || is_c_source_extension(path);
        profile.has_cxx_source = profile.has_cxx_source || is_unambiguous_cxx_source_extension(path);
    }
    for (const auto& path : candidates) {
        if (needs_content_language_inference(path)) {
            const auto profile = directory_profiles.find(path.parent_path());
            if (profile != directory_profiles.end() && profile->second.has_c_source && !profile->second.has_cxx_source) {
                continue;
            }
        }
        if (auto source_input = source_input_from_path(path)) files.push_back(*source_input);
    }
    return !ec;
}

std::string normalise_glob_pattern(std::string pattern) {
    std::replace(pattern.begin(), pattern.end(), '\\', '/');
    while (pattern.find("//") != std::string::npos) pattern.erase(pattern.find("//"), 1);
    return pattern;
}

bool glob_match(std::string_view pattern, std::string_view text) {
    std::map<std::pair<std::size_t, std::size_t>, bool> memo;
    auto match = [&](auto&& self, std::size_t pi, std::size_t ti) -> bool {
        const auto key = std::make_pair(pi, ti);
        if (const auto it = memo.find(key); it != memo.end()) return it->second;

        bool result = false;
        if (pi == pattern.size()) {
            result = ti == text.size();
        } else if (pattern[pi] == '*') {
            const bool deep = pi + 1 < pattern.size() && pattern[pi + 1] == '*';
            std::size_t next_pi = pi + (deep ? 2 : 1);
            while (next_pi < pattern.size() && pattern[next_pi] == '*') ++next_pi;

            if (deep && next_pi < pattern.size() && pattern[next_pi] == '/') {
                result = self(self, next_pi + 1, ti);
            }
            if (!result) result = self(self, next_pi, ti);

            for (std::size_t next_ti = ti; !result && next_ti < text.size(); ++next_ti) {
                if (!deep && text[next_ti] == '/') break;
                result = self(self, next_pi, next_ti + 1);
            }
        } else if (pattern[pi] == '?') {
            result = ti < text.size() && text[ti] != '/' && self(self, pi + 1, ti + 1);
        } else {
            result = ti < text.size() && pattern[pi] == text[ti] && self(self, pi + 1, ti + 1);
        }

        memo.emplace(key, result);
        return result;
    };
    return match(match, 0, 0);
}

bool glob_matches_path(std::string pattern, const std::filesystem::path& path) {
    pattern = normalise_glob_pattern(std::move(pattern));
    const std::string full = path.generic_string();
    const std::string filename = path.filename().generic_string();
    const bool has_slash = pattern.find('/') != std::string::npos;
    if (pattern.empty()) return false;
    if (!has_slash) return glob_match(pattern, filename) || glob_match("**/" + pattern, full);
    if (!pattern.empty() && pattern.front() == '/') return glob_match(pattern, full);
    return glob_match(pattern, full) || glob_match("**/" + pattern, full);
}

bool glob_matches_text(std::string pattern, std::string_view text) {
    pattern = normalise_glob_pattern(std::move(pattern));
    return !pattern.empty() && glob_match(pattern, text);
}

bool matches_any_pattern(const std::vector<std::string>& patterns, const std::filesystem::path& path) {
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& pattern) {
        return glob_matches_path(pattern, path);
    });
}

bool matches_any_text_pattern(const std::vector<std::string>& patterns, std::string_view text) {
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& pattern) {
        return glob_matches_text(pattern, text);
    });
}

const std::vector<std::string>& vendored_exclude_patterns() {
    static const std::vector<std::string> patterns{
        ".git/**",
        "3rdparty/**",
        "deps/**",
        "dependencies/**",
        "external/**",
        "extern/**",
        "node_modules/**",
        "submodules/**",
        "third_party/**",
        "thirdparty/**",
        "vendor/**",
        "vendors/**",
        "crc32c/**",
        "leveldb/**",
        "minisketch/**",
        "secp256k1/**",
        "univalue/**",
    };
    return patterns;
}

void apply_path_filters(const CliOptions& cli, std::vector<SourceInput>& files) {
    std::vector<std::string> exclude_patterns = cli.exclude_patterns;
    if (cli.exclude_vendored) {
        const auto& vendored = vendored_exclude_patterns();
        exclude_patterns.insert(exclude_patterns.end(), vendored.begin(), vendored.end());
    }
    files.erase(std::remove_if(files.begin(), files.end(), [&](const SourceInput& file) {
                    if (!cli.include_patterns.empty() && !matches_any_pattern(cli.include_patterns, file.path)) return true;
                    if (matches_any_pattern(exclude_patterns, file.path)) return true;
                    return false;
                }),
                files.end());
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

moult::core::SourceLanguage language_from_x_value(std::string_view value) {
    if (value == "c" || value == "c-header" || value == "cpp-output" || value == "objective-c" ||
        value == "assembler-with-cpp") {
        return moult::core::SourceLanguage::C;
    }
    if (value == "c++" || value == "c++-header" || value == "c++-cpp-output" || value == "objective-c++") {
        return moult::core::SourceLanguage::Cxx;
    }
    return moult::core::SourceLanguage::Unknown;
}

std::optional<moult::core::SourceLanguage> explicit_language_from_args(const std::vector<std::string>& args) {
    std::optional<moult::core::SourceLanguage> out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-x" && i + 1 < args.size()) {
            const auto language = language_from_x_value(args[i + 1]);
            if (language != moult::core::SourceLanguage::Unknown) out = language;
            continue;
        }
        if (arg.rfind("-x", 0) == 0 && arg.size() > 2) {
            const auto language = language_from_x_value(std::string_view(arg).substr(2));
            if (language != moult::core::SourceLanguage::Unknown) out = language;
        }
    }
    return out;
}

bool has_cxx_standard_arg(const std::vector<std::string>& args) {
    return std::any_of(args.begin(), args.end(), [](const std::string& arg) {
        return arg.rfind("-std=c++", 0) == 0 || arg.rfind("-std=gnu++", 0) == 0 || arg.rfind("/std:c++", 0) == 0;
    });
}

bool compiler_name_is_cxx(std::string compiler) {
    compiler = lowercase(std::filesystem::path(std::move(compiler)).filename().string());
    if (compiler.size() > 4 && compiler.substr(compiler.size() - 4) == ".exe") compiler.resize(compiler.size() - 4);
    return compiler == "c++" || compiler == "g++" || compiler == "clang++" || compiler.find("g++-") == 0 ||
           compiler.find("clang++-") == 0 || compiler.find("++") != std::string::npos;
}

moult::core::SourceLanguage infer_source_language_from_compile_command(const std::vector<std::string>& args,
                                                                       const std::filesystem::path& source_file) {
    if (auto explicit_language = explicit_language_from_args(args)) return *explicit_language;

    const auto path_language = moult::core::infer_source_language(source_file);
    if (path_language == moult::core::SourceLanguage::C && !args.empty()) {
        std::size_t compiler_index = 0;
        const std::string first = lowercase(std::filesystem::path(args.front()).filename().string());
        if ((first == "ccache" || first == "sccache" || first == "distcc") && args.size() > 1) compiler_index = 1;
        if (compiler_name_is_cxx(args[compiler_index]) || has_cxx_standard_arg(args)) return moult::core::SourceLanguage::Cxx;
    }
    return path_language;
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
            sanitize_compile_command(args, source_file, command_directory),
            infer_source_language_from_compile_command(args, source_file)});
    }
    return out;
}

std::vector<SourceInput> source_files_from_compile_commands(const std::vector<CompileCommandEntry>& entries) {
    std::vector<SourceInput> out;
    out.reserve(entries.size());
    for (const auto& entry : entries) {
        if (is_cxx_input_language(entry.language)) out.push_back(SourceInput{entry.source_file, entry.language});
    }
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

struct ReportBucket {
    std::size_t edits = 0;
    std::size_t manual = 0;
    std::size_t conflicts = 0;
    std::size_t diagnostics = 0;

    [[nodiscard]] std::size_t total() const noexcept {
        return edits + manual + conflicts + diagnostics;
    }
};

enum class ReportBucketKind {
    Edit,
    Manual,
    Conflict,
    Diagnostic
};

void add_report_bucket(std::map<std::string, ReportBucket>& buckets, std::string key, ReportBucketKind kind) {
    if (key.empty()) key = "no source range";
    ReportBucket& bucket = buckets[std::move(key)];
    switch (kind) {
        case ReportBucketKind::Edit: ++bucket.edits; break;
        case ReportBucketKind::Manual: ++bucket.manual; break;
        case ReportBucketKind::Conflict: ++bucket.conflicts; break;
        case ReportBucketKind::Diagnostic: ++bucket.diagnostics; break;
    }
}

std::optional<std::string> optional_range_file(const JsonValue& object, std::string_view name = "range") {
    const JsonValue* range = optional_object_field(object, name);
    if (!range || range->type != JsonValue::Type::Object) return std::nullopt;
    const JsonValue* file = optional_object_field(*range, "file");
    if (!file || file->type != JsonValue::Type::String) return std::nullopt;
    return file->string;
}

std::string directory_key_for_file(const std::string& file) {
    if (file.empty()) return "no source range";
    const auto parent = std::filesystem::path(file).parent_path();
    return parent.empty() ? "." : parent.generic_string();
}

std::vector<std::pair<std::string, ReportBucket>> sorted_report_buckets(const std::map<std::string, ReportBucket>& buckets) {
    std::vector<std::pair<std::string, ReportBucket>> out(buckets.begin(), buckets.end());
    std::sort(out.begin(), out.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second.total() != rhs.second.total()) return lhs.second.total() > rhs.second.total();
        return lhs.first < rhs.first;
    });
    return out;
}

void print_report_buckets(std::ostream& out,
                          std::string_view title,
                          const std::map<std::string, ReportBucket>& buckets,
                          std::size_t limit = 12) {
    out << "\n" << title << "\n";
    if (buckets.empty()) {
        out << "  none\n";
        return;
    }
    const auto sorted = sorted_report_buckets(buckets);
    const std::size_t shown = std::min(limit, sorted.size());
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& [key, bucket] = sorted[i];
        out << "  " << compact_text(key)
            << "  total=" << bucket.total()
            << " edits=" << bucket.edits
            << " manual=" << bucket.manual
            << " conflicts=" << bucket.conflicts
            << " diagnostics=" << bucket.diagnostics << "\n";
    }
    if (shown < sorted.size()) out << "  ... " << (sorted.size() - shown) << " more\n";
}

std::optional<std::string> top_report_bucket_key(const std::map<std::string, ReportBucket>& buckets) {
    const auto sorted = sorted_report_buckets(buckets);
    for (const auto& [key, bucket] : sorted) {
        if (bucket.total() > 0 && key != "no source range") return key;
    }
    return std::nullopt;
}

bool shell_safe(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '.' || c == '_' || c == '-' || c == ':' ||
           c == '=' || c == '+';
}

std::string shell_quote(std::string value) {
    if (value.empty()) return "''";
    if (std::all_of(value.begin(), value.end(), shell_safe)) return value;
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string directory_glob_for_report(std::string directory) {
    if (directory.empty() || directory == ".") return "*";
    if (!directory.empty() && directory.back() == '/') directory.pop_back();
    return directory + "/**";
}

void print_recommended_next_action(std::ostream& out,
                                   const std::filesystem::path& plan_path,
                                   bool has_errors,
                                   std::size_t accepted_edits,
                                   std::size_t manual_review,
                                   std::size_t conflicts,
                                   std::size_t diagnostics,
                                   const std::map<std::string, ReportBucket>& by_directory,
                                   const std::map<std::string, ReportBucket>& by_rule,
                                   bool report_filtered) {
    const std::size_t total = accepted_edits + manual_review + conflicts + diagnostics;
    const std::string plan_arg = shell_quote(plan_path.string());
    out << "\nRecommended Next Action\n";
    if (has_errors || conflicts > 0) {
        out << "  Resolve conflicts and diagnostics before applying edits.\n";
        out << "  Start with: moult review " << plan_arg << "\n";
        return;
    }
    if (total > 500) {
        if (const auto top_directory = top_report_bucket_key(by_directory)) {
            out << "  Scope is large. Start with the busiest directory:\n";
            out << "  moult report --summary-only --file " << shell_quote(directory_glob_for_report(*top_directory)) << " "
                << plan_arg << "\n";
        } else {
            out << "  Scope is large. Re-run plan with --include/--exclude to isolate one subsystem before applying edits.\n";
        }
        if (const auto top_rule = top_report_bucket_key(by_rule)) {
            out << "  Then isolate the busiest rule:\n";
            out << "  moult report --summary-only --rule " << shell_quote(*top_rule) << " " << plan_arg << "\n";
        }
        return;
    }
    if (report_filtered && total > 0) {
        out << "  This is a filtered view. Use review decisions to change what apply will touch.\n";
        out << "  Start with: moult review " << plan_arg << "\n";
        return;
    }
    if (accepted_edits > 0) {
        out << "  Review the patch with: moult diff " << plan_arg << "\n";
        out << "  Then dry-run application with: moult apply --dry-run " << plan_arg << "\n";
        return;
    }
    if (manual_review > 0) {
        out << "  Triage manual-review findings with: moult review " << plan_arg << "\n";
        return;
    }
    if (diagnostics > 0) {
        out << "  Inspect diagnostics, then rerun the plan after parser/input issues are fixed.\n";
        return;
    }
    out << "  No migration work was found. Broaden the input paths or enable another target.\n";
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

        auto report_item_matches = [&](std::string_view rule_id, const std::optional<std::string>& file) {
            if (!cli.report_rule_patterns.empty() && !matches_any_text_pattern(cli.report_rule_patterns, rule_id)) {
                return false;
            }
            if (!cli.report_file_patterns.empty()) {
                if (!file) return false;
                if (!matches_any_pattern(cli.report_file_patterns, std::filesystem::path(*file))) return false;
            }
            return true;
        };

        std::vector<const JsonValue*> edits_to_report;
        std::vector<const JsonValue*> findings_to_report;
        std::vector<const JsonValue*> conflicts_to_report;
        std::vector<const JsonValue*> diagnostics_to_report;
        std::map<std::string, ReportBucket> by_rule;
        std::map<std::string, ReportBucket> by_directory;
        std::map<std::string, ReportBucket> by_file;

        auto add_source_summary = [&](const JsonValue& object, ReportBucketKind kind, std::string_view range_name = "range") {
            const std::string file = optional_range_file(object, range_name).value_or("no source range");
            add_report_bucket(by_file, file, kind);
            add_report_bucket(by_directory, file == "no source range" ? file : directory_key_for_file(file), kind);
        };

        for (const auto& edit : edits.array) {
            if (edit.type != JsonValue::Type::Object) continue;
            const std::string rule_id = json_string_field(edit, "rule_id");
            if (!report_item_matches(rule_id, optional_range_file(edit))) continue;
            edits_to_report.push_back(&edit);
            add_report_bucket(by_rule, rule_id, ReportBucketKind::Edit);
            add_source_summary(edit, ReportBucketKind::Edit);
        }
        for (const auto& finding : findings.array) {
            if (finding.type != JsonValue::Type::Object || finding_is_planned_edit(finding)) continue;
            const std::string rule_id = json_string_field(finding, "rule_id");
            if (!report_item_matches(rule_id, optional_range_file(finding))) continue;
            findings_to_report.push_back(&finding);
            add_report_bucket(by_rule, rule_id, ReportBucketKind::Manual);
            add_source_summary(finding, ReportBucketKind::Manual);
        }
        for (const auto& conflict : conflicts.array) {
            if (conflict.type != JsonValue::Type::Object) continue;
            if (!report_item_matches("edit conflict", optional_range_file(conflict, "proposed_range"))) continue;
            conflicts_to_report.push_back(&conflict);
            add_report_bucket(by_rule, "edit conflict", ReportBucketKind::Conflict);
            add_source_summary(conflict, ReportBucketKind::Conflict, "proposed_range");
        }
        for (const auto& diagnostic : diagnostics.array) {
            if (diagnostic.type != JsonValue::Type::Object) continue;
            const std::string rule_id = "diagnostic:" + json_string_field(diagnostic, "code");
            if (!report_item_matches(rule_id, optional_range_file(diagnostic))) continue;
            diagnostics_to_report.push_back(&diagnostic);
            add_report_bucket(by_rule, rule_id, ReportBucketKind::Diagnostic);
            add_source_summary(diagnostic, ReportBucketKind::Diagnostic);
        }

        const bool has_errors = json_bool_field(root, "has_errors");
        const std::size_t accepted_edit_count = edits_to_report.size();
        const std::size_t review_count = findings_to_report.size();
        const std::size_t conflict_count = conflicts_to_report.size();
        const std::size_t diagnostic_count = diagnostics_to_report.size();
        const bool filtered = !cli.report_rule_patterns.empty() || !cli.report_file_patterns.empty();
        std::cout << "Moult Report\n";
        std::cout << "Target: " << json_string_field(root, "target") << "\n";
        std::cout << "Action: " << json_string_field(root, "action") << "\n";
        std::cout << "Status: " << (has_errors ? "errors" : "ok") << "\n";
        std::cout << "Accepted edits: " << accepted_edit_count << "\n";
        std::cout << "Manual-review findings: " << review_count << "\n";
        std::cout << "Conflicts: " << conflict_count << "\n";
        std::cout << "Diagnostics: " << diagnostic_count << "\n";
        if (filtered) {
            std::cout << "Filters:";
            for (const auto& pattern : cli.report_rule_patterns) std::cout << " rule=" << shell_quote(pattern);
            for (const auto& pattern : cli.report_file_patterns) std::cout << " file=" << shell_quote(pattern);
            std::cout << "\n";
        }
        print_report_buckets(std::cout, "Summary by Rule", by_rule, cli.report_limit);
        print_report_buckets(std::cout, "Summary by Directory", by_directory, cli.report_limit);
        print_report_buckets(std::cout, "Summary by File", by_file, cli.report_limit);
        print_recommended_next_action(std::cout,
                                      cli.inputs.front(),
                                      has_errors,
                                      accepted_edit_count,
                                      review_count,
                                      conflict_count,
                                      diagnostic_count,
                                      by_directory,
                                      by_rule,
                                      filtered);

        if (cli.report_summary_only) return 0;

        if (edits_to_report.empty()) {
            print_report_section_empty(std::cout, "Planned Edits");
        } else {
            std::cout << "\nPlanned Edits\n";
            std::size_t index = 1;
            for (const JsonValue* edit_ptr : edits_to_report) {
                const JsonValue& edit = *edit_ptr;
                const JsonValue& range = object_field(edit, "range");
                const std::string replacement = json_string_field(edit, "replacement");
                std::cout << "  " << index++ << ". " << json_string_field(edit, "rule_id") << " ["
                          << json_string_field(edit, "confidence") << "]\n";
                std::cout << "     " << format_range(range, line_cache) << "\n";
                std::cout << "     replacement: "
                          << (replacement.empty() ? "<remove text>" : compact_text(replacement)) << "\n";
            }
        }

        if (findings_to_report.empty()) {
            print_report_section_empty(std::cout, "Manual Review");
        } else {
            std::cout << "\nManual Review\n";
            std::size_t index = 1;
            for (const JsonValue* finding_ptr : findings_to_report) {
                const JsonValue& finding = *finding_ptr;
                std::cout << "  " << index++ << ". " << json_string_field(finding, "rule_id") << " ["
                          << json_string_field(finding, "severity") << ", "
                          << json_string_field(finding, "confidence") << "]\n";
                std::cout << "     " << format_optional_range(finding, line_cache) << "\n";
                std::cout << "     " << json_string_field(finding, "title") << "\n";
                std::cout << "     " << compact_text(json_string_field(finding, "message")) << "\n";
            }
        }

        if (conflicts_to_report.empty()) {
            print_report_section_empty(std::cout, "Conflicts");
        } else {
            std::cout << "\nConflicts\n";
            std::size_t index = 1;
            for (const JsonValue* conflict_ptr : conflicts_to_report) {
                const JsonValue& conflict = *conflict_ptr;
                std::cout << "  " << index++ << ". " << optional_json_string_field(conflict, "message", "edit conflict")
                          << "\n";
                if (const JsonValue* range = optional_object_field(conflict, "proposed_range")) {
                    if (range->type == JsonValue::Type::Object) std::cout << "     " << format_range(*range, line_cache) << "\n";
                }
            }
        }

        if (diagnostics_to_report.empty()) {
            print_report_section_empty(std::cout, "Diagnostics");
        } else {
            std::cout << "\nDiagnostics\n";
            std::size_t index = 1;
            for (const JsonValue* diagnostic_ptr : diagnostics_to_report) {
                const JsonValue& diagnostic = *diagnostic_ptr;
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
        if (arg == "--include") {
            auto value = require_value("--include");
            if (!value) return std::nullopt;
            options.include_patterns.push_back(*value);
            continue;
        }
        if (arg == "--exclude") {
            auto value = require_value("--exclude");
            if (!value) return std::nullopt;
            options.exclude_patterns.push_back(*value);
            continue;
        }
        if (arg == "--exclude-vendored") {
            options.exclude_vendored = true;
            continue;
        }
        if (arg == "--summary-only") {
            options.report_summary_only = true;
            continue;
        }
        if (arg == "--limit") {
            auto value = require_value("--limit");
            if (!value) return std::nullopt;
            try {
                const auto parsed_limit = std::stoul(*value);
                if (parsed_limit == 0) throw std::invalid_argument("zero limit");
                options.report_limit = parsed_limit;
            } catch (const std::exception&) {
                std::cerr << "invalid --limit value: " << *value << "\n";
                exit_code = 2;
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--rule") {
            auto value = require_value("--rule");
            if (!value) return std::nullopt;
            options.report_rule_patterns.push_back(*value);
            continue;
        }
        if (arg == "--file") {
            auto value = require_value("--file");
            if (!value) return std::nullopt;
            options.report_file_patterns.push_back(*value);
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
    const bool has_report_options = options.report_summary_only || options.report_limit != 12 ||
                                    !options.report_rule_patterns.empty() || !options.report_file_patterns.empty();
    if (options.command != Command::Report && has_report_options) {
        std::cerr << "report options require the report command\n";
        exit_code = 2;
        return std::nullopt;
    }
    if (artefact_command && (!options.include_patterns.empty() || !options.exclude_patterns.empty() || options.exclude_vendored)) {
        std::cerr << "scan path filters require scan or plan\n";
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

    std::vector<SourceInput> files;
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
    std::sort(files.begin(), files.end(), [](const SourceInput& lhs, const SourceInput& rhs) {
        return lhs.path < rhs.path;
    });
    files.erase(std::unique(files.begin(), files.end(), [](const SourceInput& lhs, const SourceInput& rhs) {
                    return lhs.path == rhs.path;
                }),
                files.end());
    apply_path_filters(cli, files);
    if (files.empty()) {
        std::cerr << "no C++ source-like files found after language and path filters\n";
        return 1;
    }

    moult::core::SourceStore sources;
    for (const auto& file : files) {
        if (!sources.load_file(file.path, file.language)) {
            std::cerr << "failed to load file: " << file.path << "\n";
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

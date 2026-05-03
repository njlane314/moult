#include "moult/core/api.hpp"
#include "moult/cpp/modernization.hpp"
#ifdef MOULT_HAVE_CLANG_ADAPTER
#include "moult/clang/adapter.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

struct CliOptions {
    moult::core::PlanAction action = moult::core::PlanAction::Plan;
    std::string target = std::string(moult::cpp_modernization::target_name);
    std::optional<std::filesystem::path> output_dir;
    moult::core::Confidence minimum_confidence = moult::core::Confidence::High;
    bool include_facts = false;
    std::string adapter = "textual";
    std::vector<std::string> clang_args;
    std::vector<std::filesystem::path> inputs;
};

void print_usage(std::ostream& out) {
    out << "usage: moult <scan|plan> [options] <file-or-directory>...\n"
        << "\n"
        << "options:\n"
        << "  --target <name>              target to run (default: cpp-modernization)\n"
        << "  --adapter <name>             textual or clang"
#ifdef MOULT_HAVE_CLANG_ADAPTER
        << " (default: clang)\n"
#else
        << " (default: textual; clang not built)\n"
#endif
        << "  --clang-arg <arg>            extra argument for libclang; repeat as needed\n"
        << "  --out <directory>            write plan.json, facts.json, evidence.jsonl, findings.sarif\n"
        << "  --min-confidence <level>     low, medium, high, or proven (default: high)\n"
        << "  --include-facts              include facts inside plan.json/stdout JSON\n"
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
        options.action = moult::core::PlanAction::Scan;
    } else if (command == "plan") {
        options.action = moult::core::PlanAction::Plan;
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
        std::cerr << "at least one input file or directory is required\n";
        print_usage(std::cerr);
        exit_code = 2;
        return std::nullopt;
    }
    if (options.target != moult::cpp_modernization::target_name) {
        std::cerr << "unsupported target: " << options.target << "\n";
        exit_code = 2;
        return std::nullopt;
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
        engine.set_adapter(std::make_shared<moult::clang_adapter::ClangCppModernizationAdapter>(std::move(clang_args)));
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

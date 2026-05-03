#include "moult/clang/adapter.hpp"

#include "moult/core/types.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// This file uses the stable libclang C ABI. Some macOS CommandLineTools installs
// ship libclang.dylib without clang-c/Index.h, so we declare the small API subset
// needed here instead of requiring the development header.
extern "C" {

typedef void* CXIndex;
typedef void* CXTranslationUnit;
typedef void* CXClientData;
typedef void* CXDiagnostic;
typedef void* CXFile;

typedef struct {
    const void* data;
    unsigned private_flags;
} CXString;

typedef struct {
    const void* ptr_data[2];
    unsigned int_data;
} CXSourceLocation;

typedef struct {
    const void* ptr_data[2];
    unsigned begin_int_data;
    unsigned end_int_data;
} CXSourceRange;

typedef struct {
    const char* Filename;
    const char* Contents;
    unsigned long Length;
} CXUnsavedFile;

enum CXCursorKind {
    CXCursor_FunctionDecl = 8,
    CXCursor_VarDecl = 9,
    CXCursor_ParmDecl = 10,
    CXCursor_FieldDecl = 6,
    CXCursor_CXXMethod = 21,
    CXCursor_Constructor = 24,
    CXCursor_Destructor = 25,
    CXCursor_TypedefDecl = 20,
    CXCursor_DeclRefExpr = 101,
    CXCursor_CallExpr = 103,
    CXCursor_CStyleCastExpr = 117,
    CXCursor_CXXStaticCastExpr = 124,
    CXCursor_CXXDynamicCastExpr = 125,
    CXCursor_CXXReinterpretCastExpr = 126,
    CXCursor_CXXConstCastExpr = 127,
    CXCursor_CXXFunctionalCastExpr = 128,
    CXCursor_CXXNewExpr = 134,
    CXCursor_CXXDeleteExpr = 135,
    CXCursor_TranslationUnit = 300,
    CXCursor_MacroExpansion = 502,
    CXCursor_InclusionDirective = 503,
};

typedef struct {
    enum CXCursorKind kind;
    int xdata;
    const void* data[3];
} CXCursor;

enum CXTypeKind {
    CXType_Invalid = 0,
};

typedef struct {
    enum CXTypeKind kind;
    void* data[2];
} CXType;

enum CXChildVisitResult {
    CXChildVisit_Break = 0,
    CXChildVisit_Continue = 1,
    CXChildVisit_Recurse = 2,
};

enum CX_StorageClass {
    CX_SC_Invalid = 0,
    CX_SC_None = 1,
    CX_SC_Extern = 2,
    CX_SC_Static = 3,
    CX_SC_PrivateExtern = 4,
    CX_SC_OpenCLWorkGroupLocal = 5,
    CX_SC_Auto = 6,
    CX_SC_Register = 7,
};

typedef enum CXChildVisitResult (*CXCursorVisitor)(CXCursor cursor, CXCursor parent, CXClientData client_data);

CXIndex clang_createIndex(int excludeDeclarationsFromPCH, int displayDiagnostics);
void clang_disposeIndex(CXIndex index);
CXTranslationUnit clang_parseTranslationUnit(CXIndex index,
                                             const char* source_filename,
                                             const char* const* command_line_args,
                                             int num_command_line_args,
                                             CXUnsavedFile* unsaved_files,
                                             unsigned num_unsaved_files,
                                             unsigned options);
void clang_disposeTranslationUnit(CXTranslationUnit tu);
CXCursor clang_getTranslationUnitCursor(CXTranslationUnit tu);
unsigned clang_visitChildren(CXCursor parent, CXCursorVisitor visitor, CXClientData client_data);
enum CXCursorKind clang_getCursorKind(CXCursor cursor);
CXString clang_getCursorSpelling(CXCursor cursor);
CXCursor clang_getCursorReferenced(CXCursor cursor);
CXSourceRange clang_getCursorExtent(CXCursor cursor);
enum CX_StorageClass clang_Cursor_getStorageClass(CXCursor cursor);
CXType clang_getCursorType(CXCursor cursor);
CXString clang_getTypeSpelling(CXType type);
CXSourceLocation clang_getRangeStart(CXSourceRange range);
CXSourceLocation clang_getRangeEnd(CXSourceRange range);
void clang_getFileLocation(CXSourceLocation location,
                           CXFile* file,
                           unsigned* line,
                           unsigned* column,
                           unsigned* offset);
CXString clang_getFileName(CXFile file);
unsigned clang_getNumDiagnostics(CXTranslationUnit tu);
CXDiagnostic clang_getDiagnostic(CXTranslationUnit tu, unsigned index);
unsigned clang_getDiagnosticSeverity(CXDiagnostic diagnostic);
CXSourceLocation clang_getDiagnosticLocation(CXDiagnostic diagnostic);
CXString clang_formatDiagnostic(CXDiagnostic diagnostic, unsigned options);
unsigned clang_defaultDiagnosticDisplayOptions(void);
void clang_disposeDiagnostic(CXDiagnostic diagnostic);
const char* clang_getCString(CXString string);
void clang_disposeString(CXString string);
}

namespace moult::clang_adapter {
namespace {

constexpr unsigned translation_unit_detailed_preprocessing_record = 0x01;
constexpr unsigned translation_unit_keep_going = 0x200;

struct ModernizationOpportunity {
    std::string_view kind;
    std::string_view token;
    std::string_view replacement;
    core::Confidence confidence;
    bool edit_capable;
    std::string_view title;
    std::string_view message;
    std::string_view rationale;
};

constexpr ModernizationOpportunity use_nullptr{
    "use-nullptr",
    "NULL",
    "nullptr",
    core::Confidence::High,
    true,
    "replace NULL with nullptr",
    "Use nullptr for null pointer constants in modern C++.",
    "A Clang parse found NULL in C++ source; nullptr represents null pointer constants more precisely."};

constexpr ModernizationOpportunity use_noexcept{
    "use-noexcept",
    "throw()",
    "noexcept",
    core::Confidence::High,
    true,
    "replace empty exception specification with noexcept",
    "Use noexcept instead of the deprecated throw() exception specification.",
    "Clang found an empty dynamic exception specification, which has the same non-throwing intent as noexcept."};

constexpr ModernizationOpportunity remove_register{
    "remove-register",
    "register",
    "",
    core::Confidence::High,
    true,
    "remove obsolete register keyword",
    "Remove the obsolete register storage-class specifier.",
    "Clang classified this declaration as using the obsolete register storage class."};

constexpr ModernizationOpportunity replace_auto_ptr{
    "replace-auto-ptr",
    "std::auto_ptr",
    "std::unique_ptr",
    core::Confidence::Medium,
    false,
    "review std::auto_ptr usage",
    "std::auto_ptr is removed in C++17; migrate ownership semantics to std::unique_ptr.",
    "Clang found std::auto_ptr text in a declaration type; ownership-transfer semantics require manual review."};

constexpr ModernizationOpportunity prefer_using_alias{
    "prefer-using-alias",
    "typedef",
    "",
    core::Confidence::Medium,
    false,
    "review typedef alias",
    "Prefer using aliases over typedef declarations in modern C++.",
    "Clang found a typedef declaration; the exact using-alias rewrite should preserve declarator structure."};

constexpr ModernizationOpportunity review_raw_new{
    "review-raw-new",
    "new",
    "",
    core::Confidence::Medium,
    false,
    "review raw new expression",
    "Review raw new usage for replacement with ownership types or factory helpers.",
    "Clang found a raw new expression; RAII ownership may remove manual lifetime management."};

constexpr ModernizationOpportunity review_raw_delete{
    "review-raw-delete",
    "delete",
    "",
    core::Confidence::Medium,
    false,
    "review raw delete expression",
    "Review raw delete usage for replacement with RAII ownership.",
    "Clang found a raw delete expression; RAII ownership may remove manual lifetime management."};

constexpr ModernizationOpportunity review_c_style_cast{
    "review-c-style-cast",
    "(",
    "",
    core::Confidence::Medium,
    false,
    "review C-style cast",
    "Replace C-style casts with the narrowest C++ cast that expresses the intended conversion.",
    "Clang found a C-style cast, which can hide const, reinterpret, and static conversions behind one syntax."};

class OwnedCxString {
public:
    explicit OwnedCxString(CXString string) : string_(string) {}
    OwnedCxString(const OwnedCxString&) = delete;
    OwnedCxString& operator=(const OwnedCxString&) = delete;
    ~OwnedCxString() { clang_disposeString(string_); }

    [[nodiscard]] std::string str() const {
        const char* value = clang_getCString(string_);
        return value ? std::string(value) : std::string();
    }

private:
    CXString string_;
};

class OwnedDiagnostic {
public:
    explicit OwnedDiagnostic(CXDiagnostic diagnostic) : diagnostic_(diagnostic) {}
    OwnedDiagnostic(const OwnedDiagnostic&) = delete;
    OwnedDiagnostic& operator=(const OwnedDiagnostic&) = delete;
    ~OwnedDiagnostic() {
        if (diagnostic_) clang_disposeDiagnostic(diagnostic_);
    }

    [[nodiscard]] CXDiagnostic get() const noexcept { return diagnostic_; }

private:
    CXDiagnostic diagnostic_;
};

class OwnedTranslationUnit {
public:
    explicit OwnedTranslationUnit(CXTranslationUnit tu) : tu_(tu) {}
    OwnedTranslationUnit(const OwnedTranslationUnit&) = delete;
    OwnedTranslationUnit& operator=(const OwnedTranslationUnit&) = delete;
    ~OwnedTranslationUnit() {
        if (tu_) clang_disposeTranslationUnit(tu_);
    }

    [[nodiscard]] CXTranslationUnit get() const noexcept { return tu_; }
    [[nodiscard]] explicit operator bool() const noexcept { return tu_ != nullptr; }

private:
    CXTranslationUnit tu_;
};

class OwnedIndex {
public:
    OwnedIndex() : index_(clang_createIndex(0, 0)) {}
    OwnedIndex(const OwnedIndex&) = delete;
    OwnedIndex& operator=(const OwnedIndex&) = delete;
    ~OwnedIndex() {
        if (index_) clang_disposeIndex(index_);
    }

    [[nodiscard]] CXIndex get() const noexcept { return index_; }
    [[nodiscard]] explicit operator bool() const noexcept { return index_ != nullptr; }

private:
    CXIndex index_;
};

std::string cursor_spelling(CXCursor cursor) {
    return OwnedCxString(clang_getCursorSpelling(cursor)).str();
}

std::string cursor_type_spelling(CXCursor cursor) {
    const CXType type = clang_getCursorType(cursor);
    return OwnedCxString(clang_getTypeSpelling(type)).str();
}

std::string file_name(CXFile file) {
    return OwnedCxString(clang_getFileName(file)).str();
}

bool path_matches(std::string_view clang_path, std::string_view source_path) {
    if (clang_path == source_path) return true;

    std::error_code ec;
    const auto clang_abs = std::filesystem::weakly_canonical(std::filesystem::absolute(std::filesystem::path(clang_path)), ec);
    if (ec) return false;
    const auto source_abs = std::filesystem::weakly_canonical(std::filesystem::absolute(std::filesystem::path(source_path)), ec);
    if (ec) return false;
    return clang_abs == source_abs;
}

const core::SourceBuffer* matching_source(std::string_view clang_path, const core::SourceStore& sources) {
    for (const auto& [source_path, source] : sources.files()) {
        if (path_matches(clang_path, source_path)) return &source;
    }
    return nullptr;
}

std::optional<core::SourceRange> source_range_from_cx(CXSourceRange range, const core::SourceStore& sources) {
    CXFile start_file = nullptr;
    CXFile end_file = nullptr;
    unsigned start_offset = 0;
    unsigned end_offset = 0;
    clang_getFileLocation(clang_getRangeStart(range), &start_file, nullptr, nullptr, &start_offset);
    clang_getFileLocation(clang_getRangeEnd(range), &end_file, nullptr, nullptr, &end_offset);
    if (!start_file || start_file != end_file || end_offset < start_offset) return std::nullopt;

    const auto* source = matching_source(file_name(start_file), sources);
    if (!source) return std::nullopt;

    core::SourceRange out{source->path(), start_offset, end_offset};
    if (!source->contains(out)) return std::nullopt;
    return out;
}

std::optional<core::SourceRange> cursor_range(CXCursor cursor, const core::SourceStore& sources) {
    return source_range_from_cx(clang_getCursorExtent(cursor), sources);
}

std::string subject_for(const core::SourceRange& range) {
    return "site:" + range.file + ":" + std::to_string(range.begin);
}

void add_fact(core::FactStore& facts,
              std::string kind,
              std::string subject,
              std::string predicate,
              std::string object,
              std::optional<core::SourceRange> range,
              core::Attributes attrs = {}) {
    attrs.emplace("adapter", "libclang");
    facts.add(std::move(kind), std::move(subject), std::move(predicate), std::move(object), std::move(range), std::move(attrs));
}

void add_opportunity(core::FactStore& facts,
                     const core::SourceRange& range,
                     const ModernizationOpportunity& opportunity) {
    core::Attributes attrs{
        {"token", std::string(opportunity.token)},
        {"replacement", std::string(opportunity.replacement)},
        {"title", std::string(opportunity.title)},
        {"message", std::string(opportunity.message)},
        {"rationale", std::string(opportunity.rationale)},
        {"confidence", core::to_string(opportunity.confidence)},
        {"edit_capable", opportunity.edit_capable ? "true" : "false"},
        {"scanner", "libclang"}};

    facts.add("cpp.modernisation.opportunity",
              subject_for(range),
              "opportunity",
              std::string(opportunity.kind),
              range,
              std::move(attrs));
}

std::optional<core::SourceRange> find_token_in_range(const core::SourceStore& sources,
                                                     const core::SourceRange& outer,
                                                     std::string_view token,
                                                     bool include_following_space = false) {
    const auto* source = sources.get(outer.file);
    if (!source || !source->contains(outer)) return std::nullopt;

    const std::string_view text = source->slice(outer);
    const std::size_t relative = text.find(token);
    if (relative == std::string_view::npos) return std::nullopt;

    std::size_t end = outer.begin + relative + token.size();
    if (include_following_space && end < source->size()) {
        const char c = source->text()[end];
        if (c == ' ' || c == '\t') ++end;
    }
    return source->range(outer.begin + relative, end);
}

std::optional<core::SourceRange> find_throw_specifier(const core::SourceStore& sources,
                                                      const core::SourceRange& function_range) {
    const auto* source = sources.get(function_range.file);
    if (!source || !source->contains(function_range)) return std::nullopt;

    std::string_view text = source->slice(function_range);
    const std::size_t body_begin = text.find('{');
    const std::size_t declaration_end = body_begin == std::string_view::npos ? text.size() : body_begin;
    text = text.substr(0, declaration_end);

    const std::size_t relative = text.find(use_noexcept.token);
    if (relative == std::string_view::npos) return std::nullopt;
    return source->range(function_range.begin + relative, function_range.begin + relative + use_noexcept.token.size());
}

void report_diagnostics(CXTranslationUnit tu, const core::SourceStore& sources, core::DiagnosticSink& diagnostics) {
    const unsigned count = clang_getNumDiagnostics(tu);
    const unsigned options = clang_defaultDiagnosticDisplayOptions();
    for (unsigned i = 0; i < count; ++i) {
        OwnedDiagnostic diagnostic(clang_getDiagnostic(tu, i));
        if (!diagnostic.get()) continue;

        const unsigned clang_severity = clang_getDiagnosticSeverity(diagnostic.get());
        core::Severity severity = core::Severity::Note;
        if (clang_severity == 2) {
            severity = core::Severity::Warning;
        } else if (clang_severity >= 3) {
            severity = core::Severity::Error;
        }

        CXSourceLocation location = clang_getDiagnosticLocation(diagnostic.get());
        CXFile file = nullptr;
        unsigned offset = 0;
        clang_getFileLocation(location, &file, nullptr, nullptr, &offset);
        std::optional<core::SourceRange> range;
        if (file) {
            const auto* source = matching_source(file_name(file), sources);
            if (source && offset <= source->size()) range = source->range(offset, offset);
        }

        diagnostics.report(core::Diagnostic{
            severity,
            "clang",
            OwnedCxString(clang_formatDiagnostic(diagnostic.get(), options)).str(),
            std::move(range),
            {{"adapter", "libclang"}}});
    }
}

struct VisitorState {
    const core::SourceStore* sources = nullptr;
    core::FactStore* facts = nullptr;
};

enum CXChildVisitResult visit_cursor(CXCursor cursor, CXCursor, CXClientData client_data) {
    auto* state = static_cast<VisitorState*>(client_data);
    const auto kind = clang_getCursorKind(cursor);
    const auto range = cursor_range(cursor, *state->sources);

    if (range) {
        const std::string subject = subject_for(*range);
        switch (kind) {
            case CXCursor_FunctionDecl:
            case CXCursor_CXXMethod:
            case CXCursor_Constructor:
            case CXCursor_Destructor: {
                const std::string spelling = cursor_spelling(cursor);
                add_fact(*state->facts, "cxx.function", subject, "spelling", spelling, range);
                if (const auto throw_range = find_throw_specifier(*state->sources, *range)) {
                    add_opportunity(*state->facts, *throw_range, use_noexcept);
                }
                break;
            }
            case CXCursor_CallExpr: {
                const std::string spelling = cursor_spelling(cursor);
                if (!spelling.empty()) {
                    add_fact(*state->facts, "cxx.call", subject, "callee.spelling", spelling, range);
                }
                const CXCursor referenced = clang_getCursorReferenced(cursor);
                const std::string referenced_spelling = cursor_spelling(referenced);
                if (!referenced_spelling.empty() && referenced_spelling != spelling) {
                    add_fact(*state->facts, "cxx.call", subject, "callee.referenced", referenced_spelling, range);
                }
                break;
            }
            case CXCursor_TypedefDecl: {
                const std::string spelling = cursor_spelling(cursor);
                const std::string type = cursor_type_spelling(cursor);
                if (!spelling.empty()) add_fact(*state->facts, "cxx.typedef", subject, "spelling", spelling, range);
                if (!type.empty()) add_fact(*state->facts, "cxx.typedef", subject, "type", type, range);
                const auto typedef_range = find_token_in_range(*state->sources, *range, prefer_using_alias.token);
                add_opportunity(*state->facts, typedef_range.value_or(*range), prefer_using_alias);
                break;
            }
            case CXCursor_VarDecl:
            case CXCursor_ParmDecl:
            case CXCursor_FieldDecl: {
                const std::string spelling = cursor_spelling(cursor);
                const std::string type = cursor_type_spelling(cursor);
                if (!spelling.empty()) add_fact(*state->facts, "cxx.declaration", subject, "spelling", spelling, range);
                if (!type.empty()) add_fact(*state->facts, "cxx.declaration", subject, "type", type, range);
                if (type.find("std::auto_ptr") != std::string::npos || type.find("auto_ptr") != std::string::npos) {
                    const auto auto_ptr_range = find_token_in_range(*state->sources, *range, replace_auto_ptr.token);
                    add_opportunity(*state->facts, auto_ptr_range.value_or(*range), replace_auto_ptr);
                }
                if (clang_Cursor_getStorageClass(cursor) == CX_SC_Register) {
                    const auto register_range = find_token_in_range(*state->sources, *range, remove_register.token, true);
                    if (register_range) add_opportunity(*state->facts, *register_range, remove_register);
                }
                break;
            }
            case CXCursor_DeclRefExpr:
                if (cursor_spelling(cursor) == use_nullptr.token) {
                    add_opportunity(*state->facts, *range, use_nullptr);
                }
                break;
            case CXCursor_MacroExpansion:
                add_fact(*state->facts, "cxx.macro.expansion", subject, "name", cursor_spelling(cursor), range);
                if (cursor_spelling(cursor) == use_nullptr.token) {
                    add_opportunity(*state->facts, *range, use_nullptr);
                }
                break;
            case CXCursor_InclusionDirective:
                add_fact(*state->facts, "cxx.include", subject, "spelling", cursor_spelling(cursor), range);
                break;
            case CXCursor_CStyleCastExpr:
                add_fact(*state->facts, "cxx.cast", subject, "kind", "c_style", range);
                add_opportunity(*state->facts, *range, review_c_style_cast);
                break;
            case CXCursor_CXXStaticCastExpr:
            case CXCursor_CXXDynamicCastExpr:
            case CXCursor_CXXReinterpretCastExpr:
            case CXCursor_CXXConstCastExpr:
            case CXCursor_CXXFunctionalCastExpr:
                add_fact(*state->facts, "cxx.cast", subject, "kind", std::to_string(static_cast<int>(kind)), range);
                break;
            case CXCursor_CXXNewExpr:
                add_fact(*state->facts, "cxx.allocation", subject, "operator", "new", range);
                if (const auto new_range = find_token_in_range(*state->sources, *range, review_raw_new.token)) {
                    add_opportunity(*state->facts, *new_range, review_raw_new);
                }
                break;
            case CXCursor_CXXDeleteExpr:
                add_fact(*state->facts, "cxx.allocation", subject, "operator", "delete", range);
                if (const auto delete_range = find_token_in_range(*state->sources, *range, review_raw_delete.token)) {
                    add_opportunity(*state->facts, *delete_range, review_raw_delete);
                }
                break;
            default:
                break;
        }
    }

    return CXChildVisit_Recurse;
}

std::vector<const char*> c_args(const std::vector<std::string>& args) {
    std::vector<const char*> out;
    out.reserve(args.size());
    for (const auto& arg : args) out.push_back(arg.c_str());
    return out;
}

const std::vector<std::string>& args_for_source(std::string_view source_path,
                                                const ClangCppModernizationAdapter::CompileCommandMap& compile_commands,
                                                const std::vector<std::string>& fallback_args) {
    auto exact = compile_commands.find(std::string(source_path));
    if (exact != compile_commands.end()) return exact->second;
    for (const auto& [command_path, args] : compile_commands) {
        if (path_matches(command_path, source_path)) return args;
    }
    return fallback_args;
}

} // namespace

ClangCppModernizationAdapter::ClangCppModernizationAdapter()
    : command_line_args_{"-x", "c++", "-std=c++14"} {}

ClangCppModernizationAdapter::ClangCppModernizationAdapter(std::vector<std::string> command_line_args)
    : command_line_args_(std::move(command_line_args)) {
    if (command_line_args_.empty()) {
        command_line_args_ = {"-x", "c++", "-std=c++14"};
    }
}

ClangCppModernizationAdapter::ClangCppModernizationAdapter(std::vector<std::string> fallback_command_line_args,
                                                           CompileCommandMap compile_commands)
    : command_line_args_(std::move(fallback_command_line_args)), compile_commands_(std::move(compile_commands)) {
    if (command_line_args_.empty()) {
        command_line_args_ = {"-x", "c++", "-std=c++14"};
    }
}

void ClangCppModernizationAdapter::analyze(const core::SourceStore& sources,
                                           core::FactStore& facts,
                                           core::DiagnosticSink& diagnostics,
                                           const core::RunOptions&) const {
    OwnedIndex index;
    if (!index) {
        diagnostics.error("clang.index", "failed to create libclang index");
        return;
    }

    for (const auto& [path, source] : sources.files()) {
        const auto& source_args = args_for_source(path, compile_commands_, command_line_args_);
        const auto args = c_args(source_args);
        CXUnsavedFile unsaved{source.path().c_str(), source.text().c_str(), static_cast<unsigned long>(source.text().size())};
        OwnedTranslationUnit tu(clang_parseTranslationUnit(index.get(),
                                                           path.c_str(),
                                                           args.data(),
                                                           static_cast<int>(args.size()),
                                                           &unsaved,
                                                           1,
                                                           translation_unit_detailed_preprocessing_record |
                                                               translation_unit_keep_going));
        if (!tu) {
            diagnostics.error("clang.parse", "libclang failed to parse translation unit: " + path, source.range(0, 0));
            continue;
        }

        add_fact(facts, "cxx.translation_unit", "tu:" + path, "path", path, core::SourceRange{source.path(), 0, source.size()});
        report_diagnostics(tu.get(), sources, diagnostics);
        VisitorState state{&sources, &facts};
        clang_visitChildren(clang_getTranslationUnitCursor(tu.get()), visit_cursor, &state);
    }
}

} // namespace moult::clang_adapter

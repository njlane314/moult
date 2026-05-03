#include "moult/core/source.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace moult::core {

namespace {

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool has_c_abi_header_markers(std::string_view text) {
    return text.find("__cplusplus") != std::string_view::npos && text.find("extern \"C\"") != std::string_view::npos;
}

bool has_obvious_cxx_header_markers(std::string_view text) {
    return text.find("namespace ") != std::string_view::npos || text.find("namespace\t") != std::string_view::npos ||
           text.find("template <") != std::string_view::npos || text.find("template<") != std::string_view::npos ||
           text.find("class ") != std::string_view::npos || text.find("class\t") != std::string_view::npos ||
           text.find("std::") != std::string_view::npos || text.find("public:") != std::string_view::npos ||
           text.find("private:") != std::string_view::npos || text.find("protected:") != std::string_view::npos ||
           text.find("#include <cstddef>") != std::string_view::npos ||
           text.find("#include <cstdint>") != std::string_view::npos ||
           text.find("#include <memory>") != std::string_view::npos ||
           text.find("#include <string>") != std::string_view::npos ||
           text.find("#include <vector>") != std::string_view::npos;
}

bool has_plain_c_header_markers(std::string_view text) {
    return text.find("#include <stddef.h>") != std::string_view::npos ||
           text.find("#include <stdint.h>") != std::string_view::npos ||
           text.find("#include <stdlib.h>") != std::string_view::npos ||
           text.find("#include <stdio.h>") != std::string_view::npos ||
           text.find("#include <string.h>") != std::string_view::npos ||
           text.find("typedef struct ") != std::string_view::npos ||
           text.find("typedef enum ") != std::string_view::npos;
}

} // namespace

std::string_view to_string(SourceLanguage language) noexcept {
    switch (language) {
        case SourceLanguage::Unknown: return "unknown";
        case SourceLanguage::C: return "c";
        case SourceLanguage::Cxx: return "c++";
    }
    return "unknown";
}

SourceLanguage infer_source_language(const std::filesystem::path& path) {
    const std::string ext = lowercase(path.extension().string());
    if (ext == ".c") return SourceLanguage::C;
    if (ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".c++" || ext == ".hh" || ext == ".hpp" ||
        ext == ".hxx" || ext == ".h++" || ext == ".ipp" || ext == ".ixx" || ext == ".tpp" || ext == ".txx") {
        return SourceLanguage::Cxx;
    }
    if (ext == ".h") return SourceLanguage::Cxx;
    return SourceLanguage::Unknown;
}

SourceLanguage infer_source_language(const std::filesystem::path& path, std::string_view text) {
    const auto inferred = infer_source_language(path);
    const std::string ext = lowercase(path.extension().string());
    if (ext == ".h") {
        if (has_c_abi_header_markers(text)) return SourceLanguage::C;
        if (has_obvious_cxx_header_markers(text)) return SourceLanguage::Cxx;
        if (has_plain_c_header_markers(text)) return SourceLanguage::C;
        return SourceLanguage::Unknown;
    }
    return inferred;
}

bool is_cxx_language(SourceLanguage language) noexcept {
    return language == SourceLanguage::Cxx;
}

SourceBuffer::SourceBuffer(std::string path, std::string text)
    : path_(std::move(path)), text_(std::move(text)), language_(infer_source_language(path_, text_)) {
    recompute_line_starts();
}

SourceBuffer::SourceBuffer(std::string path, std::string text, SourceLanguage language)
    : path_(std::move(path)), text_(std::move(text)), language_(language) {
    recompute_line_starts();
}

void SourceBuffer::recompute_line_starts() {
    line_starts_.clear();
    line_starts_.push_back(0);
    for (std::size_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n') line_starts_.push_back(i + 1);
    }
}

bool SourceBuffer::contains(SourceRange r) const noexcept {
    return r.file == path_ && r.begin <= r.end && r.end <= text_.size();
}

std::string_view SourceBuffer::slice(std::size_t begin, std::size_t end) const {
    if (begin > end || end > text_.size()) {
        throw std::out_of_range("invalid source slice");
    }
    return std::string_view(text_).substr(begin, end - begin);
}

std::string_view SourceBuffer::slice(SourceRange r) const {
    if (r.file != path_) throw std::out_of_range("source range file mismatch");
    return slice(r.begin, r.end);
}

LineColumn SourceBuffer::line_column(std::size_t offset) const {
    if (offset > text_.size()) throw std::out_of_range("source offset out of range");
    auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    std::size_t line_index = static_cast<std::size_t>(std::distance(line_starts_.begin(), it));
    if (line_index == 0) line_index = 1;
    const std::size_t line_start = line_starts_[line_index - 1];
    return LineColumn{line_index, offset - line_start + 1};
}

SourceRange SourceBuffer::range(std::size_t begin, std::size_t end) const {
    if (begin > end || end > text_.size()) throw std::out_of_range("invalid source range");
    return SourceRange{path_, begin, end};
}

void SourceStore::add(std::string path, std::string text) {
    const SourceLanguage language = infer_source_language(path, text);
    add(std::move(path), std::move(text), language);
}

void SourceStore::add(std::string path, std::string text, SourceLanguage language) {
    const std::string key = path;
    files_[key] = SourceBuffer(std::move(path), std::move(text), language);
}

bool SourceStore::load_file(const std::filesystem::path& path) {
    return load_file(path, infer_source_language(path));
}

bool SourceStore::load_file(const std::filesystem::path& path, SourceLanguage language) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (language == SourceLanguage::Unknown) language = infer_source_language(path, text);
    add(path.string(), std::move(text), language);
    return true;
}

bool SourceStore::load_files(const std::vector<std::filesystem::path>& paths) {
    bool ok = true;
    for (const auto& path : paths) ok = load_file(path) && ok;
    return ok;
}

const SourceBuffer* SourceStore::get(std::string_view path) const {
    auto it = files_.find(std::string(path));
    return it == files_.end() ? nullptr : &it->second;
}

SourceBuffer* SourceStore::get(std::string_view path) {
    auto it = files_.find(std::string(path));
    return it == files_.end() ? nullptr : &it->second;
}

bool SourceStore::contains(std::string_view path) const {
    return files_.find(std::string(path)) != files_.end();
}

std::vector<std::string> SourceStore::paths() const {
    std::vector<std::string> out;
    out.reserve(files_.size());
    for (const auto& [path, _] : files_) out.push_back(path);
    return out;
}

} // namespace moult::core

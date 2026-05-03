#include "moult/core/source.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace moult::core {

SourceBuffer::SourceBuffer(std::string path, std::string text)
    : path_(std::move(path)), text_(std::move(text)) {
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
    const std::string key = path;
    files_[key] = SourceBuffer(std::move(path), std::move(text));
}

bool SourceStore::load_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    add(path.string(), std::move(text));
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

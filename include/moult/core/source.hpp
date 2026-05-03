#pragma once

#include "moult/core/types.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moult::core {

class SourceBuffer {
public:
    SourceBuffer() = default;
    SourceBuffer(std::string path, std::string text);

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] std::size_t size() const noexcept { return text_.size(); }

    [[nodiscard]] bool contains(std::size_t offset) const noexcept { return offset <= text_.size(); }
    [[nodiscard]] bool contains(SourceRange range) const noexcept;
    [[nodiscard]] std::string_view slice(std::size_t begin, std::size_t end) const;
    [[nodiscard]] std::string_view slice(SourceRange range) const;
    [[nodiscard]] LineColumn line_column(std::size_t offset) const;
    [[nodiscard]] SourceRange range(std::size_t begin, std::size_t end) const;

private:
    void recompute_line_starts();

    std::string path_;
    std::string text_;
    std::vector<std::size_t> line_starts_ {0};
};

class SourceStore {
public:
    void add(std::string path, std::string text);
    bool load_file(const std::filesystem::path& path);
    bool load_files(const std::vector<std::filesystem::path>& paths);

    [[nodiscard]] const SourceBuffer* get(std::string_view path) const;
    [[nodiscard]] SourceBuffer* get(std::string_view path);
    [[nodiscard]] bool contains(std::string_view path) const;
    [[nodiscard]] std::vector<std::string> paths() const;
    [[nodiscard]] const std::map<std::string, SourceBuffer>& files() const noexcept { return files_; }
    [[nodiscard]] bool empty() const noexcept { return files_.empty(); }

private:
    std::map<std::string, SourceBuffer> files_;
};

} // namespace moult::core

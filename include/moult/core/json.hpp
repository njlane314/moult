#pragma once

#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace moult::core {

std::string json_escape(std::string_view input);
void json_write_string(std::ostream& os, std::string_view input);

class JsonObjectWriter {
public:
    explicit JsonObjectWriter(std::ostream& os);
    ~JsonObjectWriter();

    JsonObjectWriter(const JsonObjectWriter&) = delete;
    JsonObjectWriter& operator=(const JsonObjectWriter&) = delete;

    void string_field(std::string_view name, std::string_view value);
    void bool_field(std::string_view name, bool value);
    void number_field(std::string_view name, std::size_t value);
    void raw_field(std::string_view name, std::string_view raw_json);
    void string_array_field(std::string_view name, const std::vector<std::string>& values);
    void object_string_map_field(std::string_view name, const std::map<std::string, std::string>& values);
    void finish();

private:
    void comma();
    std::ostream& os_;
    bool first_ = true;
    bool closed_ = false;
};

std::string string_map_to_json(const std::map<std::string, std::string>& values);
std::string string_array_to_json(const std::vector<std::string>& values);

} // namespace moult::core

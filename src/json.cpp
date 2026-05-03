#include "moult/core/json.hpp"

#include <iomanip>
#include <sstream>

namespace moult::core {

std::string json_escape(std::string_view input) {
    std::ostringstream os;
    for (unsigned char c : input) {
        switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (c < 0x20) {
                    os << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
                } else {
                    os << static_cast<char>(c);
                }
                break;
        }
    }
    return os.str();
}

void json_write_string(std::ostream& os, std::string_view input) {
    os << '"' << json_escape(input) << '"';
}

JsonObjectWriter::JsonObjectWriter(std::ostream& os) : os_(os) {
    os_ << "{";
}

JsonObjectWriter::~JsonObjectWriter() {
    finish();
}

void JsonObjectWriter::finish() {
    if (!closed_) {
        os_ << "}";
        closed_ = true;
    }
}

void JsonObjectWriter::comma() {
    if (!first_) os_ << ",";
    first_ = false;
}

void JsonObjectWriter::string_field(std::string_view name, std::string_view value) {
    comma();
    json_write_string(os_, name);
    os_ << ":";
    json_write_string(os_, value);
}

void JsonObjectWriter::bool_field(std::string_view name, bool value) {
    comma();
    json_write_string(os_, name);
    os_ << ":" << (value ? "true" : "false");
}

void JsonObjectWriter::number_field(std::string_view name, std::size_t value) {
    comma();
    json_write_string(os_, name);
    os_ << ":" << value;
}

void JsonObjectWriter::raw_field(std::string_view name, std::string_view raw_json) {
    comma();
    json_write_string(os_, name);
    os_ << ":" << raw_json;
}

void JsonObjectWriter::string_array_field(std::string_view name, const std::vector<std::string>& values) {
    raw_field(name, string_array_to_json(values));
}

void JsonObjectWriter::object_string_map_field(std::string_view name, const std::map<std::string, std::string>& values) {
    raw_field(name, string_map_to_json(values));
}

std::string string_map_to_json(const std::map<std::string, std::string>& values) {
    std::ostringstream os;
    os << "{";
    bool first = true;
    for (const auto& [k, v] : values) {
        if (!first) os << ",";
        first = false;
        json_write_string(os, k);
        os << ":";
        json_write_string(os, v);
    }
    os << "}";
    return os.str();
}

std::string string_array_to_json(const std::vector<std::string>& values) {
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& v : values) {
        if (!first) os << ",";
        first = false;
        json_write_string(os, v);
    }
    os << "]";
    return os.str();
}

} // namespace moult::core

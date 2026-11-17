#include "output_preferences.h"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <string_view>
#include <utility>

namespace multiroom {
namespace {

std::vector<std::string_view> split_fields(std::string_view line) {
    std::vector<std::string_view> fields;
    for (auto separator = line.find('|'); separator != line.npos; separator = line.find('|')) {
        fields.push_back(line.substr(0, separator));
        line.remove_prefix(separator + 1);
    }
    fields.push_back(line);
    return fields;
}

std::string encode_id(std::string_view id) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(id.size() * 2);
    for (const unsigned char byte : id) {
        encoded.push_back(digits[byte >> 4]);
        encoded.push_back(digits[byte & 0x0f]);
    }
    return encoded;
}

std::string decode_id(std::string_view encoded) {
    if (encoded.size() % 2 != 0) return {};
    std::string id;
    id.reserve(encoded.size() / 2);
    for (size_t pos = 0; pos < encoded.size(); pos += 2) {
        unsigned int byte = 0;
        const auto end = encoded.data() + pos + 2;
        const auto result = std::from_chars(encoded.data() + pos, end, byte, 16);
        if (result.ec != std::errc{} || result.ptr != end || byte == 0) return {};
        id.push_back(static_cast<char>(byte));
    }
    return id;
}

bool read_int(std::string_view text, int& value) {
    if (text.empty()) return false;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool has_alias(const OutputDevice& output, const std::string& id) {
    return std::find(output.aliases.begin(), output.aliases.end(), id) != output.aliases.end();
}

bool uniquely_matches_alias(
    const std::vector<OutputDevice>& outputs, const OutputDevice& output, const std::string& id) {
    return has_alias(output, id) && std::none_of(outputs.begin(), outputs.end(), [&](const auto& other) {
        return other.id != output.id && (other.id == id || has_alias(other, id));
    });
}

}  // namespace

std::vector<SavedOutputPreferences> parse_output_preferences(const std::string& text) {
    std::vector<SavedOutputPreferences> preferences;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto fields = split_fields(line);
        const bool encoded = fields.front() == "v2";
        const size_t first = encoded ? 1 : 0;
        const size_t count = fields.size() - first;
        if (count < 3 || count > 5 || (encoded && count != 5)) continue;

        SavedOutputPreferences entry;
        entry.output_id = encoded ? decode_id(fields[first]) : std::string(fields[first]);
        int volume = 50;
        if (entry.output_id.empty() || !read_int(fields[first + 2], volume)) continue;
        entry.selected = fields[first + 1] == "1";
        entry.volume = std::clamp(volume, 0, 100);
        entry.visible_in_dropdown = count < 4 || fields[first + 3] != "0";
        int offset_ms = 0;
        if (count == 5 && read_int(fields[first + 4], offset_ms)) {
            entry.offset_ms = std::clamp(offset_ms, -kMaxOutputOffsetMs, kMaxOutputOffsetMs);
        }
        preferences.push_back(std::move(entry));
    }
    return preferences;
}

std::string serialize_output_preferences(const std::vector<SavedOutputPreferences>& preferences) {
    std::ostringstream encoded;
    for (const auto& entry : preferences) {
        if (entry.output_id.empty()) continue;
        encoded << "v2|" << encode_id(entry.output_id) << '|' << (entry.selected ? '1' : '0') << '|'
                << entry.volume << '|' << (entry.visible_in_dropdown ? '1' : '0') << '|' << entry.offset_ms << '\n';
    }
    return encoded.str();
}

void record_output_preferences(
    std::vector<SavedOutputPreferences>& preferences, const OutputDevice& output) {
    std::erase_if(preferences, [&](const auto& entry) { return entry.output_id == output.id; });
    preferences.push_back({
        output.id,
        output.selected,
        std::clamp(output.volume, 0, 100),
        output.visible_in_dropdown,
        std::clamp(output.offset_ms, -kMaxOutputOffsetMs, kMaxOutputOffsetMs),
    });
}

bool restore_output_preferences(
    std::vector<SavedOutputPreferences>& preferences, std::vector<OutputDevice>& outputs) {
    bool migrated = false;
    for (auto& output : outputs) {
        auto saved = std::find_if(preferences.rbegin(), preferences.rend(), [&](const auto& entry) {
            return entry.output_id == output.id;
        });
        if (saved == preferences.rend()) {
            saved = std::find_if(preferences.rbegin(), preferences.rend(), [&](const auto& entry) {
                return uniquely_matches_alias(outputs, output, entry.output_id);
            });
        }
        if (saved == preferences.rend()) continue;

        output.selected = saved->selected;
        output.volume = saved->volume;
        output.visible_in_dropdown = saved->visible_in_dropdown;
        output.offset_ms = saved->offset_ms;
        if (std::erase_if(preferences, [&](const auto& entry) {
                return uniquely_matches_alias(outputs, output, entry.output_id);
            }) != 0) {
            record_output_preferences(preferences, output);
            migrated = true;
        }
    }
    return migrated;
}

bool has_airplay2_endpoint(const OutputDevice& output) {
    return output.supports_airplay2 && !output.endpoint_host.empty() && output.endpoint_port != 0;
}

std::vector<std::string> selected_playback_ids(const std::vector<OutputDevice>& outputs) {
    std::vector<std::string> result;
    for (const auto& output : outputs) {
        if (output.selected && has_airplay2_endpoint(output)) result.push_back(output.id);
    }
    return result;
}

bool has_undiscovered_selection(
    const std::vector<SavedOutputPreferences>& preferences,
    const std::vector<OutputDevice>& outputs) {
    return std::any_of(preferences.begin(), preferences.end(), [&](const auto& entry) {
        if (!entry.selected) return false;
        return std::none_of(outputs.begin(), outputs.end(), [&](const auto& output) {
            return output.selected && has_airplay2_endpoint(output) &&
                (output.id == entry.output_id || uniquely_matches_alias(outputs, output, entry.output_id));
        });
    });
}

}  // namespace multiroom

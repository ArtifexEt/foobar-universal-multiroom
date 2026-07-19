#include "speaker_groups.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace multiroom {
namespace {

std::string trim_ascii(std::string text) {
    const auto not_space = [](unsigned char ch) { return std::isspace(ch) == 0; };
    const auto first = std::find_if(text.begin(), text.end(), not_space);
    if (first == text.end()) return {};
    const auto last = std::find_if(text.rbegin(), text.rend(), not_space).base();
    return std::string(first, last);
}

std::string hex_encode(const std::string& text) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(text.size() * 2);
    for (const unsigned char byte : text) {
        result.push_back(kHex[(byte >> 4) & 0x0f]);
        result.push_back(kHex[byte & 0x0f]);
    }
    return result;
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}

bool hex_decode(const std::string& text, std::string& result) {
    if ((text.size() % 2) != 0) return false;
    result.clear();
    result.reserve(text.size() / 2);
    for (size_t index = 0; index < text.size(); index += 2) {
        const int high = hex_value(text[index]);
        const int low = hex_value(text[index + 1]);
        if (high < 0 || low < 0) return false;
        result.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> result;
    std::stringstream stream(text);
    std::string field;
    while (std::getline(stream, field, separator)) result.push_back(field);
    return result;
}

bool output_matches_id(const OutputDevice& output, const std::string& id) {
    return output.id == id ||
        std::find(output.aliases.begin(), output.aliases.end(), id) != output.aliases.end();
}

}  // namespace

SpeakerGroup normalize_speaker_group(SpeakerGroup group) {
    group.id = trim_ascii(std::move(group.id));
    group.name = trim_ascii(std::move(group.name));

    std::vector<std::string> normalized_ids;
    normalized_ids.reserve(group.output_ids.size());
    for (auto& id : group.output_ids) {
        id = trim_ascii(std::move(id));
        if (id.empty() || std::find(normalized_ids.begin(), normalized_ids.end(), id) != normalized_ids.end()) {
            continue;
        }
        normalized_ids.push_back(std::move(id));
    }
    group.output_ids = std::move(normalized_ids);
    return group;
}

std::string serialize_speaker_groups(const std::vector<SpeakerGroup>& groups) {
    std::ostringstream result;
    for (auto group : groups) {
        group = normalize_speaker_group(std::move(group));
        if (group.id.empty() || group.name.empty() || group.output_ids.empty()) continue;

        result << "v1|" << hex_encode(group.id) << '|' << hex_encode(group.name) << '|';
        for (size_t index = 0; index < group.output_ids.size(); ++index) {
            if (index != 0) result << ',';
            result << hex_encode(group.output_ids[index]);
        }
        result << '\n';
    }
    return result.str();
}

std::vector<SpeakerGroup> deserialize_speaker_groups(const std::string& text) {
    std::vector<SpeakerGroup> result;
    std::stringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto fields = split(line, '|');
        if (fields.size() != 4 || fields[0] != "v1") continue;

        SpeakerGroup group;
        if (!hex_decode(fields[1], group.id) || !hex_decode(fields[2], group.name)) continue;
        for (const auto& encoded_id : split(fields[3], ',')) {
            std::string id;
            if (!hex_decode(encoded_id, id)) {
                group.output_ids.clear();
                break;
            }
            group.output_ids.push_back(std::move(id));
        }
        group = normalize_speaker_group(std::move(group));
        if (group.id.empty() || group.name.empty() || group.output_ids.empty()) continue;
        if (std::any_of(result.begin(), result.end(), [&](const auto& existing) { return existing.id == group.id; })) {
            continue;
        }
        result.push_back(std::move(group));
    }
    return result;
}

std::vector<std::string> resolve_speaker_group_output_ids(
    const SpeakerGroup& group,
    const std::vector<OutputDevice>& outputs) {
    std::vector<std::string> result;
    for (const auto& output : outputs) {
        const bool member = std::any_of(group.output_ids.begin(), group.output_ids.end(), [&](const auto& id) {
            return output_matches_id(output, id);
        });
        if (member && std::find(result.begin(), result.end(), output.id) == result.end()) {
            result.push_back(output.id);
        }
    }
    return result;
}

bool speaker_group_contains_persisted_output(
    const SpeakerGroup& group,
    const std::string& persisted_output_id,
    const std::vector<OutputDevice>& known_outputs) {
    if (std::find(group.output_ids.begin(), group.output_ids.end(), persisted_output_id) != group.output_ids.end()) {
        return true;
    }
    const auto output = std::find_if(known_outputs.begin(), known_outputs.end(), [&](const auto& candidate) {
        return output_matches_id(candidate, persisted_output_id);
    });
    if (output == known_outputs.end()) return false;
    return std::any_of(group.output_ids.begin(), group.output_ids.end(), [&](const auto& group_id) {
        return output_matches_id(*output, group_id);
    });
}

bool speaker_group_matches_selection(
    const SpeakerGroup& group,
    const std::vector<OutputDevice>& outputs) {
    auto expected = resolve_speaker_group_output_ids(group, outputs);
    if (expected.empty()) return false;
    std::sort(expected.begin(), expected.end());

    std::vector<std::string> selected;
    for (const auto& output : outputs) {
        if (output.selected) selected.push_back(output.id);
    }
    std::sort(selected.begin(), selected.end());
    return selected == expected;
}

}  // namespace multiroom

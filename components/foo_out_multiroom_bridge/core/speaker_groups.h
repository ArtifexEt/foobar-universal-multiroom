#pragma once

#include "../transport.h"

#include <string>
#include <vector>

namespace multiroom {

struct SpeakerGroup {
    std::string id;
    std::string name;
    std::vector<std::string> output_ids;
};

SpeakerGroup normalize_speaker_group(SpeakerGroup group);
std::string serialize_speaker_groups(const std::vector<SpeakerGroup>& groups);
std::vector<SpeakerGroup> deserialize_speaker_groups(const std::string& text);

std::vector<std::string> resolve_speaker_group_output_ids(
    const SpeakerGroup& group,
    const std::vector<OutputDevice>& outputs);

bool speaker_group_contains_persisted_output(
    const SpeakerGroup& group,
    const std::string& persisted_output_id,
    const std::vector<OutputDevice>& known_outputs);

bool speaker_group_matches_selection(
    const SpeakerGroup& group,
    const std::vector<OutputDevice>& outputs);

}  // namespace multiroom

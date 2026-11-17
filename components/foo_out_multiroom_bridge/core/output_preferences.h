#pragma once

#include "../transport.h"

#include <string>
#include <vector>

namespace multiroom {

struct SavedOutputPreferences {
    std::string output_id;
    bool selected = false;
    int volume = 50;
    bool visible_in_dropdown = true;
    int offset_ms = 0;
};

std::vector<SavedOutputPreferences> parse_output_preferences(const std::string& text);
std::string serialize_output_preferences(const std::vector<SavedOutputPreferences>& preferences);

void record_output_preferences(
    std::vector<SavedOutputPreferences>& preferences,
    const OutputDevice& output);

// Applies saved settings and reports whether any legacy aliases were consolidated.
bool restore_output_preferences(
    std::vector<SavedOutputPreferences>& preferences,
    std::vector<OutputDevice>& outputs);

bool has_airplay2_endpoint(const OutputDevice& output);
std::vector<std::string> selected_playback_ids(const std::vector<OutputDevice>& outputs);
bool has_undiscovered_selection(
    const std::vector<SavedOutputPreferences>& preferences,
    const std::vector<OutputDevice>& outputs);

}  // namespace multiroom

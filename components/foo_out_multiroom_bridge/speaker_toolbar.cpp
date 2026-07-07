#include "stdafx.h"
#include "multiroom_component_state.h"
#include "speaker_selector_popup.h"

#include <algorithm>

namespace {

static constexpr GUID guid_airplay_toolbar_dropdown = {
    0x7d6e76d4, 0x47b0, 0x4af9, {0x82, 0x57, 0x27, 0x2d, 0xb2, 0x36, 0x8b, 0x8e}};

static constexpr GUID guid_multiroom_menu_group = {
    0x038f6a58, 0x0b53, 0x4fbb, {0x8f, 0x3a, 0xe5, 0x64, 0xdd, 0x2f, 0xc8, 0x50}};

static constexpr GUID guid_open_speaker_selector = {
    0xd8af4c86, 0x63e6, 0x42ab, {0xa4, 0x66, 0xb6, 0xee, 0x7e, 0x89, 0x5d, 0x74}};

static constexpr GUID guid_toolbar_speaker_selector = {
    0xa9f2a3d3, 0x6392, 0x4828, {0x9d, 0xdb, 0xf0, 0x76, 0xdb, 0xf1, 0xe1, 0xbd}};

bool output_playable(const multiroom::OutputDevice& output) {
    return output.supports_airplay2 &&
           !output.endpoint_host.empty() &&
           output.endpoint_port != 0;
}

std::string output_name(const multiroom::OutputDevice& output) {
    if (!output.name.empty()) return output.name;
    if (!output.id.empty()) return output.id;
    return "AirPlay speaker";
}

std::string dropdown_label(const multiroom::OutputDevice& output) {
    std::string label = output.selected ? "[x] " : "[ ] ";
    label += output_name(output);
    if (!output_playable(output)) label += " (AirPlay 2 unavailable)";
    return label;
}

class AirPlaySpeakerToolbarDropdown : public fb2k::toolbarDropDown {
public:
    GUID getGuid() override {
        return guid_airplay_toolbar_dropdown;
    }

    void getShortName(pfc::string_base& out) override {
        out = "AirPlay";
    }

    void getLongName(pfc::string_base& out) override {
        out = "Universal Multiroom AirPlay speakers";
    }

    size_t getNumValues() override {
        const auto outputs = MultiroomComponentState::instance().outputs();
        return outputs.empty() ? 1 : outputs.size();
    }

    void getValue(size_t index, pfc::string_base& out) override {
        const auto outputs = MultiroomComponentState::instance().outputs();
        if (outputs.empty()) {
            out = "No AirPlay speakers";
            return;
        }
        if (index >= outputs.size()) {
            out = "";
            return;
        }

        out = dropdown_label(outputs[index]).c_str();
    }

    void setSelectedIndex(size_t index) override {
        auto outputs = MultiroomComponentState::instance().outputs();
        if (index >= outputs.size()) return;
        if (!output_playable(outputs[index])) return;

        MultiroomComponentState::instance().toggle_output(outputs[index].id);
        notify_content_changed();
        notify_selection_changed();
    }

    size_t getSelectedIndex() override {
        const auto outputs = MultiroomComponentState::instance().outputs();
        for (size_t index = 0; index < outputs.size(); ++index) {
            if (outputs[index].selected) return index;
        }
        return 0;
    }

    void addNotify(fb2k::toolbarDropDownNotify* notify) override {
        if (notify == nullptr) return;

        std::lock_guard lock(notify_mutex_);
        if (std::find(notifies_.begin(), notifies_.end(), notify) == notifies_.end()) {
            notifies_.push_back(notify);
        }
    }

    void removeNotify(fb2k::toolbarDropDownNotify* notify) override {
        std::lock_guard lock(notify_mutex_);
        notifies_.erase(std::remove(notifies_.begin(), notifies_.end(), notify), notifies_.end());
    }

    void onDropDown() override {
        MultiroomComponentState::instance().refresh_outputs();
        notify_content_changed();
        notify_selection_changed();
    }

private:
    std::vector<fb2k::toolbarDropDownNotify*> snapshot_notifies() {
        std::lock_guard lock(notify_mutex_);
        return notifies_;
    }

    void notify_content_changed() {
        for (auto* notify : snapshot_notifies()) {
            notify->contentChanged();
        }
    }

    void notify_selection_changed() {
        for (auto* notify : snapshot_notifies()) {
            notify->selectionChanged();
        }
    }

    std::mutex notify_mutex_;
    std::vector<fb2k::toolbarDropDownNotify*> notifies_;
};

static service_factory_single_t<AirPlaySpeakerToolbarDropdown> g_airplay_toolbar_dropdown_factory;

static mainmenu_group_popup_factory g_multiroom_menu_group(
    guid_multiroom_menu_group,
    mainmenu_groups::view,
    mainmenu_commands::sort_priority_dontcare,
    "Universal Multiroom");

class MultiroomMainMenuCommands : public mainmenu_commands {
public:
    enum {
        cmd_open_speaker_selector = 0,
        cmd_total
    };

    t_uint32 get_command_count() override {
        return cmd_total;
    }

    GUID get_command(t_uint32 index) override {
        switch (index) {
        case cmd_open_speaker_selector:
            return guid_open_speaker_selector;
        default:
            uBugCheck();
        }
    }

    void get_name(t_uint32 index, pfc::string_base& out) override {
        switch (index) {
        case cmd_open_speaker_selector:
            out = "Speakers...";
            break;
        default:
            uBugCheck();
        }
    }

    bool get_description(t_uint32 index, pfc::string_base& out) override {
        switch (index) {
        case cmd_open_speaker_selector:
            out = "Opens the Universal Multiroom AirPlay speaker selector.";
            return true;
        default:
            return false;
        }
    }

    GUID get_parent() override {
        return guid_multiroom_menu_group;
    }

    void execute(t_uint32 index, service_ptr_t<service_base>) override {
        switch (index) {
        case cmd_open_speaker_selector:
            MultiroomComponentState::instance().refresh_outputs();
            show_multiroom_speaker_picker(core_api::get_main_window(), nullptr);
            break;
        default:
            uBugCheck();
        }
    }
};

static mainmenu_commands_factory_t<MultiroomMainMenuCommands> g_multiroom_mainmenu_commands_factory;

class MultiroomPlaybackMenuCommands : public mainmenu_commands {
public:
    enum {
        cmd_airplay_speakers = 0,
        cmd_total
    };

    t_uint32 get_command_count() override {
        return cmd_total;
    }

    GUID get_command(t_uint32 index) override {
        switch (index) {
        case cmd_airplay_speakers:
            return guid_toolbar_speaker_selector;
        default:
            uBugCheck();
        }
    }

    void get_name(t_uint32 index, pfc::string_base& out) override {
        switch (index) {
        case cmd_airplay_speakers:
            out = "AirPlay Speakers...";
            break;
        default:
            uBugCheck();
        }
    }

    bool get_description(t_uint32 index, pfc::string_base& out) override {
        switch (index) {
        case cmd_airplay_speakers:
            out = "Opens the Universal Multiroom AirPlay speaker picker.";
            return true;
        default:
            return false;
        }
    }

    GUID get_parent() override {
        return mainmenu_groups::playback_controls;
    }

    t_uint32 get_sort_priority() override {
        return mainmenu_commands::sort_priority_base + 900;
    }

    void execute(t_uint32 index, service_ptr_t<service_base>) override {
        switch (index) {
        case cmd_airplay_speakers:
            MultiroomComponentState::instance().refresh_outputs();
            show_multiroom_speaker_picker(core_api::get_main_window(), nullptr);
            break;
        default:
            uBugCheck();
        }
    }
};

static mainmenu_commands_factory_t<MultiroomPlaybackMenuCommands> g_multiroom_playback_commands_factory;

}  // namespace

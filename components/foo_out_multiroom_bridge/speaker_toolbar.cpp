#include "stdafx.h"
#include "multiroom_component_state.h"
#include "speaker_selector_popup.h"
#include "speaker_toolbar.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace {

static constexpr GUID guid_airplay_speaker_selector_command = {
    0xa9f2a3d3, 0x6392, 0x4828, {0x9d, 0xdb, 0xf0, 0x76, 0xdb, 0xf1, 0xe1, 0xbd}};
static constexpr GUID guid_airplay_speaker_toolbar_dropdown = {
    0xb3947304, 0xaf38, 0x4dcd, {0x9a, 0x4a, 0x7e, 0xeb, 0xc9, 0x66, 0xf8, 0xfa}};

std::mutex g_toolbar_notify_mutex;
std::vector<fb2k::toolbarDropDownNotify*> g_toolbar_notifications;

bool output_playable(const multiroom::OutputDevice& output) {
    return output.supports_airplay2 && !output.endpoint_host.empty() && output.endpoint_port != 0;
}

std::vector<multiroom::OutputDevice> playable_outputs() {
    auto outputs = MultiroomComponentState::instance().outputs();
    outputs.erase(
        std::remove_if(outputs.begin(), outputs.end(), [](const auto& output) {
            return !output_playable(output);
        }),
        outputs.end());
    return outputs;
}

std::string selected_summary(const std::vector<multiroom::OutputDevice>& outputs) {
    std::string first;
    size_t count = 0;
    for (const auto& output : outputs) {
        if (!output.selected) continue;
        if (first.empty()) first = output.name.empty() ? output.id : output.name;
        ++count;
    }
    if (count == 0) return "AirPlay - no speakers";
    if (count == 1) return first;
    return first + " +" + std::to_string(count - 1);
}

void notify_toolbar_on_main_thread() {
    std::vector<fb2k::toolbarDropDownNotify*> notifications;
    {
        std::lock_guard lock(g_toolbar_notify_mutex);
        notifications = g_toolbar_notifications;
    }
    for (auto* notification : notifications) {
        if (notification == nullptr) continue;
        notification->contentChanged();
        notification->selectionChanged();
    }
}

class MultiroomSpeakerToolbarDropdown : public fb2k::toolbarDropDown {
public:
    GUID getGuid() override {
        return guid_airplay_speaker_toolbar_dropdown;
    }

    void getShortName(pfc::string_base& out) override {
        out = "AirPlay";
    }

    void getLongName(pfc::string_base& out) override {
        out = "Universal Multiroom AirPlay speaker selector";
    }

    size_t getNumValues() override {
        return playable_outputs().size() + 2;
    }

    void getValue(size_t index, pfc::string_base& out) override {
        const auto outputs = playable_outputs();
        if (index == 0) {
            out = selected_summary(outputs).c_str();
            return;
        }
        if (index == outputs.size() + 1) {
            out = "Open speaker picker...";
            return;
        }

        const auto output_index = index - 1;
        if (output_index >= outputs.size()) {
            out = "Open speaker picker...";
            return;
        }
        const auto& output = outputs[output_index];
        const auto name = output.name.empty() ? output.id : output.name;
        out = ((output.selected ? "[x] " : "[ ] ") + name).c_str();
    }

    void setSelectedIndex(size_t index) override {
        if (index == 0) return;
        const auto outputs = playable_outputs();
        if (index == outputs.size() + 1) {
            show_multiroom_speaker_picker(core_api::get_main_window(), nullptr);
            return;
        }

        const auto output_index = index - 1;
        if (output_index >= outputs.size()) return;
        MultiroomComponentState::instance().toggle_output(outputs[output_index].id);
        notify_multiroom_speaker_toolbar_changed();
    }

    size_t getSelectedIndex() override {
        return 0;
    }

    void addNotify(fb2k::toolbarDropDownNotify* notification) override {
        if (notification == nullptr) return;
        std::lock_guard lock(g_toolbar_notify_mutex);
        if (std::find(g_toolbar_notifications.begin(), g_toolbar_notifications.end(), notification) ==
            g_toolbar_notifications.end()) {
            g_toolbar_notifications.push_back(notification);
        }
    }

    void removeNotify(fb2k::toolbarDropDownNotify* notification) override {
        std::lock_guard lock(g_toolbar_notify_mutex);
        g_toolbar_notifications.erase(
            std::remove(g_toolbar_notifications.begin(), g_toolbar_notifications.end(), notification),
            g_toolbar_notifications.end());
    }

    void onDropDown() override {
        if (playable_outputs().empty()) {
            MultiroomComponentState::instance().refresh_outputs();
        }
        notify_toolbar_on_main_thread();
    }
};

static service_factory_single_t<MultiroomSpeakerToolbarDropdown> g_multiroom_speaker_toolbar_dropdown_factory;

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
            return guid_airplay_speaker_selector_command;
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
            out = "Opens the Universal Multiroom AirPlay speaker picker; add this command to a toolbar for a compact speaker button.";
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
            show_multiroom_speaker_picker(core_api::get_main_window(), nullptr);
            break;
        default:
            uBugCheck();
        }
    }
};

static mainmenu_commands_factory_t<MultiroomPlaybackMenuCommands> g_multiroom_playback_commands_factory;

}  // namespace

void notify_multiroom_speaker_toolbar_changed() {
    if (!core_api::are_services_available() || core_api::is_shutting_down()) return;
    if (core_api::is_main_thread()) {
        notify_toolbar_on_main_thread();
    } else {
        fb2k::inMainThread([] {
            notify_toolbar_on_main_thread();
        });
    }
}

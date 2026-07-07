#include "stdafx.h"
#include "multiroom_component_state.h"
#include "speaker_selector_popup.h"

namespace {

static constexpr GUID guid_airplay_speaker_selector_command = {
    0xa9f2a3d3, 0x6392, 0x4828, {0x9d, 0xdb, 0xf0, 0x76, 0xdb, 0xf1, 0xe1, 0xbd}};

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

#include "stdafx.h"
#include "multiroom_component_state.h"
#include "core/volume_control.h"
#include "speaker_toolbar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

static constexpr GUID guid_cfg_airplay_pairing_credentials = {
    0xf9fecc9d, 0x639a, 0x45ad, {0x8a, 0x11, 0x4b, 0x55, 0x5f, 0x6f, 0x50, 0xa4}};
static constexpr GUID guid_cfg_airplay_output_state = {
    0x5e565c70, 0x8175, 0x4c61, {0xb4, 0x22, 0x99, 0x8d, 0x4f, 0x7c, 0x65, 0x19}};
static constexpr GUID guid_cfg_speaker_groups = {
    0x472924a6, 0x5daa, 0x47d8, {0xa0, 0x45, 0xda, 0xb3, 0xaf, 0xe9, 0x5c, 0x41}};

static cfg_string cfg_airplay_pairing_credentials(guid_cfg_airplay_pairing_credentials, "");
static cfg_string cfg_airplay_output_state(guid_cfg_airplay_output_state, "");
static cfg_string cfg_speaker_groups(guid_cfg_speaker_groups, "");
static std::mutex cfg_airplay_output_state_mutex;
static std::mutex cfg_speaker_groups_mutex;

struct StoredOutputState {
    std::string output_id;
    bool selected = false;
    int volume = 50;
    bool visible_in_dropdown = true;
};

std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (required <= 1) return {};

    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), required);
    result.resize(static_cast<size_t>(required - 1));
    return result;
}

std::string hex_from_bytes(const std::vector<uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(kHex[(byte >> 4) & 0x0F]);
        result.push_back(kHex[byte & 0x0F]);
    }
    return result;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

std::vector<uint8_t> bytes_from_hex(const std::string& text) {
    if ((text.size() % 2) != 0) {
        return {};
    }

    std::vector<uint8_t> result;
    result.reserve(text.size() / 2);
    for (size_t index = 0; index < text.size(); index += 2) {
        const int high = hex_value(text[index]);
        const int low = hex_value(text[index + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        result.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return result;
}

std::vector<std::string> split_pipe_fields(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, '|')) {
        fields.push_back(field);
    }
    return fields;
}

bool parse_int_field(const std::string& text, int& out) {
    if (text.empty()) {
        return false;
    }

    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }

    out = static_cast<int>(value);
    return true;
}

std::vector<StoredOutputState> parse_output_state_text(const std::string& text) {
    std::vector<StoredOutputState> result;
    std::stringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto fields = split_pipe_fields(line);
        // The fourth field was added after the original selected/volume
        // format. Existing three-field settings migrate as visible.
        if ((fields.size() != 3 && fields.size() != 4) || fields[0].empty()) {
            continue;
        }

        int volume = 50;
        if (!parse_int_field(fields[2], volume)) {
            continue;
        }

        StoredOutputState item;
        item.output_id = fields[0];
        item.selected = fields[1] == "1";
        item.volume = std::clamp(volume, 0, 100);
        item.visible_in_dropdown = fields.size() < 4 || fields[3] != "0";
        result.push_back(std::move(item));
    }
    return result;
}

std::string format_output_state_text(const std::vector<StoredOutputState>& states) {
    std::ostringstream out;
    for (const auto& state : states) {
        if (state.output_id.empty()) {
            continue;
        }
        out << state.output_id << '|'
            << (state.selected ? '1' : '0') << '|'
            << std::clamp(state.volume, 0, 100) << '|'
            << (state.visible_in_dropdown ? '1' : '0') << '\n';
    }
    return out.str();
}

std::vector<StoredOutputState> load_output_state() {
    std::lock_guard lock(cfg_airplay_output_state_mutex);
    return parse_output_state_text(cfg_airplay_output_state.get().c_str());
}

void update_stored_output_state(const multiroom::OutputDevice& output) {
    // Selection, volume, and dropdown visibility can be updated by different
    // UI/worker threads. Keep the read-modify-write transaction under one lock
    // so one preference cannot overwrite another with an older snapshot.
    std::lock_guard lock(cfg_airplay_output_state_mutex);
    auto states = parse_output_state_text(cfg_airplay_output_state.get().c_str());
    states.erase(
        std::remove_if(states.begin(), states.end(), [&](const auto& state) {
            return std::find(output.aliases.begin(), output.aliases.end(), state.output_id) != output.aliases.end();
        }),
        states.end());
    const auto it = std::find_if(states.begin(), states.end(), [&](const auto& state) {
        return state.output_id == output.id;
    });

    StoredOutputState updated;
    updated.output_id = output.id;
    updated.selected = output.selected;
    updated.volume = std::clamp(output.volume, 0, 100);
    updated.visible_in_dropdown = output.visible_in_dropdown;

    if (it == states.end()) {
        states.push_back(std::move(updated));
    } else {
        *it = std::move(updated);
    }

    const auto serialized = format_output_state_text(states);
    cfg_airplay_output_state = serialized.c_str();
}

void apply_stored_output_state(std::vector<multiroom::OutputDevice>& outputs) {
    const auto states = load_output_state();
    for (auto& output : outputs) {
        const auto it = std::find_if(states.begin(), states.end(), [&](const auto& state) {
            return state.output_id == output.id ||
                   std::find(output.aliases.begin(), output.aliases.end(), state.output_id) != output.aliases.end();
        });
        if (it == states.end()) {
            continue;
        }
        output.selected = it->selected;
        output.volume = it->volume;
        output.visible_in_dropdown = it->visible_in_dropdown;
        if (it->output_id != output.id) {
            update_stored_output_state(output);
        }
    }
}

bool has_stored_selected_output() {
    const auto states = load_output_state();
    return std::any_of(states.begin(), states.end(), [](const auto& state) {
        return state.selected;
    });
}

void replace_stored_output_selection(
    const multiroom::SpeakerGroup& group,
    const std::vector<multiroom::OutputDevice>& known_outputs) {
    std::lock_guard lock(cfg_airplay_output_state_mutex);
    auto states = parse_output_state_text(cfg_airplay_output_state.get().c_str());
    for (auto& state : states) {
        state.selected = multiroom::speaker_group_contains_persisted_output(
            group, state.output_id, known_outputs);
    }
    for (const auto& group_id : group.output_ids) {
        const bool represented = std::any_of(states.begin(), states.end(), [&](const auto& state) {
            return multiroom::speaker_group_contains_persisted_output(
                group, state.output_id, known_outputs) &&
                (state.output_id == group_id || std::any_of(known_outputs.begin(), known_outputs.end(), [&](const auto& output) {
                    const bool state_matches = output.id == state.output_id ||
                        std::find(output.aliases.begin(), output.aliases.end(), state.output_id) != output.aliases.end();
                    const bool group_matches = output.id == group_id ||
                        std::find(output.aliases.begin(), output.aliases.end(), group_id) != output.aliases.end();
                    return state_matches && group_matches;
                }));
        });
        if (!represented) states.push_back({group_id, true, 50, true});
    }
    const auto serialized = format_output_state_text(states);
    cfg_airplay_output_state = serialized.c_str();
}

std::vector<multiroom::SpeakerGroup> load_speaker_groups() {
    std::lock_guard lock(cfg_speaker_groups_mutex);
    return multiroom::deserialize_speaker_groups(cfg_speaker_groups.get().c_str());
}

std::string new_speaker_group_id() {
    GUID guid = {};
    if (FAILED(::CoCreateGuid(&guid))) {
        throw std::runtime_error("Could not create a speaker group identifier.");
    }
    char text[37] = {};
    std::snprintf(
        text,
        sizeof(text),
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        static_cast<unsigned long>(guid.Data1),
        guid.Data2,
        guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}

std::vector<multiroom::airplay::AirPlayPairingCredentials> parse_pairing_credentials_text(const std::string& text) {
    std::vector<multiroom::airplay::AirPlayPairingCredentials> result;
    std::stringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto fields = split_pipe_fields(line);
        if (fields.size() != 5) {
            continue;
        }

        multiroom::airplay::AirPlayPairingCredentials item;
        item.output_id = fields[0];
        item.client_id = fields[1];
        item.controller_seed = bytes_from_hex(fields[2]);
        item.accessory_identifier = bytes_from_hex(fields[3]);
        item.accessory_public_key = bytes_from_hex(fields[4]);
        if (item.valid()) {
            result.push_back(std::move(item));
        }
    }
    return result;
}

std::string format_pairing_credentials_text(
    const std::vector<multiroom::airplay::AirPlayPairingCredentials>& credentials) {
    std::ostringstream out;
    for (const auto& item : credentials) {
        if (!item.valid()) {
            continue;
        }
        out << item.output_id << '|'
            << item.client_id << '|'
            << hex_from_bytes(item.controller_seed) << '|'
            << hex_from_bytes(item.accessory_identifier) << '|'
            << hex_from_bytes(item.accessory_public_key) << '\n';
    }
    return out.str();
}

std::vector<std::string> selected_ids(const std::vector<multiroom::OutputDevice>& outputs) {
    std::vector<std::string> result;
    for (const auto& output : outputs) {
        if (output.selected) result.push_back(output.id);
    }
    return result;
}

int effective_remote_volume(int speaker_volume, int master_volume_percent) {
    return multiroom::effective_output_volume_percent(master_volume_percent, speaker_volume);
}

void preserve_user_output_state(
    std::vector<multiroom::OutputDevice>& refreshed,
    const std::vector<multiroom::OutputDevice>& previous) {
    for (auto& output : refreshed) {
        const auto it = std::find_if(previous.begin(), previous.end(), [&](const auto& candidate) {
            return candidate.id == output.id ||
                   std::find(output.aliases.begin(), output.aliases.end(), candidate.id) != output.aliases.end();
        });
        if (it == previous.end()) {
            continue;
        }
        output.selected = it->selected;
        output.volume = it->volume;
        output.offset_ms = it->offset_ms;
        output.measured_latency_ms = it->measured_latency_ms;
        output.visible_in_dropdown = it->visible_in_dropdown;
    }
}

void validate_playback_format(const multiroom::PcmFormat& format) {
    if (format.sample_rate == 0 || format.channels == 0 || format.bits_per_sample == 0) {
        throw std::invalid_argument("PCM format must have non-zero sample rate, channels, and bit depth.");
    }
    if (format.bits_per_sample % 8 != 0) {
        throw std::invalid_argument("PCM bit depth must be byte aligned.");
    }
}

const char* remote_command_name(multiroom::airplay::AirPlayRemoteCommand command) {
    using multiroom::airplay::AirPlayRemoteCommand;
    switch (command) {
    case AirPlayRemoteCommand::Play: return "play";
    case AirPlayRemoteCommand::Pause: return "pause";
    case AirPlayRemoteCommand::TogglePlayPause: return "toggle-play-pause";
    case AirPlayRemoteCommand::Stop: return "stop";
    case AirPlayRemoteCommand::NextTrack: return "next";
    case AirPlayRemoteCommand::PreviousTrack: return "previous";
    }
    return "unknown";
}

class FoobarAirPlayPairingStore final : public multiroom::airplay::AirPlayPairingStore {
public:
    std::optional<multiroom::airplay::AirPlayPairingCredentials> load(const std::string& output_id) override {
        std::lock_guard lock(mutex_);
        const auto text = cfg_airplay_pairing_credentials.get();
        for (auto& item : parse_pairing_credentials_text(text.c_str())) {
            if (item.output_id == output_id) {
                return item;
            }
        }
        return std::nullopt;
    }

    void save(const multiroom::airplay::AirPlayPairingCredentials& credentials) override {
        if (!credentials.valid()) {
            throw std::invalid_argument("Cannot save incomplete AirPlay pairing credentials.");
        }

        std::lock_guard lock(mutex_);
        auto all = parse_pairing_credentials_text(cfg_airplay_pairing_credentials.get().c_str());
        bool replaced = false;
        for (auto& item : all) {
            if (item.output_id == credentials.output_id) {
                item = credentials;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            all.push_back(credentials);
        }

        const auto serialized = format_pairing_credentials_text(all);
        cfg_airplay_pairing_credentials = serialized.c_str();
    }

private:
    std::mutex mutex_;
};

}  // namespace

MultiroomComponentState& MultiroomComponentState::instance() {
    static MultiroomComponentState state;
    return state;
}

MultiroomComponentState::MultiroomComponentState()
    : transport_(multiroom::airplay::make_airplay_rtsp_control_client(std::make_shared<FoobarAirPlayPairingStore>())) {
    transport_.set_remote_command_handler([this](const auto& output_id, const auto& event) {
        handle_remote_command(output_id, event);
    });
}

MultiroomComponentState::~MultiroomComponentState() {
    shutting_down_.store(true);
    transport_.set_remote_command_handler({});
    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }
    if (control_thread_.joinable()) {
        control_thread_.join();
    }
    if (pairing_thread_.joinable()) {
        pairing_thread_.join();
    }
}

void MultiroomComponentState::handle_remote_command(
    const std::string& output_id,
    const multiroom::airplay::AirPlayRemoteCommandEvent& event) {
    if (shutting_down_.load()) {
        return;
    }

    if (!event.command_id.empty()) {
        std::lock_guard lock(mutex_);
        if (std::find(
                recent_remote_command_ids_.begin(),
                recent_remote_command_ids_.end(),
                event.command_id) != recent_remote_command_ids_.end()) {
            return;
        }
        recent_remote_command_ids_.push_back(event.command_id);
        if (recent_remote_command_ids_.size() > 64) {
            recent_remote_command_ids_.pop_front();
        }
    }

    FB2K_console_formatter() << "[Universal Multiroom] AirPlay remote command from "
                              << output_id.c_str() << ": "
                              << remote_command_name(event.command);

    const auto command = event.command;
    fb2k::inMainThread([this, command] {
        if (shutting_down_.load()) {
            return;
        }

        try {
            auto control = playback_control::get();
            using multiroom::airplay::AirPlayRemoteCommand;
            switch (command) {
            case AirPlayRemoteCommand::Play:
                if (control->is_playing()) control->pause(false);
                else control->start();
                break;
            case AirPlayRemoteCommand::Pause:
                if (control->is_playing()) control->pause(true);
                break;
            case AirPlayRemoteCommand::TogglePlayPause:
                control->play_or_pause();
                break;
            case AirPlayRemoteCommand::Stop:
                if (control->is_playing()) control->stop();
                break;
            case AirPlayRemoteCommand::NextTrack:
                control->start(playback_control::track_command_next);
                break;
            case AirPlayRemoteCommand::PreviousTrack:
                control->start(playback_control::track_command_prev);
                break;
            }
        } catch (const std::exception& e) {
            FB2K_console_formatter() << "[Universal Multiroom] Could not apply AirPlay remote command: "
                                      << e.what();
        }
    });
}

void MultiroomComponentState::ensure_discovery_started() {
    std::lock_guard transport_lock(transport_mutex_);
    {
        std::lock_guard lock(mutex_);
        if (discovery_started_) return;
    }

    transport_.start_discovery();
    {
        std::lock_guard lock(mutex_);
        discovery_started_ = true;
    }
}

void MultiroomComponentState::refresh_outputs() {
    {
        std::lock_guard lock(mutex_);
        // Discovery refresh takes the transport mutex and mutates the registry.
        // Never let a UI refresh contend with PCM delivery or reconcile a
        // transient mDNS snapshot into an active session. Queue it until Stop.
        if (playback_open_.load() || playback_connecting_) {
            refresh_deferred_until_stop_ = true;
            return;
        }
        if (refresh_in_progress_) {
            refresh_requested_ = true;
            return;
        }
        refresh_in_progress_ = true;
        refresh_requested_ = false;
    }

    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }
    refresh_thread_ = std::thread(&MultiroomComponentState::refresh_outputs_worker, this);
}

void MultiroomComponentState::refresh_outputs_worker() {
    for (;;) {
        std::vector<multiroom::OutputDevice> refreshed_outputs;
        std::wstring refresh_error;
        std::string refresh_error_narrow;
        bool refresh_ok = false;

        try {
            ensure_discovery_started();
            std::lock_guard transport_lock(transport_mutex_);
            {
                std::lock_guard lock(mutex_);
                // Recheck after acquiring the transport lock. Playback may
                // have started after refresh_outputs() accepted this worker,
                // or between two coalesced refresh iterations. If discovery
                // owns the lock first, setup waits; if setup owns it first,
                // discovery observes Connecting/Open here and is deferred.
                if (playback_open_.load() || playback_connecting_) {
                    refresh_deferred_until_stop_ = true;
                    refresh_requested_ = false;
                    refresh_in_progress_ = false;
                    return;
                }
            }
            transport_.refresh_discovery();
            refreshed_outputs = transport_.list_outputs();
            apply_stored_output_state(refreshed_outputs);
            refresh_ok = true;
        } catch (const std::exception& e) {
            refresh_error_narrow = e.what();
            refresh_error = widen_utf8(e.what());
        }

        size_t refresh_number = 0;
        {
            std::lock_guard lock(mutex_);
            ++refresh_count_;
            refresh_number = refresh_count_;
            if (refresh_ok) {
                cached_outputs_ = refreshed_outputs;
                last_error_.clear();
            } else {
                last_error_ = std::move(refresh_error);
            }
        }

        if (refresh_ok) {
            notify_multiroom_speaker_toolbar_changed();
            FB2K_console_formatter() << "[Universal Multiroom] AirPlay discovery refresh #" << refresh_number
                                      << ": " << refreshed_outputs.size() << " device(s)";
            for (const auto& output : refreshed_outputs) {
                FB2K_console_formatter() << "[Universal Multiroom] AirPlay device: "
                                          << output.name.c_str()
                                          << " id=" << output.id.c_str()
                                          << " endpoint=" << output.endpoint_host.c_str()
                                          << ":" << output.endpoint_port
                                          << " airplay2=" << (output.supports_airplay2 ? "yes" : "no")
                                          << " legacy=" << (output.supports_legacy_l16 ? "yes" : "no")
                                          << " auth=" << (output.requires_auth ? "yes" : "no")
                                          << " format=" << output.format.c_str();
            }
        } else {
            FB2K_console_formatter() << "[Universal Multiroom] AirPlay discovery failed: " << refresh_error_narrow.c_str();
        }

        bool run_again = false;
        {
            std::lock_guard lock(mutex_);
            if (refresh_requested_) {
                refresh_requested_ = false;
                run_again = true;
            } else {
                refresh_in_progress_ = false;
            }
        }

        if (!run_again) {
            return;
        }
    }
}

bool MultiroomComponentState::playback_active() {
    std::lock_guard lock(mutex_);
    return playback_open_.load() || playback_connecting_;
}

void MultiroomComponentState::refresh_outputs_for_playback() {
    bool should_refresh = false;
    {
        std::lock_guard lock(mutex_);
        const auto selected = selected_ids(cached_outputs_);
        should_refresh = cached_outputs_.empty() || (selected.empty() && has_stored_selected_output());
    }

    if (!should_refresh) {
        return;
    }

    std::vector<multiroom::OutputDevice> refreshed_outputs;
    const bool saved_selection_expected = has_stored_selected_output();
    {
        std::lock_guard transport_lock(transport_mutex_);
        for (size_t attempt = 0; attempt < 2; ++attempt) {
            for (size_t slice = 0; slice < 10; ++slice) {
                if (playback_open_cancel_requested_.load()) {
                    throw std::runtime_error("AirPlay playback setup cancelled.");
                }
                transport_.refresh_discovery(std::chrono::milliseconds(250));
                refreshed_outputs = transport_.list_outputs();
                apply_stored_output_state(refreshed_outputs);
                if (!saved_selection_expected || !selected_ids(refreshed_outputs).empty()) {
                    break;
                }
            }
            if (!saved_selection_expected || !selected_ids(refreshed_outputs).empty()) {
                break;
            }
        }
    }

    size_t refresh_number = 0;
    {
        std::lock_guard lock(mutex_);
        ++refresh_count_;
        refresh_number = refresh_count_;
        cached_outputs_ = std::move(refreshed_outputs);
        last_error_.clear();
    }

    FB2K_console_formatter() << "[Universal Multiroom] AirPlay playback discovery refresh #"
                              << refresh_number << " applied saved speaker state";
}

void MultiroomComponentState::schedule_control_update() {
    {
        std::lock_guard lock(mutex_);
        control_full_update_requested_ = true;
        if (control_in_progress_) {
            return;
        }
        control_in_progress_ = true;
    }

    if (control_thread_.joinable()) {
        control_thread_.join();
    }
    control_thread_ = std::thread(&MultiroomComponentState::control_update_worker, this);
}

void MultiroomComponentState::schedule_volume_update(const std::vector<std::string>& ids) {
    if (ids.empty()) return;

    {
        std::lock_guard lock(mutex_);
        control_volume_update_ids_.insert(ids.begin(), ids.end());
        if (control_in_progress_) {
            return;
        }
        control_in_progress_ = true;
    }

    if (control_thread_.joinable()) {
        control_thread_.join();
    }
    control_thread_ = std::thread(&MultiroomComponentState::control_update_worker, this);
}

void MultiroomComponentState::schedule_metadata_update() {
    {
        std::lock_guard lock(mutex_);
        control_metadata_update_requested_ = true;
        if (control_in_progress_) {
            return;
        }
        control_in_progress_ = true;
    }

    if (control_thread_.joinable()) {
        control_thread_.join();
    }
    control_thread_ = std::thread(&MultiroomComponentState::control_update_worker, this);
}

void MultiroomComponentState::control_update_worker() {
    for (;;) {
        // A trackbar can produce dozens of notifications per second. Give those
        // notifications a short window to collapse into one RTSP update instead
        // of blocking the PCM writer once for every intermediate thumb position.
        bool debounce_volume_updates = false;
        {
            std::lock_guard lock(mutex_);
            debounce_volume_updates =
                !control_full_update_requested_ && !control_volume_update_ids_.empty();
        }
        if (debounce_volume_updates) {
            // Do not hold either state or transport locks during the debounce.
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }

        std::vector<multiroom::OutputDevice> outputs_snapshot;
        std::set<std::string> volume_update_ids;
        multiroom::PlaybackMetadata metadata_snapshot;
        multiroom::PcmFormat playback_format_snapshot{};
        int master_volume_snapshot = 100;
        bool should_connect = false;
        bool full_update = false;
        bool metadata_active = false;
        bool metadata_update_requested = false;
        {
            std::lock_guard lock(mutex_);
            outputs_snapshot = cached_outputs_;
            master_volume_snapshot = master_volume_percent_;
            metadata_snapshot = playback_metadata_;
            metadata_active = playback_metadata_active_;
            metadata_update_requested = control_metadata_update_requested_;
            control_metadata_update_requested_ = false;
            full_update = control_full_update_requested_;
            control_full_update_requested_ = false;
            volume_update_ids.swap(control_volume_update_ids_);
            should_connect = playback_open_.load() && playback_format_valid_;
            if (should_connect) {
                playback_format_snapshot = playback_format_;
            }
        }

        std::wstring control_error;
        std::string control_error_narrow;
        bool control_ok = false;

        try {
            if (!full_update) {
                // Receiver volume is a control-plane operation. It must not own
                // the PCM transport lock while waiting for an RTSP round trip.
                for (const auto& output : outputs_snapshot) {
                    if (volume_update_ids.find(output.id) == volume_update_ids.end()) continue;
                    transport_.set_session_volume(
                        output.id,
                        effective_remote_volume(output.volume, master_volume_snapshot));
                }
            }
            if (full_update || metadata_update_requested) {
                ensure_discovery_started();
                std::lock_guard transport_lock(transport_mutex_);
                // Stop may have completed after the snapshot was taken while this
                // worker waited for the transport. Never reopen a receiver for a
                // stream which foobar has already closed.
                should_connect = should_connect && playback_open_.load();
                if (full_update || metadata_update_requested) {
                    if (metadata_active) {
                        transport_.set_playback_metadata(metadata_snapshot);
                    } else {
                        transport_.clear_playback_metadata();
                    }
                }
                if (full_update) {
                    const auto selected = selected_ids(outputs_snapshot);
                    transport_.set_enabled_outputs(selected);
                    for (const auto& output : outputs_snapshot) {
                        transport_.set_output_volume(output.id, effective_remote_volume(output.volume, master_volume_snapshot));
                    }
                    const auto previous_outputs = outputs_snapshot;
                    if (should_connect) {
                        if (!playback_engine_) {
                            playback_engine_ = std::make_unique<multiroom::MultiroomEngine>(transport_);
                        }
                        if (!playback_engine_->stream_open()) {
                            playback_engine_->open_stream(playback_format_snapshot);
                        }
                        transport_.connect_selected_outputs();
                    }
                    outputs_snapshot = transport_.list_outputs();
                    preserve_user_output_state(outputs_snapshot, previous_outputs);
                }
            }
            control_ok = true;
        } catch (const std::exception& e) {
            control_error_narrow = e.what();
            control_error = widen_utf8(e.what());
        }

        std::vector<std::wstring> active_output_names;
        if (control_ok && should_connect && full_update) {
            std::lock_guard transport_lock(transport_mutex_);
            for (const auto& session : transport_.sessions()) {
                if (session.open && session.phase == multiroom::airplay::AirPlaySessionPhase::Ready) {
                    active_output_names.push_back(widen_utf8(
                        session.output_name.empty() ? session.output_id : session.output_name));
                }
            }
        }

        bool destination_label_changed = false;
        {
            std::lock_guard lock(mutex_);
            if (control_ok) {
                if (full_update) {
                    preserve_user_output_state(outputs_snapshot, cached_outputs_);
                    cached_outputs_ = outputs_snapshot;
                    if (should_connect) {
                        active_output_names_ = std::move(active_output_names);
                        destination_label_changed = true;
                    }
                }
                last_error_.clear();
            } else {
                // set_enabled_outputs() closes sessions which are no longer
                // selected before a replacement is opened. Never keep
                // advertising those old sessions when the replacement fails.
                if (full_update && should_connect) {
                    active_output_names_.clear();
                    destination_label_changed = true;
                }
                last_error_ = std::move(control_error);
            }
        }

        if (control_ok) {
            notify_multiroom_speaker_toolbar_changed();
            FB2K_console_formatter() << "[Universal Multiroom] Speaker control update applied";
        } else {
            if (destination_label_changed) {
                notify_multiroom_speaker_toolbar_changed();
            }
            FB2K_console_formatter() << "[Universal Multiroom] Speaker control update failed: "
                                      << control_error_narrow.c_str();
        }

        bool run_again = false;
        {
            std::lock_guard lock(mutex_);
            run_again = control_full_update_requested_ ||
                !control_volume_update_ids_.empty() ||
                control_metadata_update_requested_;
            if (!run_again) {
                control_in_progress_ = false;
            }
        }

        if (!run_again) {
            return;
        }
    }
}

bool MultiroomComponentState::refresh_in_progress() {
    std::lock_guard lock(mutex_);
    // UI polling must remain alive while an explicit refresh is queued behind
    // active playback, then follow the worker through its eventual scan.
    return refresh_in_progress_ || refresh_deferred_until_stop_;
}

bool MultiroomComponentState::control_in_progress() {
    std::lock_guard lock(mutex_);
    return control_in_progress_;
}

bool MultiroomComponentState::pairing_in_progress() {
    std::lock_guard lock(mutex_);
    return pairing_in_progress_;
}

std::vector<multiroom::OutputDevice> MultiroomComponentState::outputs() {
    std::lock_guard lock(mutex_);
    return cached_outputs_;
}

void MultiroomComponentState::pair_output(const std::string& id, const std::string& pin) {
    try {
        if (pin.empty()) {
            throw std::invalid_argument("AirPlay PIN cannot be empty.");
        }

        {
            std::lock_guard lock(mutex_);
            if (pairing_in_progress_) {
                throw std::runtime_error("AirPlay pairing is already in progress.");
            }
            pairing_in_progress_ = true;
            last_error_.clear();
        }

        if (pairing_thread_.joinable()) {
            pairing_thread_.join();
        }
        pairing_thread_ = std::thread(&MultiroomComponentState::pairing_worker, this, id, pin);
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        pairing_in_progress_ = false;
        last_error_ = widen_utf8(e.what());
    }
}

void MultiroomComponentState::pairing_worker(std::string id, std::string pin) {
    std::wstring pairing_error;
    std::string pairing_error_narrow;
    bool pairing_ok = false;

    try {
        ensure_discovery_started();
        std::lock_guard transport_lock(transport_mutex_);
        static_cast<void>(transport_.pair_output(id, pin));
        pairing_ok = true;
    } catch (const std::exception& e) {
        pairing_error_narrow = e.what();
        pairing_error = widen_utf8(e.what());
    }

    {
        std::lock_guard lock(mutex_);
        pairing_in_progress_ = false;
        if (pairing_ok) {
            last_error_.clear();
        } else {
            last_error_ = std::move(pairing_error);
        }
    }

    if (pairing_ok) {
        FB2K_console_formatter() << "[Universal Multiroom] AirPlay PIN pairing completed";
        if (playback_open_.load()) {
            schedule_control_update();
        }
    } else {
        FB2K_console_formatter() << "[Universal Multiroom] AirPlay PIN pairing failed: "
                                  << pairing_error_narrow.c_str();
    }
}

void MultiroomComponentState::toggle_output(const std::string& id) {
    try {
        multiroom::OutputDevice updated_output;
        bool changed = false;
        {
            std::lock_guard lock(mutex_);
            const auto output_it = std::find_if(cached_outputs_.begin(), cached_outputs_.end(), [&](const auto& output) {
                return output.id == id;
            });
            if (output_it != cached_outputs_.end() && !output_it->supports_airplay2) {
                throw std::runtime_error("AirPlay 2 is required for the multiroom MVP: " + id);
            }
            if (output_it != cached_outputs_.end()) {
                output_it->selected = !output_it->selected;
                updated_output = *output_it;
                changed = true;
            }
            last_error_.clear();
        }
        if (changed) {
            update_stored_output_state(updated_output);
            notify_multiroom_speaker_toolbar_changed();
        }
        schedule_control_update();
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
    }
}

static bool output_available_for_group(const multiroom::OutputDevice& output) {
    return output.supports_airplay2 && !output.endpoint_host.empty() && output.endpoint_port != 0;
}

std::vector<multiroom::SpeakerGroup> MultiroomComponentState::speaker_groups() {
    return load_speaker_groups();
}

std::string MultiroomComponentState::save_speaker_group(
    const std::string& id,
    const std::string& name,
    const std::vector<std::string>& output_ids) {
    auto group = multiroom::normalize_speaker_group({id, name, output_ids});
    if (group.name.empty()) {
        throw std::invalid_argument("Speaker group name cannot be empty.");
    }
    if (group.output_ids.empty()) {
        throw std::invalid_argument("Select at least one speaker for the group.");
    }
    if (group.id.empty()) group.id = new_speaker_group_id();

    {
        std::lock_guard lock(mutex_);
        const auto unsupported = std::find_if(cached_outputs_.begin(), cached_outputs_.end(), [&](const auto& output) {
            return !output.supports_airplay2 &&
                multiroom::speaker_group_contains_persisted_output(group, output.id, cached_outputs_);
        });
        if (unsupported != cached_outputs_.end()) {
            throw std::invalid_argument("AirPlay 2 is required for group member: " + unsupported->id);
        }
    }

    {
        std::lock_guard lock(cfg_speaker_groups_mutex);
        auto groups = multiroom::deserialize_speaker_groups(cfg_speaker_groups.get().c_str());
        const auto duplicate_name = std::find_if(groups.begin(), groups.end(), [&](const auto& existing) {
            return existing.id != group.id && _stricmp(existing.name.c_str(), group.name.c_str()) == 0;
        });
        if (duplicate_name != groups.end()) {
            throw std::invalid_argument("A speaker group with this name already exists.");
        }

        const auto existing = std::find_if(groups.begin(), groups.end(), [&](const auto& candidate) {
            return candidate.id == group.id;
        });
        if (existing == groups.end()) {
            groups.push_back(group);
        } else {
            *existing = group;
        }
        const auto serialized = multiroom::serialize_speaker_groups(groups);
        cfg_speaker_groups = serialized.c_str();
    }
    notify_multiroom_speaker_toolbar_changed();
    return group.id;
}

void MultiroomComponentState::delete_speaker_group(const std::string& id) {
    bool changed = false;
    {
        std::lock_guard lock(cfg_speaker_groups_mutex);
        auto groups = multiroom::deserialize_speaker_groups(cfg_speaker_groups.get().c_str());
        const auto old_size = groups.size();
        groups.erase(
            std::remove_if(groups.begin(), groups.end(), [&](const auto& group) { return group.id == id; }),
            groups.end());
        changed = groups.size() != old_size;
        if (changed) {
            const auto serialized = multiroom::serialize_speaker_groups(groups);
            cfg_speaker_groups = serialized.c_str();
        }
    }
    if (changed) notify_multiroom_speaker_toolbar_changed();
}

bool MultiroomComponentState::activate_speaker_group(const std::string& id) {
    try {
        const auto groups = load_speaker_groups();
        const auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& candidate) {
            return candidate.id == id;
        });
        if (group == groups.end()) throw std::invalid_argument("Speaker group no longer exists.");

        std::vector<multiroom::OutputDevice> changed_outputs;
        std::vector<multiroom::OutputDevice> known_outputs;
        bool no_group_members_available = false;
        {
            std::lock_guard lock(mutex_);
            auto available_outputs = cached_outputs_;
            available_outputs.erase(
                std::remove_if(available_outputs.begin(), available_outputs.end(), [](const auto& output) {
                    return !output_available_for_group(output);
                }),
                available_outputs.end());
            const auto resolved_ids = multiroom::resolve_speaker_group_output_ids(*group, available_outputs);
            no_group_members_available = resolved_ids.empty();
            for (auto& output : cached_outputs_) {
                const bool selected = std::find(resolved_ids.begin(), resolved_ids.end(), output.id) != resolved_ids.end();
                if (output.selected != selected) {
                    output.selected = selected;
                    changed_outputs.push_back(output);
                }
            }
            known_outputs = cached_outputs_;
            last_error_.clear();
        }

        for (const auto& output : changed_outputs) update_stored_output_state(output);
        // The full preset is authoritative. Write it after the current cache
        // snapshot so a discovered AirPlay 2 member which is temporarily
        // missing its endpoint remains queued for the next discovery result.
        replace_stored_output_selection(*group, known_outputs);
        notify_multiroom_speaker_toolbar_changed();
        schedule_control_update();
        if (no_group_members_available && !refresh_in_progress()) refresh_outputs();
        return true;
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
        return false;
    }
}

std::string MultiroomComponentState::active_speaker_group_id() {
    const auto groups = load_speaker_groups();
    std::vector<multiroom::OutputDevice> known_outputs;
    {
        std::lock_guard lock(mutex_);
        known_outputs = cached_outputs_;
    }
    std::vector<std::string> persisted_selected_ids;
    for (const auto& state : load_output_state()) {
        if (state.selected) persisted_selected_ids.push_back(state.output_id);
    }
    const auto persisted_active = std::find_if(groups.begin(), groups.end(), [&](const auto& group) {
        return multiroom::speaker_group_matches_persisted_selection(
            group, persisted_selected_ids, known_outputs);
    });
    if (persisted_active != groups.end()) return persisted_active->id;

    auto available_outputs = known_outputs;
    available_outputs.erase(
        std::remove_if(available_outputs.begin(), available_outputs.end(), [](const auto& output) {
            return !output_available_for_group(output);
        }),
        available_outputs.end());
    const auto active = std::find_if(groups.begin(), groups.end(), [&](const auto& group) {
        return multiroom::speaker_group_matches_selection(group, available_outputs);
    });
    return active == groups.end() ? std::string{} : active->id;
}

void MultiroomComponentState::set_output_dropdown_visibility(const std::string& id, bool visible) {
    multiroom::OutputDevice updated_output;
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        const auto output_it = std::find_if(cached_outputs_.begin(), cached_outputs_.end(), [&](const auto& output) {
            return output.id == id;
        });
        if (output_it != cached_outputs_.end() && output_it->visible_in_dropdown != visible) {
            output_it->visible_in_dropdown = visible;
            updated_output = *output_it;
            changed = true;
        }
        last_error_.clear();
    }
    if (changed) {
        update_stored_output_state(updated_output);
        notify_multiroom_speaker_toolbar_changed();
    }
}

void MultiroomComponentState::set_output_volume(const std::string& id, int volume) {
    try {
        multiroom::OutputDevice updated_output;
        bool changed = false;
        {
            std::lock_guard lock(mutex_);
            for (auto& output : cached_outputs_) {
                if (output.id == id) {
                    output.volume = std::clamp(volume, 0, 100);
                    updated_output = output;
                    changed = true;
                    break;
                }
            }
            last_error_.clear();
        }
        if (changed) {
            update_stored_output_state(updated_output);
            schedule_volume_update({updated_output.id});
        }
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
    }
}

void MultiroomComponentState::set_master_volume_percent(int volume) {
    try {
        std::vector<std::string> output_ids;
        {
            std::lock_guard lock(mutex_);
            master_volume_percent_ = std::clamp(volume, 0, 100);
            if (discovery_started_) {
                output_ids.reserve(cached_outputs_.size());
                for (const auto& output : cached_outputs_) {
                    output_ids.push_back(output.id);
                }
            }
            last_error_.clear();
        }
        schedule_volume_update(output_ids);
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
    }
}

void MultiroomComponentState::set_playback_metadata(const multiroom::PlaybackMetadata& metadata) {
    {
        std::lock_guard lock(mutex_);
        playback_metadata_ = metadata;
        playback_metadata_active_ = true;
    }
    schedule_metadata_update();
}

void MultiroomComponentState::update_playback_position(uint64_t position_ms) {
    std::lock_guard lock(mutex_);
    if (playback_metadata_active_) {
        playback_metadata_.position_ms = position_ms;
    }
}

void MultiroomComponentState::clear_playback_metadata() {
    {
        std::lock_guard lock(mutex_);
        playback_metadata_ = {};
        playback_metadata_active_ = false;
    }
    schedule_metadata_update();
}

void MultiroomComponentState::prepare_playback_open() {
    playback_open_cancel_requested_.store(false);
    transport_.reset_pending_open_cancel();
    {
        std::lock_guard lock(mutex_);
        // Foobar returns from output::open() before the render thread begins
        // the network handshake. Publish Connecting here so UI discovery
        // cannot enter that asynchronous handoff window.
        playback_connecting_ = true;
        active_output_names_.clear();
        last_error_.clear();
    }
    notify_multiroom_speaker_toolbar_changed();
}

void MultiroomComponentState::cancel_pending_playback_open() {
    playback_open_cancel_requested_.store(true);
    // Deliberately do not take transport_mutex_: the setup worker owns it while
    // blocked in connect/send/recv. Closing its pending socket is what wakes it.
    transport_.cancel_pending_open();
}

void MultiroomComponentState::open_playback_stream(const multiroom::PcmFormat& format) {
    try {
        validate_playback_format(format);
        ensure_discovery_started();
        refresh_outputs_for_playback();
        if (playback_open_cancel_requested_.load()) {
            throw std::runtime_error("AirPlay playback setup cancelled.");
        }

        std::vector<multiroom::OutputDevice> outputs_snapshot;
        multiroom::PlaybackMetadata metadata_snapshot;
        int master_volume_snapshot = 100;
        bool metadata_active = false;
        const bool saved_selection_expected = has_stored_selected_output();
        {
            std::lock_guard lock(mutex_);
            playback_format_ = format;
            playback_format_valid_ = true;
            outputs_snapshot = cached_outputs_;
            metadata_snapshot = playback_metadata_;
            metadata_active = playback_metadata_active_;
            master_volume_snapshot = master_volume_percent_;
            last_error_.clear();
        }
        if (selected_ids(outputs_snapshot).empty() && saved_selection_expected) {
            throw std::runtime_error("Saved AirPlay speaker selection was not discovered.");
        }

        std::vector<multiroom::OutputDevice> refreshed_outputs;
        std::vector<std::wstring> active_output_names;
        {
            std::lock_guard transport_lock(transport_mutex_);
            if (!playback_engine_) {
                playback_engine_ = std::make_unique<multiroom::MultiroomEngine>(transport_);
            }
            transport_.set_enabled_outputs(selected_ids(outputs_snapshot));
            for (const auto& output : outputs_snapshot) {
                transport_.set_output_volume(output.id, effective_remote_volume(output.volume, master_volume_snapshot));
            }
            playback_engine_->open_stream(format);
            if (metadata_active) {
                transport_.set_playback_metadata(metadata_snapshot);
            } else {
                transport_.clear_playback_metadata();
            }
            transport_.connect_selected_outputs();
            if (playback_open_cancel_requested_.load()) {
                playback_engine_->stop();
                throw std::runtime_error("AirPlay playback setup cancelled.");
            }
            for (const auto& output : outputs_snapshot) {
                transport_.set_output_volume(output.id, effective_remote_volume(output.volume, master_volume_snapshot));
            }
            refreshed_outputs = transport_.list_outputs();
            preserve_user_output_state(refreshed_outputs, outputs_snapshot);
            for (const auto& session : transport_.sessions()) {
                if (session.open && session.phase == multiroom::airplay::AirPlaySessionPhase::Ready) {
                    active_output_names.push_back(widen_utf8(
                        session.output_name.empty() ? session.output_id : session.output_name));
                }
            }
        }

        {
            std::lock_guard lock(mutex_);
            cached_outputs_ = std::move(refreshed_outputs);
            active_output_names_ = std::move(active_output_names);
            // Publish Open before clearing Connecting while holding the same
            // lock used by playback_active(). A refresh must never observe a
            // false/false gap after the AirPlay sessions are already ready.
            playback_open_.store(true);
            playback_connecting_ = false;
            last_error_.clear();
        }
        notify_multiroom_speaker_toolbar_changed();
        // Selection/volume callbacks may have run while the session handshake
        // was in progress. Reconcile once more now that reconnecting is safe.
        schedule_control_update();
    } catch (const std::exception& e) {
        {
            std::lock_guard transport_lock(transport_mutex_);
            if (playback_engine_ && playback_engine_->stream_open()) {
                playback_engine_->stop();
            }
            playback_open_.store(false);
        }
        {
            std::lock_guard lock(mutex_);
            playback_format_valid_ = false;
            playback_connecting_ = false;
            active_output_names_.clear();
            last_error_ = widen_utf8(e.what());
        }
        notify_multiroom_speaker_toolbar_changed();
        throw;
    }
}

void MultiroomComponentState::write_playback_pcm(const void* frames, size_t bytes) {
    std::wstring playback_error;
    try {
        {
            std::lock_guard transport_lock(transport_mutex_);
            if (!playback_engine_ || !playback_open_.load()) {
                return;
            }
            if (!playback_engine_->stream_open()) {
                return;
            }
            playback_engine_->write_interleaved_pcm(frames, bytes);
        }

        std::lock_guard lock(mutex_);
        last_error_.clear();
    } catch (const std::exception& e) {
        playback_error = widen_utf8(e.what());
        {
            std::lock_guard lock(mutex_);
            last_error_ = playback_error;
        }
        FB2K_console_formatter() << "[Universal Multiroom] AirPlay PCM write failed: " << e.what();
        throw;
    }
}

void MultiroomComponentState::flush_playback() {
    try {
        std::lock_guard transport_lock(transport_mutex_);
        if (playback_engine_ && playback_open_.load()) {
            playback_engine_->flush();
        }
        std::lock_guard lock(mutex_);
        last_error_.clear();
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
        throw;
    }
}

void MultiroomComponentState::stop_playback() {
    bool run_deferred_refresh = false;
    try {
        std::lock_guard transport_lock(transport_mutex_);
        if (playback_engine_ && playback_open_.load()) {
            playback_engine_->stop();
        }
        playback_open_.store(false);
        playback_open_cancel_requested_.store(false);
        transport_.reset_pending_open_cancel();
        std::lock_guard lock(mutex_);
        playback_format_valid_ = false;
        playback_connecting_ = false;
        active_output_names_.clear();
        run_deferred_refresh = refresh_deferred_until_stop_;
        refresh_deferred_until_stop_ = false;
        last_error_.clear();
    } catch (const std::exception& e) {
        {
            std::lock_guard transport_lock(transport_mutex_);
            playback_open_.store(false);
            playback_open_cancel_requested_.store(false);
            transport_.reset_pending_open_cancel();
        }
        std::lock_guard lock(mutex_);
        playback_format_valid_ = false;
        playback_connecting_ = false;
        active_output_names_.clear();
        last_error_ = widen_utf8(e.what());
        throw;
    }
    notify_multiroom_speaker_toolbar_changed();
    if (run_deferred_refresh && !shutting_down_.load()) {
        refresh_outputs();
    }
}

void MultiroomComponentState::report_playback_failure(const std::string& message) {
    {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(message);
    }
    FB2K_console_formatter() << "[Universal Multiroom] AirPlay stream stopped after transport failure: "
                              << message.c_str();

    if (shutting_down_.load() || playback_failure_stop_queued_.exchange(true)) {
        return;
    }

    fb2k::inMainThread([this] {
        if (shutting_down_.load()) {
            playback_failure_stop_queued_.store(false);
            return;
        }

        try {
            auto control = playback_control::get();
            if (control->is_playing()) {
                control->stop();
            } else {
                stop_playback();
            }
        } catch (const std::exception& e) {
            FB2K_console_formatter() << "[Universal Multiroom] Could not stop foobar after AirPlay failure: "
                                      << e.what();
        }
        playback_failure_stop_queued_.store(false);
    });
}

std::wstring MultiroomComponentState::playback_destination_label() {
    std::lock_guard lock(mutex_);
    if (playback_connecting_) return L"Connecting...";
    if (!playback_open_.load() || active_output_names_.empty()) return L"Idle";
    if (active_output_names_.size() == 1) return active_output_names_.front();
    return active_output_names_.front() + L" +" + std::to_wstring(active_output_names_.size() - 1);
}

std::wstring MultiroomComponentState::status_text() {
    std::lock_guard lock(mutex_);
    if (!last_error_.empty()) return L"Discovery error: " + last_error_;
    if (!discovery_started_) return L"Discovery: not started";
    if (pairing_in_progress_) return L"Speakers: pairing";
    if (control_in_progress_) return L"Speakers: applying selection";
    if (refresh_deferred_until_stop_) return L"Discovery: refresh queued until playback stops";
    if (refresh_in_progress_) {
        std::wstringstream stream;
        stream << L"Discovery: scanning";
        if (!cached_outputs_.empty()) {
            stream << L", " << cached_outputs_.size() << L" cached";
        }
        return stream.str();
    }

    size_t selected = 0;
    for (const auto& output : cached_outputs_) {
        if (output.selected) ++selected;
    }

    std::wstringstream stream;
    stream << L"Discovery: " << cached_outputs_.size() << L" speaker";
    if (cached_outputs_.size() != 1) stream << L"s";
    stream << L", " << selected << L" selected, " << refresh_count_ << L" scan";
    if (refresh_count_ != 1) stream << L"s";
    return stream.str();
}

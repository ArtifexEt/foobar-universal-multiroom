#include "stdafx.h"
#include "multiroom_component_state.h"
#include "speaker_toolbar.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

static constexpr GUID guid_cfg_airplay_pairing_credentials = {
    0xf9fecc9d, 0x639a, 0x45ad, {0x8a, 0x11, 0x4b, 0x55, 0x5f, 0x6f, 0x50, 0xa4}};
static constexpr GUID guid_cfg_airplay_output_state = {
    0x5e565c70, 0x8175, 0x4c61, {0xb4, 0x22, 0x99, 0x8d, 0x4f, 0x7c, 0x65, 0x19}};

static cfg_string cfg_airplay_pairing_credentials(guid_cfg_airplay_pairing_credentials, "");
static cfg_string cfg_airplay_output_state(guid_cfg_airplay_output_state, "");
static std::mutex cfg_airplay_output_state_mutex;

struct StoredOutputState {
    std::string output_id;
    bool selected = false;
    int volume = 50;
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
        if (fields.size() != 3 || fields[0].empty()) {
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
            << std::clamp(state.volume, 0, 100) << '\n';
    }
    return out.str();
}

std::vector<StoredOutputState> load_output_state() {
    std::lock_guard lock(cfg_airplay_output_state_mutex);
    return parse_output_state_text(cfg_airplay_output_state.get().c_str());
}

void save_output_state(std::vector<StoredOutputState> states) {
    std::lock_guard lock(cfg_airplay_output_state_mutex);
    const auto serialized = format_output_state_text(states);
    cfg_airplay_output_state = serialized.c_str();
}

void update_stored_output_state(const multiroom::OutputDevice& output) {
    auto states = load_output_state();
    const auto it = std::find_if(states.begin(), states.end(), [&](const auto& state) {
        return state.output_id == output.id;
    });

    StoredOutputState updated;
    updated.output_id = output.id;
    updated.selected = output.selected;
    updated.volume = std::clamp(output.volume, 0, 100);

    if (it == states.end()) {
        states.push_back(std::move(updated));
    } else {
        *it = std::move(updated);
    }

    save_output_state(std::move(states));
}

void apply_stored_output_state(std::vector<multiroom::OutputDevice>& outputs) {
    const auto states = load_output_state();
    for (auto& output : outputs) {
        const auto it = std::find_if(states.begin(), states.end(), [&](const auto& state) {
            return state.output_id == output.id;
        });
        if (it == states.end()) {
            continue;
        }
        output.selected = it->selected;
        output.volume = it->volume;
    }
}

bool has_stored_selected_output() {
    const auto states = load_output_state();
    return std::any_of(states.begin(), states.end(), [](const auto& state) {
        return state.selected;
    });
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
    const auto clamped_speaker = std::clamp(speaker_volume, 0, 100);
    const auto clamped_master = std::clamp(master_volume_percent, 0, 100);
    return std::clamp((clamped_speaker * clamped_master + 50) / 100, 0, 100);
}

void preserve_user_output_state(
    std::vector<multiroom::OutputDevice>& refreshed,
    const std::vector<multiroom::OutputDevice>& previous) {
    for (auto& output : refreshed) {
        const auto it = std::find_if(previous.begin(), previous.end(), [&](const auto& candidate) {
            return candidate.id == output.id;
        });
        if (it == previous.end()) {
            continue;
        }
        output.selected = it->selected;
        output.volume = it->volume;
        output.offset_ms = it->offset_ms;
        output.measured_latency_ms = it->measured_latency_ms;
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
    : transport_(multiroom::airplay::make_airplay_rtsp_control_client(std::make_shared<FoobarAirPlayPairingStore>())) {}

MultiroomComponentState::~MultiroomComponentState() {
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

void MultiroomComponentState::ensure_discovery_started() {
    bool should_start = false;
    {
        std::lock_guard lock(mutex_);
        if (!discovery_started_) {
            discovery_started_ = true;
            should_start = true;
        }
    }

    if (should_start) {
        std::lock_guard transport_lock(transport_mutex_);
        transport_.start_discovery();
    }
}

void MultiroomComponentState::refresh_outputs() {
    ensure_discovery_started();

    {
        std::lock_guard lock(mutex_);
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
            std::lock_guard transport_lock(transport_mutex_);
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
        notify_multiroom_speaker_toolbar_changed();

        if (refresh_ok) {
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
            if (refresh_ok && playback_open_.load()) {
                schedule_control_update();
            }
            return;
        }
    }
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
            transport_.refresh_discovery(std::chrono::milliseconds(2500));
            refreshed_outputs = transport_.list_outputs();
            apply_stored_output_state(refreshed_outputs);
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
    notify_multiroom_speaker_toolbar_changed();

    FB2K_console_formatter() << "[Universal Multiroom] AirPlay playback discovery refresh #"
                              << refresh_number << " applied saved speaker state";
}

void MultiroomComponentState::schedule_control_update() {
    {
        std::lock_guard lock(mutex_);
        if (control_in_progress_) {
            control_requested_ = true;
            return;
        }
        control_in_progress_ = true;
        control_requested_ = false;
    }

    if (control_thread_.joinable()) {
        control_thread_.join();
    }
    control_thread_ = std::thread(&MultiroomComponentState::control_update_worker, this);
}

void MultiroomComponentState::control_update_worker() {
    for (;;) {
        std::vector<multiroom::OutputDevice> outputs_snapshot;
        multiroom::PcmFormat playback_format_snapshot{};
        int master_volume_snapshot = 100;
        bool should_connect = false;
        {
            std::lock_guard lock(mutex_);
            outputs_snapshot = cached_outputs_;
            master_volume_snapshot = master_volume_percent_;
            should_connect = playback_open_.load() && playback_format_valid_;
            if (should_connect) {
                playback_format_snapshot = playback_format_;
            }
        }

        std::wstring control_error;
        std::string control_error_narrow;
        bool control_ok = false;

        try {
            const auto selected = selected_ids(outputs_snapshot);
            std::lock_guard transport_lock(transport_mutex_);
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
            control_ok = true;
        } catch (const std::exception& e) {
            control_error_narrow = e.what();
            control_error = widen_utf8(e.what());
        }

        {
            std::lock_guard lock(mutex_);
            if (control_ok) {
                cached_outputs_ = outputs_snapshot;
                last_error_.clear();
            } else {
                last_error_ = std::move(control_error);
            }
        }
        notify_multiroom_speaker_toolbar_changed();

        if (control_ok) {
            FB2K_console_formatter() << "[Universal Multiroom] Speaker control update applied";
        } else {
            FB2K_console_formatter() << "[Universal Multiroom] Speaker control update failed: "
                                      << control_error_narrow.c_str();
        }

        bool run_again = false;
        {
            std::lock_guard lock(mutex_);
            if (control_requested_) {
                control_requested_ = false;
                run_again = true;
            } else {
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
    return refresh_in_progress_;
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
        ensure_discovery_started();
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
        ensure_discovery_started();

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

void MultiroomComponentState::set_output_volume(const std::string& id, int volume) {
    try {
        ensure_discovery_started();
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
        }
        schedule_control_update();
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
    }
}

void MultiroomComponentState::set_master_volume_percent(int volume) {
    try {
        bool discovery_started = false;
        {
            std::lock_guard lock(mutex_);
            master_volume_percent_ = std::clamp(volume, 0, 100);
            discovery_started = discovery_started_;
            last_error_.clear();
        }
        if (discovery_started) {
            schedule_control_update();
        }
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
    }
}

void MultiroomComponentState::open_playback_stream(const multiroom::PcmFormat& format) {
    try {
        validate_playback_format(format);
        ensure_discovery_started();
        refresh_outputs_for_playback();

        std::vector<multiroom::OutputDevice> outputs_snapshot;
        int master_volume_snapshot = 100;
        const bool saved_selection_expected = has_stored_selected_output();
        {
            std::lock_guard lock(mutex_);
            playback_format_ = format;
            playback_format_valid_ = true;
            outputs_snapshot = cached_outputs_;
            master_volume_snapshot = master_volume_percent_;
            last_error_.clear();
        }
        if (selected_ids(outputs_snapshot).empty() && saved_selection_expected) {
            throw std::runtime_error("Saved AirPlay speaker selection was not discovered.");
        }

        std::vector<multiroom::OutputDevice> refreshed_outputs;
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
            transport_.connect_selected_outputs();
            for (const auto& output : outputs_snapshot) {
                transport_.set_output_volume(output.id, effective_remote_volume(output.volume, master_volume_snapshot));
            }
            refreshed_outputs = transport_.list_outputs();
            preserve_user_output_state(refreshed_outputs, outputs_snapshot);
        }

        {
            std::lock_guard lock(mutex_);
            cached_outputs_ = std::move(refreshed_outputs);
            last_error_.clear();
        }
        playback_open_.store(true);
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
            last_error_ = widen_utf8(e.what());
        }
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
    try {
        std::lock_guard transport_lock(transport_mutex_);
        if (playback_engine_ && playback_open_.load()) {
            playback_engine_->stop();
        }
        playback_open_.store(false);
        std::lock_guard lock(mutex_);
        playback_format_valid_ = false;
        last_error_.clear();
    } catch (const std::exception& e) {
        {
            std::lock_guard transport_lock(transport_mutex_);
            playback_open_.store(false);
        }
        std::lock_guard lock(mutex_);
        playback_format_valid_ = false;
        last_error_ = widen_utf8(e.what());
        throw;
    }
}

std::wstring MultiroomComponentState::selected_label() {
    const auto current_outputs = outputs();

    std::wstring first;
    size_t count = 0;
    for (const auto& output : current_outputs) {
        if (!output.selected) continue;
        if (first.empty()) first = widen_utf8(output.name);
        ++count;
    }

    if (count == 0) return L"No speakers";
    if (count == 1) return first;
    return first + L" +" + std::to_wstring(count - 1);
}

std::wstring MultiroomComponentState::status_text() {
    std::lock_guard lock(mutex_);
    if (!last_error_.empty()) return L"AirPlay discovery error: " + last_error_;
    if (!discovery_started_) return L"AirPlay discovery: not started";
    if (pairing_in_progress_) return L"AirPlay speakers: pairing";
    if (control_in_progress_) return L"AirPlay speakers: applying selection";
    if (refresh_in_progress_) {
        std::wstringstream stream;
        stream << L"AirPlay discovery: scanning";
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
    stream << L"AirPlay discovery: " << cached_outputs_.size() << L" speaker";
    if (cached_outputs_.size() != 1) stream << L"s";
    stream << L", " << selected << L" selected, " << refresh_count_ << L" scan";
    if (refresh_count_ != 1) stream << L"s";
    return stream.str();
}

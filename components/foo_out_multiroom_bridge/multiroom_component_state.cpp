#include "stdafx.h"
#include "multiroom_component_state.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (required <= 1) return {};

    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), required);
    result.resize(static_cast<size_t>(required - 1));
    return result;
}

std::string narrow_utf16(const std::wstring& text) {
    if (text.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};

    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), required, nullptr, nullptr);
    result.resize(static_cast<size_t>(required - 1));
    return result;
}

std::string lowercase_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::vector<std::string> selected_ids(const std::vector<multiroom::OutputDevice>& outputs) {
    std::vector<std::string> result;
    for (const auto& output : outputs) {
        if (output.selected) result.push_back(output.id);
    }
    return result;
}

void validate_playback_format(const multiroom::PcmFormat& format) {
    if (format.sample_rate == 0 || format.channels == 0 || format.bits_per_sample == 0) {
        throw std::invalid_argument("PCM format must have non-zero sample rate, channels, and bit depth.");
    }
    if (format.bits_per_sample % 8 != 0) {
        throw std::invalid_argument("PCM bit depth must be byte aligned.");
    }
}

}  // namespace

MultiroomComponentState& MultiroomComponentState::instance() {
    static MultiroomComponentState state;
    return state;
}

MultiroomComponentState::~MultiroomComponentState() {
    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }
    if (control_thread_.joinable()) {
        control_thread_.join();
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
        bool should_connect = false;
        {
            std::lock_guard lock(mutex_);
            outputs_snapshot = cached_outputs_;
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
                transport_.set_output_volume(output.id, output.volume);
            }
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

std::vector<multiroom::OutputDevice> MultiroomComponentState::outputs() {
    std::lock_guard lock(mutex_);
    return cached_outputs_;
}

bool MultiroomComponentState::add_manual_airplay_output(
    const std::wstring& name,
    const std::wstring& host,
    std::uint16_t port) {
    try {
        const auto endpoint_host = narrow_utf16(host);
        if (endpoint_host.empty()) {
            throw std::invalid_argument("Manual AirPlay host/IP is required.");
        }
        if (port == 0) {
            throw std::invalid_argument("Manual AirPlay port must be between 1 and 65535.");
        }

        const auto endpoint_port = std::to_string(port);
        auto output_name = narrow_utf16(name);
        if (output_name.empty()) {
            output_name = endpoint_host + ":" + endpoint_port;
        }

        multiroom::OutputDevice device;
        device.id = "airplay:manual:" + lowercase_ascii(endpoint_host) + ":" + endpoint_port;
        device.name = output_name;
        device.type = multiroom::OutputType::AirPlay;
        device.requires_auth = true;
        device.needs_auth_key = true;
        device.supports_airplay2 = true;
        device.requires_encrypted_stream = true;
        device.volume = 50;
        device.format = "airplay2";
        device.supported_formats = {"airplay2"};
        device.endpoint_host = endpoint_host;
        device.endpoint_port = port;
        device.txt_records = {
            {"manual", "1"},
            {"et", "4"},
        };

        ensure_discovery_started();

        std::vector<multiroom::OutputDevice> outputs_snapshot;
        {
            std::lock_guard transport_lock(transport_mutex_);
            transport_.add_discovered_output(std::move(device));
            outputs_snapshot = transport_.list_outputs();
        }
        {
            std::lock_guard lock(mutex_);
            cached_outputs_ = std::move(outputs_snapshot);
            last_error_.clear();
        }

        FB2K_console_formatter() << "[Universal Multiroom] Manual AirPlay device added: "
                                  << output_name.c_str()
                                  << " endpoint=" << endpoint_host.c_str()
                                  << ":" << port;
        return true;
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
        return false;
    }
}

void MultiroomComponentState::toggle_output(const std::string& id) {
    try {
        ensure_discovery_started();

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
            }
            last_error_.clear();
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
        {
            std::lock_guard lock(mutex_);
            for (auto& output : cached_outputs_) {
                if (output.id == id) {
                    output.volume = std::clamp(volume, 0, 100);
                    break;
                }
            }
            last_error_.clear();
        }
        schedule_control_update();
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
    }
}

void MultiroomComponentState::open_playback_stream(const multiroom::PcmFormat& format) {
    try {
        validate_playback_format(format);
        ensure_discovery_started();
        {
            std::lock_guard lock(mutex_);
            playback_format_ = format;
            playback_format_valid_ = true;
            last_error_.clear();
        }

        std::unique_lock transport_lock(transport_mutex_, std::try_to_lock);
        if (!transport_lock.owns_lock()) {
            playback_open_.store(true);
            schedule_control_update();
            return;
        }

        {
            if (!playback_engine_) {
                playback_engine_ = std::make_unique<multiroom::MultiroomEngine>(transport_);
            }
            playback_engine_->open_stream(format);
        }
        playback_open_.store(true);
        schedule_control_update();
    } catch (const std::exception& e) {
        {
            std::lock_guard transport_lock(transport_mutex_);
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
    try {
        std::unique_lock transport_lock(transport_mutex_, std::try_to_lock);
        if (!transport_lock.owns_lock()) {
            return;
        }
        if (!playback_engine_ || !playback_open_.load()) {
            return;
        }
        if (!playback_engine_->stream_open()) {
            return;
        }
        playback_engine_->write_interleaved_pcm(frames, bytes);
        std::lock_guard lock(mutex_);
        last_error_.clear();
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
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

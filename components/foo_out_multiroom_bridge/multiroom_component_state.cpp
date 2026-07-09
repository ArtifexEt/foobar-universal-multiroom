#include "stdafx.h"
#include "multiroom_component_state.h"

#include <algorithm>
#include <sstream>
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

std::vector<std::string> selected_ids(const std::vector<multiroom::OutputDevice>& outputs) {
    std::vector<std::string> result;
    for (const auto& output : outputs) {
        if (output.selected) result.push_back(output.id);
    }
    return result;
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
}

void MultiroomComponentState::ensure_discovery_started() {
    std::lock_guard transport_lock(transport_mutex_);

    bool should_start = false;
    {
        std::lock_guard lock(mutex_);
        if (!discovery_started_) {
            discovery_started_ = true;
            should_start = true;
        }
    }

    if (should_start) {
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

bool MultiroomComponentState::refresh_in_progress() {
    std::lock_guard lock(mutex_);
    return refresh_in_progress_;
}

std::vector<multiroom::OutputDevice> MultiroomComponentState::outputs() {
    std::lock_guard lock(mutex_);
    return cached_outputs_;
}

void MultiroomComponentState::toggle_output(const std::string& id) {
    try {
        ensure_discovery_started();
        std::vector<multiroom::OutputDevice> outputs_snapshot;
        {
            std::lock_guard lock(mutex_);
            outputs_snapshot = cached_outputs_;
        }

        auto selected = selected_ids(outputs_snapshot);

        const auto output_it = std::find_if(outputs_snapshot.begin(), outputs_snapshot.end(), [&](const auto& output) {
            return output.id == id;
        });
        if (output_it != outputs_snapshot.end() && !output_it->supports_airplay2) {
            throw std::runtime_error("AirPlay 2 is required for the multiroom MVP: " + id);
        }

        const auto it = std::find(selected.begin(), selected.end(), id);
        if (it == selected.end()) {
            selected.push_back(id);
        } else {
            selected.erase(it);
        }

        {
            std::lock_guard transport_lock(transport_mutex_);
            transport_.set_enabled_outputs(selected);
            outputs_snapshot = transport_.list_outputs();
        }
        {
            std::lock_guard lock(mutex_);
            cached_outputs_ = std::move(outputs_snapshot);
            last_error_.clear();
        }
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
    }
}

void MultiroomComponentState::set_output_volume(const std::string& id, int volume) {
    try {
        ensure_discovery_started();
        std::vector<multiroom::OutputDevice> outputs_snapshot;
        {
            std::lock_guard transport_lock(transport_mutex_);
            transport_.set_output_volume(id, volume);
            outputs_snapshot = transport_.list_outputs();
        }
        for (auto& output : outputs_snapshot) {
            if (output.id == id) {
                output.volume = std::clamp(volume, 0, 100);
                break;
            }
        }
        {
            std::lock_guard lock(mutex_);
            cached_outputs_ = std::move(outputs_snapshot);
            last_error_.clear();
        }
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
    }
}

void MultiroomComponentState::open_playback_stream(const multiroom::PcmFormat& format) {
    try {
        ensure_discovery_started();
        std::lock_guard transport_lock(transport_mutex_);
        if (!playback_engine_) {
            playback_engine_ = std::make_unique<multiroom::MultiroomEngine>(transport_);
        }
        playback_engine_->open_stream(format);
        playback_open_ = true;
        std::lock_guard lock(mutex_);
        last_error_.clear();
    } catch (const std::exception& e) {
        {
            std::lock_guard transport_lock(transport_mutex_);
            playback_open_ = false;
        }
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
        throw;
    }
}

void MultiroomComponentState::write_playback_pcm(const void* frames, size_t bytes) {
    try {
        std::lock_guard transport_lock(transport_mutex_);
        if (!playback_engine_ || !playback_open_) {
            throw std::logic_error("Multiroom playback stream is not open.");
        }
        playback_engine_->write_interleaved_pcm(frames, bytes);
        std::lock_guard lock(mutex_);
        last_error_.clear();
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = widen_utf8(e.what());
        throw;
    }
}

void MultiroomComponentState::flush_playback() {
    try {
        std::lock_guard transport_lock(transport_mutex_);
        if (playback_engine_ && playback_open_) {
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
        if (playback_engine_ && playback_open_) {
            playback_engine_->stop();
        }
        playback_open_ = false;
        std::lock_guard lock(mutex_);
        last_error_.clear();
    } catch (const std::exception& e) {
        {
            std::lock_guard transport_lock(transport_mutex_);
            playback_open_ = false;
        }
        std::lock_guard lock(mutex_);
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

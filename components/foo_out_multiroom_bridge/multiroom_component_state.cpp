#include "stdafx.h"
#include "multiroom_component_state.h"

#include <algorithm>
#include <sstream>

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

void MultiroomComponentState::ensure_discovery_started() {
    if (discovery_started_) return;
    transport_.start_discovery();
    discovery_started_ = true;
}

void MultiroomComponentState::refresh_outputs() {
    std::lock_guard lock(mutex_);
    try {
        ensure_discovery_started();
        cached_outputs_ = transport_.list_outputs();
        ++refresh_count_;
        last_error_.clear();
        FB2K_console_formatter() << "[Universal Multiroom] AirPlay discovery refresh #" << refresh_count_
                                  << ": " << cached_outputs_.size() << " device(s)";
        for (const auto& output : cached_outputs_) {
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
    } catch (const std::exception& e) {
        ++refresh_count_;
        last_error_ = widen_utf8(e.what());
        FB2K_console_formatter() << "[Universal Multiroom] AirPlay discovery failed: " << e.what();
    }
}

std::vector<multiroom::OutputDevice> MultiroomComponentState::outputs() {
    std::lock_guard lock(mutex_);
    return cached_outputs_;
}

void MultiroomComponentState::toggle_output(const std::string& id) {
    std::lock_guard lock(mutex_);
    try {
        ensure_discovery_started();
        auto selected = selected_ids(cached_outputs_);

        const auto output_it = std::find_if(cached_outputs_.begin(), cached_outputs_.end(), [&](const auto& output) {
            return output.id == id;
        });
        if (output_it != cached_outputs_.end() && !output_it->supports_airplay2) {
            throw std::runtime_error("AirPlay 2 is required for the multiroom MVP: " + id);
        }

        const auto it = std::find(selected.begin(), selected.end(), id);
        if (it == selected.end()) {
            selected.push_back(id);
        } else {
            selected.erase(it);
        }

        transport_.set_enabled_outputs(selected);
        cached_outputs_ = transport_.list_outputs();
        last_error_.clear();
    } catch (const std::exception& e) {
        last_error_ = widen_utf8(e.what());
    }
}

void MultiroomComponentState::set_output_volume(const std::string& id, int volume) {
    std::lock_guard lock(mutex_);
    try {
        ensure_discovery_started();
        transport_.set_output_volume(id, volume);
        for (auto& output : cached_outputs_) {
            if (output.id == id) {
                output.volume = std::clamp(volume, 0, 100);
                break;
            }
        }
        last_error_.clear();
    } catch (const std::exception& e) {
        last_error_ = widen_utf8(e.what());
    }
}

void MultiroomComponentState::open_playback_stream(const multiroom::PcmFormat& format) {
    std::lock_guard lock(mutex_);
    try {
        ensure_discovery_started();
        if (!playback_engine_) {
            playback_engine_ = std::make_unique<multiroom::MultiroomEngine>(transport_);
        }
        playback_engine_->open_stream(format);
        playback_open_ = true;
        last_error_.clear();
    } catch (const std::exception& e) {
        playback_open_ = false;
        last_error_ = widen_utf8(e.what());
        throw;
    }
}

void MultiroomComponentState::write_playback_pcm(const void* frames, size_t bytes) {
    std::lock_guard lock(mutex_);
    try {
        if (!playback_engine_ || !playback_open_) {
            throw std::logic_error("Multiroom playback stream is not open.");
        }
        playback_engine_->write_interleaved_pcm(frames, bytes);
        last_error_.clear();
    } catch (const std::exception& e) {
        last_error_ = widen_utf8(e.what());
        throw;
    }
}

void MultiroomComponentState::flush_playback() {
    std::lock_guard lock(mutex_);
    try {
        if (playback_engine_ && playback_open_) {
            playback_engine_->flush();
        }
        last_error_.clear();
    } catch (const std::exception& e) {
        last_error_ = widen_utf8(e.what());
        throw;
    }
}

void MultiroomComponentState::stop_playback() {
    std::lock_guard lock(mutex_);
    try {
        if (playback_engine_ && playback_open_) {
            playback_engine_->stop();
        }
        playback_open_ = false;
        last_error_.clear();
    } catch (const std::exception& e) {
        playback_open_ = false;
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

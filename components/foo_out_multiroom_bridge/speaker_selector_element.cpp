#include "stdafx.h"
#include "multiroom_component_state.h"
#include "speaker_selector_popup.h"

#include <commctrl.h>
#include <libPPUI/win32_op.h>

#include <sstream>

namespace {

static constexpr GUID guid_speaker_selector_element = {
    0x4f175c3d, 0x7d32, 0x4bb3, {0xa7, 0x24, 0x4a, 0x24, 0x47, 0x23, 0x43, 0xc3}};
static constexpr UINT_PTR kSpeakerCheckBase = 30000;
static constexpr UINT_PTR kSpeakerVolumeBase = 31000;
static constexpr UINT_PTR kRefreshCommand = 32000;
static constexpr UINT_PTR kEmptyStatusId = 32001;
static constexpr UINT_PTR kVolumeLabelBase = 33000;
static constexpr UINT_PTR kRefreshTimer = 34000;
static constexpr int kPopupWidth = 372;
static constexpr int kPopupPadding = 12;
static constexpr int kHeaderHeight = 34;
static constexpr int kRowHeight = 62;
static constexpr int kFooterHeight = 42;
static constexpr int kToolbarHeight = 28;

std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (required <= 1) return {};

    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), required);
    result.resize(static_cast<size_t>(required - 1));
    return result;
}

std::wstring volume_label(int volume) {
    return std::to_wstring(volume) + L"%";
}

std::string outputs_signature(const std::vector<multiroom::OutputDevice>& outputs) {
    std::ostringstream stream;
    for (const auto& output : outputs) {
        stream << output.id << '|'
               << output.name << '|'
               << output.selected << '|'
               << output.volume << '|'
               << output.endpoint_host << '|'
               << output.endpoint_port << '|'
               << output.format << ';';
    }
    return stream.str();
}

bool output_playable(const multiroom::OutputDevice& output) {
    return output.supports_airplay2 &&
           !output.endpoint_host.empty() &&
           output.endpoint_port != 0;
}

std::wstring output_status_label(const multiroom::OutputDevice& output) {
    if (output.endpoint_host.empty() || output.endpoint_port == 0) return L"No endpoint";
    if (output.supports_airplay2 && output.requires_encrypted_stream) return L"AP2";
    if (output.supports_airplay2) return volume_label(output.volume);
    if (output.supports_legacy_l16) return L"Legacy";
    if (output.requires_auth) return L"Auth required";
    return volume_label(output.volume);
}

COLORREF blend_color(COLORREF from, COLORREF to, int amount_percent) {
    const auto mix = [amount_percent](int a, int b) {
        return a + ((b - a) * amount_percent / 100);
    };
    return RGB(
        mix(GetRValue(from), GetRValue(to)),
        mix(GetGValue(from), GetGValue(to)),
        mix(GetBValue(from), GetBValue(to)));
}

class SpeakerPickerPopup
    : public CWindowImpl<SpeakerPickerPopup> {
public:
    DECLARE_WND_CLASS_EX(TEXT("{BB3569C2-8F93-445E-9681-E32B38CFCE62}"), CS_VREDRAW | CS_HREDRAW, -1);

    SpeakerPickerPopup(HWND owner, ui_element_instance_callback_ptr callback)
        : owner_(owner)
        , callback_(callback) {}

    void open_below(HWND anchor) {
        MultiroomComponentState::instance().refresh_outputs();
        outputs_ = MultiroomComponentState::instance().outputs();
        status_ = MultiroomComponentState::instance().status_text();
        outputs_signature_ = outputs_signature(outputs_);

        INITCOMMONCONTROLSEX cc = {};
        cc.dwSize = sizeof(cc);
        cc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&cc);

        CRect anchor_rect;
        if (anchor != nullptr && ::IsWindow(anchor)) {
            WIN32_OP_D(::GetWindowRect(anchor, &anchor_rect));
        } else {
            POINT cursor = {};
            WIN32_OP_D(::GetCursorPos(&cursor));
            anchor_rect.SetRect(cursor.x, cursor.y, cursor.x, cursor.y);
        }

        const int height = popup_height();
        CRect popup_rect(anchor_rect.left, anchor_rect.bottom + 4, anchor_rect.left + kPopupWidth, anchor_rect.bottom + 4 + height);
        keep_on_monitor(popup_rect);

        WIN32_OP(Create(
            owner_ == nullptr ? core_api::get_main_window() : owner_,
            popup_rect,
            nullptr,
            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST) != nullptr);
        SetTimer(kRefreshTimer, 250);
        ShowWindow(SW_SHOW);
        SetFocus();
    }

    BEGIN_MSG_MAP_EX(SpeakerPickerPopup)
        MESSAGE_HANDLER(WM_CREATE, on_create)
        MSG_WM_ERASEBKGND(on_erase_background)
        MSG_WM_PAINT(on_paint)
        MESSAGE_HANDLER(WM_COMMAND, on_command)
        MESSAGE_HANDLER(WM_HSCROLL, on_scroll)
        MESSAGE_HANDLER(WM_TIMER, on_timer)
        MESSAGE_HANDLER(WM_ACTIVATE, on_activate)
        MESSAGE_HANDLER(WM_KEYDOWN, on_key_down)
    END_MSG_MAP()

    void OnFinalMessage(HWND) override {
        delete this;
    }

private:
    LRESULT on_create(UINT, WPARAM, LPARAM, BOOL&) {
        rebuild_controls();
        return 0;
    }

    BOOL on_erase_background(CDCHandle dc) {
        CRect rc;
        WIN32_OP_D(GetClientRect(&rc));
        CBrush brush;
        WIN32_OP_D(brush.CreateSolidBrush(background_color()) != nullptr);
        WIN32_OP_D(dc.FillRect(&rc, brush));
        return TRUE;
    }

    void on_paint(CDCHandle) {
        CPaintDC dc(*this);
        CRect rc;
        WIN32_OP_D(GetClientRect(&rc));

        dc.SetBkMode(TRANSPARENT);
        dc.SetTextColor(text_color());
        SelectObjectScope font_scope(dc, popup_font());

        CRect header(kPopupPadding, 8, rc.right - kPopupPadding, kHeaderHeight);
        WIN32_OP_D(dc.DrawTextW(L"AirPlay", -1, &header, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX) > 0);

        CPen divider;
        WIN32_OP_D(divider.CreatePen(PS_SOLID, 1, subtle_color()) != nullptr);
        SelectObjectScope pen_scope(dc, divider);
        dc.MoveTo(kPopupPadding, kHeaderHeight);
        dc.LineTo(rc.right - kPopupPadding, kHeaderHeight);

        int row_top = kHeaderHeight + 6;
        for (size_t index = 0; index < outputs_.size(); ++index) {
            CRect row(kPopupPadding, row_top, rc.right - kPopupPadding, row_top + kRowHeight - 8);
            CBrush row_brush;
            WIN32_OP_D(row_brush.CreateSolidBrush(row_color(outputs_[index].selected)) != nullptr);
            WIN32_OP_D(dc.FillRect(&row, row_brush));

            CPen row_pen;
            WIN32_OP_D(row_pen.CreatePen(PS_SOLID, 1, subtle_color()) != nullptr);
            SelectObjectScope row_pen_scope(dc, row_pen);
            SelectObjectScope row_brush_scope(dc, GetStockObject(NULL_BRUSH));
            dc.RoundRect(&row, CPoint(8, 8));

            row_top += kRowHeight;
        }
    }

    LRESULT on_command(UINT, WPARAM wp, LPARAM, BOOL&) {
        const auto id = static_cast<UINT_PTR>(LOWORD(wp));
        const auto code = HIWORD(wp);

        if (id == kRefreshCommand && code == BN_CLICKED) {
            MultiroomComponentState::instance().refresh_outputs();
            outputs_ = MultiroomComponentState::instance().outputs();
            status_ = MultiroomComponentState::instance().status_text();
            outputs_signature_ = outputs_signature(outputs_);
            rebuild_controls();
            resize_to_content();
            Invalidate();
            if (owner_ != nullptr) ::InvalidateRect(owner_, nullptr, TRUE);
            SetTimer(kRefreshTimer, 250);
            return 0;
        }

        if (id >= kSpeakerCheckBase && id < kSpeakerCheckBase + outputs_.size() && code == BN_CLICKED) {
            const auto index = static_cast<size_t>(id - kSpeakerCheckBase);
            MultiroomComponentState::instance().toggle_output(outputs_[index].id);
            outputs_ = MultiroomComponentState::instance().outputs();
            sync_control_values();
            Invalidate();
            if (owner_ != nullptr) ::InvalidateRect(owner_, nullptr, TRUE);
            SetTimer(kRefreshTimer, 250);
            return 0;
        }

        return 0;
    }

    LRESULT on_timer(UINT, WPARAM wp, LPARAM, BOOL&) {
        if (wp != kRefreshTimer) return 0;

        const auto outputs = MultiroomComponentState::instance().outputs();
        const auto status = MultiroomComponentState::instance().status_text();
        const auto signature = outputs_signature(outputs);
        if (signature != outputs_signature_ || status != status_) {
            outputs_ = outputs;
            status_ = status;
            outputs_signature_ = signature;
            rebuild_controls();
            resize_to_content();
            Invalidate();
            if (owner_ != nullptr) ::InvalidateRect(owner_, nullptr, TRUE);
        }

        if (!MultiroomComponentState::instance().refresh_in_progress() &&
            !MultiroomComponentState::instance().control_in_progress()) {
            KillTimer(kRefreshTimer);
        }
        return 0;
    }

    LRESULT on_scroll(UINT, WPARAM, LPARAM lp, BOOL&) {
        HWND slider = reinterpret_cast<HWND>(lp);
        if (slider == nullptr) return 0;

        const int id = ::GetDlgCtrlID(slider);
        if (id < static_cast<int>(kSpeakerVolumeBase) ||
            id >= static_cast<int>(kSpeakerVolumeBase + outputs_.size())) {
            return 0;
        }

        const auto index = static_cast<size_t>(id - kSpeakerVolumeBase);
        const int volume = static_cast<int>(::SendMessageW(slider, TBM_GETPOS, 0, 0));
        MultiroomComponentState::instance().set_output_volume(outputs_[index].id, volume);
        outputs_[index].volume = volume;

        if (index < volume_labels_.size() && volume_labels_[index] != nullptr) {
            const auto text = volume_label(volume);
            ::SetWindowTextW(volume_labels_[index], text.c_str());
        }

        if (owner_ != nullptr) ::InvalidateRect(owner_, nullptr, TRUE);
        return 0;
    }

    LRESULT on_activate(UINT, WPARAM wp, LPARAM, BOOL&) {
        if (LOWORD(wp) == WA_INACTIVE) {
            DestroyWindow();
        }
        return 0;
    }

    LRESULT on_key_down(UINT, WPARAM wp, LPARAM, BOOL&) {
        if (wp == VK_ESCAPE) {
            DestroyWindow();
            return 0;
        }
        return 0;
    }

    void rebuild_controls() {
        for (HWND control : controls_) {
            if (control != nullptr && ::IsWindow(control)) {
                ::DestroyWindow(control);
            }
        }
        controls_.clear();
        volume_labels_.clear();

        if (outputs_.empty()) {
            add_control(::CreateWindowExW(
                0,
                L"STATIC",
                status_.empty() ? L"No AirPlay speakers found" : status_.c_str(),
                WS_CHILD | WS_VISIBLE | SS_CENTER | SS_EDITCONTROL,
                kPopupPadding,
                kHeaderHeight + 12,
                kPopupWidth - (kPopupPadding * 2),
                42,
                m_hWnd,
                reinterpret_cast<HMENU>(kEmptyStatusId),
                core_api::get_my_instance(),
                nullptr));
        } else {
            int row_top = kHeaderHeight + 10;
            for (size_t index = 0; index < outputs_.size(); ++index) {
                create_speaker_row(index, row_top);
                row_top += kRowHeight;
            }
        }

        add_control(::CreateWindowExW(
            0,
            L"BUTTON",
            L"Refresh",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            kPopupWidth - kPopupPadding - 88,
            popup_height() - 32,
            88,
            24,
            m_hWnd,
            reinterpret_cast<HMENU>(kRefreshCommand),
            core_api::get_my_instance(),
            nullptr));

        sync_control_values();
    }

    void create_speaker_row(size_t index, int row_top) {
        const auto& output = outputs_[index];
        const auto name = widen_utf8(output.name.empty() ? output.id : output.name);
        const bool playable = output_playable(output);

        HWND check = add_control(::CreateWindowExW(
            0,
            L"BUTTON",
            name.c_str(),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | (playable ? 0 : WS_DISABLED),
            kPopupPadding + 12,
            row_top + 4,
            kPopupWidth - 128,
            22,
            m_hWnd,
            reinterpret_cast<HMENU>(kSpeakerCheckBase + index),
            core_api::get_my_instance(),
            nullptr));

        HWND slider = add_control(::CreateWindowExW(
            0,
            TRACKBAR_CLASSW,
            nullptr,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS | (playable ? 0 : WS_DISABLED),
            kPopupPadding + 30,
            row_top + 28,
            kPopupWidth - 176,
            24,
            m_hWnd,
            reinterpret_cast<HMENU>(kSpeakerVolumeBase + index),
            core_api::get_my_instance(),
            nullptr));

        HWND label = add_control(::CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            kPopupWidth - kPopupPadding - 104,
            row_top + 30,
            92,
            20,
            m_hWnd,
            reinterpret_cast<HMENU>(kVolumeLabelBase + index),
            core_api::get_my_instance(),
            nullptr));

        volume_labels_.push_back(label);

        ::SendMessageW(check, BM_SETCHECK, output.selected ? BST_CHECKED : BST_UNCHECKED, 0);
        ::SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        ::SendMessageW(slider, TBM_SETPAGESIZE, 0, 10);
        ::SendMessageW(slider, TBM_SETPOS, TRUE, output.volume);
        const auto text = output_status_label(output);
        ::SetWindowTextW(label, text.c_str());
    }

    HWND add_control(HWND control) {
        WIN32_OP_D(control != nullptr);
        controls_.push_back(control);
        return control;
    }

    void sync_control_values() {
        for (size_t index = 0; index < outputs_.size(); ++index) {
            if (HWND check = ::GetDlgItem(m_hWnd, static_cast<int>(kSpeakerCheckBase + index))) {
                ::SendMessageW(check, BM_SETCHECK, outputs_[index].selected ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            if (HWND slider = ::GetDlgItem(m_hWnd, static_cast<int>(kSpeakerVolumeBase + index))) {
                ::SendMessageW(slider, TBM_SETPOS, TRUE, outputs_[index].volume);
            }
            if (index < volume_labels_.size() && volume_labels_[index] != nullptr) {
                const auto text = output_status_label(outputs_[index]);
                ::SetWindowTextW(volume_labels_[index], text.c_str());
            }
        }
    }

    int popup_height() const {
        const int body = outputs_.empty() ? 62 : static_cast<int>(outputs_.size()) * kRowHeight + 8;
        return kHeaderHeight + body + kFooterHeight;
    }

    void resize_to_content() {
        CRect rc;
        WIN32_OP_D(GetWindowRect(&rc));
        rc.bottom = rc.top + popup_height();
        keep_on_monitor(rc);
        WIN32_OP_D(::SetWindowPos(m_hWnd, HWND_TOPMOST, rc.left, rc.top, rc.Width(), rc.Height(), SWP_NOACTIVATE));
    }

    static void keep_on_monitor(CRect& rc) {
        HMONITOR monitor = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) return;

        const int width = rc.Width();
        const int height = rc.Height();
        if (rc.right > info.rcWork.right) {
            rc.left = info.rcWork.right - width;
            rc.right = info.rcWork.right;
        }
        if (rc.bottom > info.rcWork.bottom) {
            rc.top = info.rcWork.bottom - height;
            rc.bottom = info.rcWork.bottom;
        }
        if (rc.left < info.rcWork.left) {
            rc.left = info.rcWork.left;
            rc.right = rc.left + width;
        }
        if (rc.top < info.rcWork.top) {
            rc.top = info.rcWork.top;
            rc.bottom = rc.top + height;
        }
    }

    COLORREF background_color() const {
        return callback_.is_valid() ? callback_->query_std_color(ui_color_background) : GetSysColor(COLOR_WINDOW);
    }

    COLORREF text_color() const {
        return callback_.is_valid() ? callback_->query_std_color(ui_color_text) : GetSysColor(COLOR_WINDOWTEXT);
    }

    HGDIOBJ popup_font() const {
        return callback_.is_valid() ? reinterpret_cast<HGDIOBJ>(callback_->query_font_ex(ui_font_default)) : GetStockObject(DEFAULT_GUI_FONT);
    }

    COLORREF subtle_color() const {
        return blend_color(background_color(), text_color(), 18);
    }

    COLORREF row_color(bool selected) const {
        return blend_color(background_color(), text_color(), selected ? 12 : 6);
    }

    HWND owner_ = nullptr;
    ui_element_instance_callback_ptr callback_;
    std::vector<multiroom::OutputDevice> outputs_;
    std::wstring status_;
    std::string outputs_signature_;
    std::vector<HWND> controls_;
    std::vector<HWND> volume_labels_;
};

class SpeakerSelectorElement
    : public ui_element_instance
    , public CWindowImpl<SpeakerSelectorElement> {
public:
    DECLARE_WND_CLASS_EX(TEXT("{4F175C3D-7D32-4BB3-A724-4A24472343C3}"), CS_VREDRAW | CS_HREDRAW, -1);

    SpeakerSelectorElement(ui_element_config::ptr config, ui_element_instance_callback_ptr callback)
        : m_callback(callback)
        , config_(config) {}

    void initialize_window(HWND parent) {
        WIN32_OP(Create(parent, nullptr, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN) != nullptr);
    }

    HWND get_wnd() override {
        return *this;
    }

    void set_configuration(ui_element_config::ptr config) override {
        config_ = config;
    }

    ui_element_config::ptr get_configuration() override {
        return config_;
    }

    static GUID g_get_guid() {
        return guid_speaker_selector_element;
    }

    static GUID g_get_subclass() {
        return ui_element_subclass_utility;
    }

    static void g_get_name(pfc::string_base& out) {
        out = "AirPlay Speaker Selector Toolbar";
    }

    static ui_element_config::ptr g_get_default_configuration() {
        return ui_element_config::g_create_empty(g_get_guid());
    }

    static const char* g_get_description() {
        return "Compact AirPlay speaker picker intended for placement next to the seekbar or playback controls.";
    }

    ui_element_min_max_info get_min_max_info() override {
        ui_element_min_max_info info;
        info.m_min_width = 120;
        info.m_min_height = kToolbarHeight;
        info.m_max_width = 220;
        info.m_max_height = kToolbarHeight;
        return info;
    }

    void notify(const GUID& what, t_size, const void*, t_size) override {
        if (what == ui_element_notify_colors_changed || what == ui_element_notify_font_changed) {
            Invalidate();
        }
    }

    BEGIN_MSG_MAP_EX(SpeakerSelectorElement)
        MSG_WM_ERASEBKGND(on_erase_background)
        MSG_WM_PAINT(on_paint)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, on_left_button_down)
        MESSAGE_HANDLER(WM_LBUTTONUP, on_left_button_up)
        MESSAGE_HANDLER(WM_KEYDOWN, on_key_down)
        MESSAGE_HANDLER(WM_SETFOCUS, on_focus_changed)
        MESSAGE_HANDLER(WM_KILLFOCUS, on_focus_changed)
    END_MSG_MAP()

protected:
    const ui_element_instance_callback_ptr m_callback;

private:
    BOOL on_erase_background(CDCHandle dc) {
        CRect rc;
        WIN32_OP_D(GetClientRect(&rc));
        CBrush brush;
        WIN32_OP_D(brush.CreateSolidBrush(m_callback->query_std_color(ui_color_background)) != nullptr);
        WIN32_OP_D(dc.FillRect(&rc, brush));
        return TRUE;
    }

    void on_paint(CDCHandle) {
        CPaintDC dc(*this);
        CRect rc;
        WIN32_OP_D(GetClientRect(&rc));

        const COLORREF background = m_callback->query_std_color(ui_color_background);
        const COLORREF text = m_callback->query_std_color(ui_color_text);
        const COLORREF border = blend_color(background, text, 35);

        CBrush background_brush;
        WIN32_OP_D(background_brush.CreateSolidBrush(background) != nullptr);
        WIN32_OP_D(dc.FillRect(&rc, background_brush));

        CRect button = rc;
        button.DeflateRect(1, 1);
        CPen border_pen;
        WIN32_OP_D(border_pen.CreatePen(PS_SOLID, 1, border) != nullptr);
        SelectObjectScope pen_scope(dc, border_pen);
        CBrush button_brush;
        WIN32_OP_D(button_brush.CreateSolidBrush(blend_color(background, text, 8)) != nullptr);
        SelectObjectScope brush_scope(dc, button_brush);
        WIN32_OP_D(dc.RoundRect(&button, CPoint(5, 5)));

        dc.SetTextColor(text);
        dc.SetBkMode(TRANSPARENT);
        SelectObjectScope font_scope(dc, reinterpret_cast<HGDIOBJ>(m_callback->query_font_ex(ui_font_default)));

        CRect text_rect = button;
        text_rect.DeflateRect(9, 1, 22, 1);
        const auto label = MultiroomComponentState::instance().selected_label();
        const auto display = label == L"No speakers" ? L"AirPlay" : label;
        WIN32_OP_D(dc.DrawTextW(display.c_str(), -1, &text_rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) > 0);

        CRect arrow_rect = button;
        arrow_rect.left = arrow_rect.right - 18;
        WIN32_OP_D(dc.DrawTextW(L"\x25BE", -1, &arrow_rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX) > 0);

        if (GetFocus() == m_hWnd) {
            CRect focus = button;
            focus.DeflateRect(3, 3);
            dc.DrawFocusRect(&focus);
        }
    }

    LRESULT on_left_button_up(UINT, WPARAM, LPARAM, BOOL&) {
        open_picker();
        return 0;
    }

    LRESULT on_left_button_down(UINT, WPARAM, LPARAM, BOOL&) {
        SetFocus();
        return 0;
    }

    LRESULT on_key_down(UINT, WPARAM wp, LPARAM, BOOL&) {
        if (wp == VK_SPACE || wp == VK_RETURN) {
            open_picker();
            return 0;
        }
        return 0;
    }

    LRESULT on_focus_changed(UINT, WPARAM, LPARAM, BOOL&) {
        Invalidate();
        return 0;
    }

    void open_picker() {
        auto* popup = new SpeakerPickerPopup(m_hWnd, m_callback);
        popup->open_below(m_hWnd);
    }

    ui_element_config::ptr config_;
};

static service_factory_single_t<ui_element_impl<SpeakerSelectorElement>> g_speaker_selector_factory;

}  // namespace

void show_multiroom_speaker_picker(HWND owner, HWND anchor) {
    auto* popup = new SpeakerPickerPopup(owner, nullptr);
    popup->open_below(anchor);
}

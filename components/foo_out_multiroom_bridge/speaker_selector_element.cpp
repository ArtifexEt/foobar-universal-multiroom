#include "stdafx.h"
#include "multiroom_component_state.h"
#include "product_identity.h"
#include "speaker_picker_layout.h"
#include "speaker_selector_popup.h"

#include <commctrl.h>
#include <libPPUI/win32_op.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <iterator>

namespace {

static constexpr GUID guid_speaker_selector_element = {
    0x4f175c3d, 0x7d32, 0x4bb3, {0xa7, 0x24, 0x4a, 0x24, 0x47, 0x23, 0x43, 0xc3}};
static constexpr UINT_PTR kSpeakerCheckBase = 30000;
static constexpr UINT_PTR kSpeakerVolumeBase = 31000;
static constexpr UINT_PTR kRefreshCommand = 32000;
static constexpr UINT_PTR kEmptyStatusId = 32001;
static constexpr UINT_PTR kPairPinEditId = 32002;
static constexpr UINT_PTR kPairCommand = 32003;
static constexpr UINT_PTR kPairPinLabelId = 32004;
static constexpr UINT_PTR kCloseCommand = 32005;
static constexpr UINT_PTR kGroupLabelId = 32006;
static constexpr UINT_PTR kVolumeLabelBase = 33000;
static constexpr UINT_PTR kRefreshTimer = 34000;
static constexpr UINT_PTR kToolbarRefreshTimer = 34001;
static constexpr UINT_PTR kGroupComboId = 35000;
static constexpr UINT_PTR kSpeakerOffsetBase = 36000;
static constexpr int kPopupPadding = 22;
static constexpr int kHeaderHeight = 110;
static constexpr int kSectionHeight = 36;
static constexpr int kFooterHeight = 56;
static constexpr int kPairingFooterHeight = 104;
static constexpr int kBodyFontHeight = 16;
static constexpr UINT kPickerKeyMessage = WM_APP + 1;
static constexpr int kToolbarMinHeight = 22;
static constexpr int kToolbarMaxHeight = 34;

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

std::wstring text_from_window(HWND wnd) {
    if (wnd == nullptr) return {};
    const int length = ::GetWindowTextLengthW(wnd);
    if (length <= 0) return {};

    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(wnd, result.data(), length + 1);
    result.resize(static_cast<size_t>(std::max(copied, 0)));
    return result;
}

std::string narrow_pin(const std::wstring& text) {
    std::string pin;
    for (wchar_t ch : text) {
        if (std::iswspace(ch) != 0) {
            continue;
        }
        if (ch < L'0' || ch > L'9') {
            return {};
        }
        pin.push_back(static_cast<char>('0' + (ch - L'0')));
    }
    return pin;
}

std::string groups_signature(const std::vector<multiroom::SpeakerGroup>& groups) {
    return multiroom::serialize_speaker_groups(groups);
}

bool output_playable(const multiroom::OutputDevice& output) {
    return output.supports_airplay2 &&
           !output.endpoint_host.empty() &&
           output.endpoint_port != 0;
}

std::wstring output_status_label(const multiroom::OutputDevice& output) {
    if (output.endpoint_host.empty() || output.endpoint_port == 0) return L"No network address. Refresh Speakers & TVs.";
    if (!output.playback_error.empty()) return widen_utf8(output.playback_error);
    if (output.playback_ready) {
        if (output.receiver_volume_percent && *output.receiver_volume_percent != output.volume) {
            return L"Receiver volume " + volume_label(*output.receiver_volume_percent);
        }
        return L"Connected";
    }
    if (output.supports_airplay2) return {};
    if (output.supports_legacy_l16) return L"Legacy AirPlay only";
    if (output.requires_auth) return L"Unsupported AirPlay authentication";
    return L"AirPlay 2 unavailable";
}

bool same_speaker_rows(
    const std::vector<multiroom::OutputDevice>& current,
    const std::vector<multiroom::OutputDevice>& updated) {
    return std::equal(current.begin(), current.end(), updated.begin(), updated.end(),
        [](const auto& left, const auto& right) {
            return left.id == right.id && left.name == right.name &&
                left.has_password == right.has_password && output_playable(left) == output_playable(right);
        });
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

bool color_is_dark(COLORREF color) {
    return GetRValue(color) * 299 + GetGValue(color) * 587 + GetBValue(color) * 114 < 128000;
}

COLORREF airplay_accent(COLORREF background) {
    return color_is_dark(background) ? RGB(10, 132, 255) : RGB(0, 122, 255);
}

void draw_airplay_audio_glyph(HDC hdc, CPoint center, COLORREF color, int radius) {
    CDCHandle dc(hdc);
    CPen pen;
    WIN32_OP_D(pen.CreatePen(PS_SOLID, (std::max)(1, radius / 5), color) != nullptr);
    SelectObjectScope pen_scope(dc, pen);
    SelectObjectScope brush_scope(dc, GetStockObject(NULL_BRUSH));

    for (int ring = 1; ring <= 3; ++ring) {
        const double ring_radius = static_cast<double>(radius) * ring / 3.0;
        POINT points[17] = {};
        for (size_t point = 0; point < std::size(points); ++point) {
            const double angle = (205.0 + (130.0 * point / (std::size(points) - 1))) * 3.141592653589793 / 180.0;
            points[point].x = center.x + static_cast<LONG>(std::lround(std::cos(angle) * ring_radius));
            points[point].y = center.y + static_cast<LONG>(std::lround(std::sin(angle) * ring_radius));
        }
        WIN32_OP_D(dc.Polyline(points, static_cast<int>(std::size(points))));
    }

    CBrush triangle_brush;
    WIN32_OP_D(triangle_brush.CreateSolidBrush(color) != nullptr);
    SelectObjectScope triangle_scope(dc, triangle_brush);
    POINT triangle[] = {
        {center.x, center.y - 1},
        {center.x - radius / 2, center.y + radius},
        {center.x + radius / 2, center.y + radius},
    };
    WIN32_OP_D(dc.Polygon(triangle, static_cast<int>(std::size(triangle))));
}

void draw_speaker_glyph(HDC hdc, CPoint center, COLORREF color, int unit) {
    CDCHandle dc(hdc);
    CPen outline;
    WIN32_OP_D(outline.CreatePen(PS_SOLID, 1, color) != nullptr);
    SelectObjectScope pen_scope(dc, outline);
    SelectObjectScope brush_scope(dc, GetStockObject(NULL_BRUSH));

    CRect cabinet(center.x - 4 * unit, center.y - 6 * unit, center.x + 4 * unit, center.y + 6 * unit);
    WIN32_OP_D(dc.RoundRect(&cabinet, CPoint(2 * unit, 2 * unit)));

    CBrush cone;
    WIN32_OP_D(cone.CreateSolidBrush(color) != nullptr);
    SelectObjectScope cone_scope(dc, cone);
    SelectObjectScope cone_pen_scope(dc, GetStockObject(NULL_PEN));
    CRect tweeter(center.x - unit, cabinet.top + 2 * unit, center.x + unit, cabinet.top + 4 * unit);
    dc.Ellipse(&tweeter);
    CRect woofer(center.x - 2 * unit, cabinet.bottom - 5 * unit, center.x + 2 * unit, cabinet.bottom - unit);
    dc.Ellipse(&woofer);
}

UINT picker_dpi(HWND window) {
    using GetWindowDpi = UINT(WINAPI*)(HWND);
    static const auto get_dpi = reinterpret_cast<GetWindowDpi>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (get_dpi != nullptr && window != nullptr) {
        if (const auto dpi = get_dpi(window); dpi != 0) return dpi;
    }
    const HDC dc = ::GetDC(window);
    if (dc == nullptr) return USER_DEFAULT_SCREEN_DPI;
    const int dpi = ::GetDeviceCaps(dc, LOGPIXELSY);
    ::ReleaseDC(window, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : USER_DEFAULT_SCREEN_DPI;
}

int picker_scrollbar_width(UINT dpi) {
    using GetMetricForDpi = int(WINAPI*)(int, UINT);
    static const auto get_metric = reinterpret_cast<GetMetricForDpi>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    return get_metric == nullptr ? ::GetSystemMetrics(SM_CXVSCROLL) : get_metric(SM_CXVSCROLL, dpi);
}

class SpeakerPickerPopup
    : public CWindowImpl<SpeakerPickerPopup> {
public:
    DECLARE_WND_CLASS_EX(
        TEXT("{BB3569C2-8F93-445E-9681-E32B38CFCE62}"),
        CS_VREDRAW | CS_HREDRAW | CS_DROPSHADOW,
        -1);

    SpeakerPickerPopup(HWND owner, ui_element_instance_callback_ptr callback)
        : owner_(owner)
        , callback_(callback) {}

    void open_below(HWND anchor) {
        auto& state = MultiroomComponentState::instance();
        outputs_ = state.outputs();
        groups_ = state.speaker_groups();
        // Opening a view must be read-only for an active stream. Populate an
        // empty cache asynchronously only while stopped; the explicit Refresh
        // action is deferred by component state if playback is in progress.
        if (outputs_.empty() && !state.playback_active()) {
            state.refresh_outputs();
        }
        if (outputs_.empty()) status_ = state.status_text();
        groups_signature_ = groups_signature(groups_);

        INITCOMMONCONTROLSEX cc = {};
        cc.dwSize = sizeof(cc);
        cc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&cc);

        opener_ = ::GetFocus();
        anchor_ = anchor != nullptr && ::IsWindow(anchor) ? anchor : nullptr;
        const HWND reference = anchor_ != nullptr ? anchor_ : owner_;
        if (reference != nullptr && ::IsWindow(reference)) {
            WIN32_OP_D(::GetWindowRect(reference, &anchor_rect_));
        } else {
            POINT cursor = {};
            WIN32_OP_D(::GetCursorPos(&cursor));
            anchor_rect_.SetRect(cursor.x, cursor.y, cursor.x, cursor.y);
        }

        font_template_dpi_ = picker_dpi(reference);
        if (::GetObjectW(host_font(), sizeof(font_template_), &font_template_) != sizeof(font_template_)) {
            wcscpy_s(font_template_.lfFaceName, L"Segoe UI");
        }
        configure_layout(anchor_rect_, font_template_dpi_);

        const int height = popup_height();
        CPoint position(anchor_rect_.CenterPoint().x - layout_.width() / 2, anchor_rect_.CenterPoint().y - height / 2);
        if (anchor_ != nullptr) {
            position.x = anchor_rect_.left;
            position.y = anchor_rect_.bottom + px(6);
            if (position.y + height > work_area_.bottom - px(multiroom::ui::SpeakerPickerLayout::margin_dip) &&
                anchor_rect_.top - height - px(6) >= work_area_.top) {
                position.y = anchor_rect_.top - height - px(6);
            }
        }

        WIN32_OP(Create(
            owner_ == nullptr ? core_api::get_main_window() : owner_,
            rectangle_at(position),
            L"Wireless Speakers",
            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL,
            WS_EX_TOOLWINDOW) != nullptr);
        apply_rounded_region();
        SetTimer(kRefreshTimer, 250);
        ShowWindow(SW_SHOW);
        move_keyboard_focus(0, false);
    }

    BEGIN_MSG_MAP_EX(SpeakerPickerPopup)
        MESSAGE_HANDLER(WM_CREATE, on_create)
        MSG_WM_ERASEBKGND(on_erase_background)
        MSG_WM_PAINT(on_paint)
        MESSAGE_HANDLER(WM_COMMAND, on_command)
        MESSAGE_HANDLER(WM_DRAWITEM, on_draw_item)
        MESSAGE_HANDLER(WM_CTLCOLORSTATIC, on_static_color)
        MESSAGE_HANDLER(WM_CTLCOLOREDIT, on_edit_color)
        MESSAGE_HANDLER(WM_NOTIFY, on_notify)
        MESSAGE_HANDLER(WM_HSCROLL, on_scroll)
        MESSAGE_HANDLER(WM_VSCROLL, on_vertical_scroll)
        MESSAGE_HANDLER(WM_MOUSEWHEEL, on_mouse_wheel)
        MESSAGE_HANDLER(WM_TIMER, on_timer)
        MESSAGE_HANDLER(WM_ACTIVATE, on_activate)
        MESSAGE_HANDLER(WM_KEYDOWN, on_key_down)
        MESSAGE_HANDLER(WM_DPICHANGED, on_dpi_changed)
        MESSAGE_HANDLER(WM_SETTINGCHANGE, on_setting_changed)
        MESSAGE_HANDLER(kPickerKeyMessage, on_picker_key)
    END_MSG_MAP()

    void OnFinalMessage(HWND) override {
        delete this;
    }

private:
    LRESULT on_create(UINT, WPARAM, LPARAM, BOOL&) {
        update_theme();
        update_fonts();
        rebuild_controls();
        return 0;
    }

    void configure_layout(const RECT& near, UINT dpi) {
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        if (::GetMonitorInfoW(::MonitorFromRect(&near, MONITOR_DEFAULTTONEAREST), &info)) {
            work_area_ = info.rcWork;
        } else {
            ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area_, 0);
        }
        dpi_ = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
        const auto dpi_scale = ::MulDiv(dpi_, 100, USER_DEFAULT_SCREEN_DPI);
        const auto host_font_height = ::MulDiv(std::abs(font_template_.lfHeight), dpi_, font_template_dpi_);
        const auto requested_scale = (std::max)(dpi_scale, ::MulDiv(host_font_height, 100, kBodyFontHeight));
        layout_ = {requested_scale, work_area_.Width(), work_area_.Height()};

        // Even with the pairing footer, leave room for at least one whole row.
        const int fixed = kHeaderHeight + kSectionHeight + kPairingFooterHeight + 8 +
            2 * multiroom::ui::SpeakerPickerLayout::margin_dip;
        const int row = layout_.stack_controls(layout_width()) ? 154 : 116;
        layout_.scale_percent = std::clamp(
            (std::min)(layout_.work_height * 100 / (fixed + row), layout_.work_width * 100 / 312),
            1, requested_scale);
        while (layout_.scale_percent > 1 &&
               layout_.visible_rows(1, list_top() + px(8 + kPairingFooterHeight), layout_width()) == 0) {
            --layout_.scale_percent;
        }
    }

    int px(int dip) const { return layout_.px(dip); }
    int header_height() const { return px(kHeaderHeight); }
    int list_top() const { return px(kHeaderHeight + kSectionHeight); }
    int layout_width() const {
        return (std::max)(1, layout_.width() - picker_scrollbar_width(dpi_));
    }
    int row_height() const { return layout_.row_height(layout_width()); }

    CRect rectangle_at(CPoint point) const {
        const int margin = px(multiroom::ui::SpeakerPickerLayout::margin_dip);
        const int width = layout_.width();
        const int height = popup_height();
        const auto left = work_area_.left + margin;
        const auto top = work_area_.top + margin;
        point.x = std::clamp(point.x, left, (std::max)(left, work_area_.right - margin - width));
        point.y = std::clamp(point.y, top, (std::max)(top, work_area_.bottom - margin - height));
        return CRect(point.x, point.y, point.x + width, point.y + height);
    }

    void update_fonts() {
        const auto create_font = [&](CFont& font, int height, LONG weight) {
            LOGFONTW spec = font_template_;
            spec.lfHeight = -px(height);
            spec.lfWidth = 0;
            spec.lfWeight = weight;
            CFont replacement;
            if (replacement.CreateFontIndirect(&spec) != nullptr) {
                // Apply its handle to surviving controls before deleting the old font.
                if (&font == &body_font_) {
                    for (auto control : controls_) ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(replacement.m_hFont), FALSE);
                    for (auto control : speaker_controls_) ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(replacement.m_hFont), FALSE);
                }
                if (font.m_hFont != nullptr) font.DeleteObject();
                font.Attach(replacement.Detach());
            }
        };
        create_font(body_font_, kBodyFontHeight, FW_NORMAL);
        create_font(title_font_, 23, FW_SEMIBOLD);
        create_font(section_font_, 17, FW_SEMIBOLD);
    }

    BOOL on_erase_background(CDCHandle) {
        // WM_PAINT renders the whole popup background. Suppressing the separate
        // erase pass avoids visible blank patches while child controls are
        // rebuilt during scrolling and volume updates.
        return TRUE;
    }

    void on_paint(CDCHandle) {
        CPaintDC dc(*this);
        CRect rc;
        WIN32_OP_D(GetClientRect(&rc));

        CBrush background;
        WIN32_OP_D(background.CreateSolidBrush(background_color()) != nullptr);
        WIN32_OP_D(dc.FillRect(&rc, background));
        dc.SetBkMode(TRANSPARENT);
        dc.SetTextColor(text_color());
        SelectObjectScope font_scope(dc, popup_font());

        CRect title(px(kPopupPadding), 0, rc.right - px(kPopupPadding + 52), px(52));
        {
            SelectObjectScope title_scope(dc, title_font_.m_hFont != nullptr ? title_font_.m_hFont : popup_font());
            WIN32_OP_D(dc.DrawTextW(L"Wireless Speakers", -1, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) > 0);
        }

        SelectObjectScope body_font_scope(dc, popup_font());
        dc.SetTextColor(secondary_text_color());

        if (!pairing_message_.empty()) {
            auto message_rect = pairing_message_rect();
            WIN32_OP_D(dc.DrawTextW(pairing_message_.c_str(), -1, &message_rect,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) > 0);
        }

        CPen divider;
        WIN32_OP_D(divider.CreatePen(PS_SOLID, 1, subtle_color()) != nullptr);
        SelectObjectScope pen_scope(dc, divider);
        dc.MoveTo(0, header_height() - 1);
        dc.LineTo(rc.right, header_height() - 1);

        CRect section(px(kPopupPadding), header_height(), rc.right - px(kPopupPadding), list_top());
        {
            SelectObjectScope section_scope(dc, section_font_.m_hFont != nullptr ? section_font_.m_hFont : popup_font());
            WIN32_OP_D(dc.DrawTextW(L"Speakers & TVs", -1, &section, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX) > 0);
        }

        const auto summary = selection_summary();
        CRect count_rect(rc.Width() / 2, header_height(), rc.right - px(kPopupPadding), list_top());
        WIN32_OP_D(dc.DrawTextW(summary.c_str(), -1, &count_rect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) > 0);

        int row_top = list_top();
        const auto row_end = (std::min)(outputs_.size(), first_visible_row_ + visible_row_count());
        for (size_t index = first_visible_row_; index < row_end; ++index) {
            dc.MoveTo(px(kPopupPadding), row_top + row_height() - 1);
            dc.LineTo(rc.right - px(kPopupPadding), row_top + row_height() - 1);
            row_top += row_height();
        }

        const int footer_top = popup_height() - footer_height();
        CRect footer(0, footer_top, rc.right, rc.bottom);
        CBrush footer_brush;
        WIN32_OP_D(footer_brush.CreateSolidBrush(footer_color()) != nullptr);
        WIN32_OP_D(dc.FillRect(&footer, footer_brush));

        CPen footer_divider;
        WIN32_OP_D(footer_divider.CreatePen(PS_SOLID, 1, subtle_color()) != nullptr);
        SelectObjectScope footer_pen_scope(dc, footer_divider);
        dc.MoveTo(0, footer_top);
        dc.LineTo(rc.right, footer_top);
    }

    LRESULT on_static_color(UINT, WPARAM wp, LPARAM lp, BOOL&) {
        HDC hdc = reinterpret_cast<HDC>(wp);
        HWND control = reinterpret_cast<HWND>(lp);
        ::SetBkMode(hdc, TRANSPARENT);
        ::SetTextColor(hdc, secondary_text_color());
        if (control != nullptr && ::GetDlgCtrlID(control) == static_cast<int>(kPairPinLabelId)) {
            return reinterpret_cast<LRESULT>(footer_brush_.m_hBrush);
        }
        return reinterpret_cast<LRESULT>(background_brush_.m_hBrush);
    }

    LRESULT on_edit_color(UINT, WPARAM wp, LPARAM, BOOL&) {
        HDC hdc = reinterpret_cast<HDC>(wp);
        ::SetBkColor(hdc, control_background_color());
        ::SetTextColor(hdc, text_color());
        return reinterpret_cast<LRESULT>(control_background_brush_.m_hBrush);
    }

    LRESULT on_draw_item(UINT, WPARAM, LPARAM lp, BOOL&) {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (item == nullptr || item->CtlType != ODT_BUTTON) return FALSE;

        CDCHandle dc(item->hDC);
        CRect rc(item->rcItem);
        dc.SetBkMode(TRANSPARENT);
        SelectObjectScope font_scope(dc, popup_font());

        if (item->CtlID >= kSpeakerCheckBase && item->CtlID < kSpeakerCheckBase + outputs_.size()) {
            const auto index = static_cast<size_t>(item->CtlID - kSpeakerCheckBase);
            const auto& output = outputs_[index];
            const bool enabled = (item->itemState & ODS_DISABLED) == 0;
            const auto foreground = enabled ? text_color() : secondary_text_color();

            CBrush background;
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            WIN32_OP_D(background.CreateSolidBrush(pressed ? pressed_row_color() : background_color()) != nullptr);
            WIN32_OP_D(dc.FillRect(&rc, background));

            const int center_y = rc.CenterPoint().y;
            draw_speaker_glyph(dc.m_hDC, CPoint(rc.left + px(18), center_y), foreground, (std::max)(1, px(2)));

            CRect indicator(rc.right - px(32), center_y - px(11), rc.right - px(10), center_y + px(11));
            CPen ring;
            WIN32_OP_D(ring.CreatePen(PS_SOLID, px(2), output.selected ? accent_color() : secondary_text_color()) != nullptr);
            SelectObjectScope ring_scope(dc, ring);
            CBrush indicator_brush;
            WIN32_OP_D(indicator_brush.CreateSolidBrush(output.selected ? accent_color() : background_color()) != nullptr);
            SelectObjectScope indicator_scope(dc, indicator_brush);
            dc.Ellipse(&indicator);

            if (output.selected) {
                CPen check;
                WIN32_OP_D(check.CreatePen(PS_SOLID, px(2), high_contrast_ ? ::GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(255, 255, 255)) != nullptr);
                SelectObjectScope check_scope(dc, check);
                dc.MoveTo(indicator.left + px(5), center_y);
                dc.LineTo(indicator.left + px(9), center_y + px(4));
                dc.LineTo(indicator.left + px(17), center_y - px(5));
            }

            const auto name = widen_utf8(output.name.empty() ? output.id : output.name);
            dc.SetTextColor(foreground);
            CRect name_rect(rc.left + px(42), rc.top, rc.right - px(44), rc.bottom);
            const auto status = output_status_label(output);
            if (!status.empty()) {
                CRect status_rect(name_rect);
                name_rect.bottom = center_y;
                status_rect.top = center_y;
                dc.SetTextColor(secondary_text_color());
                WIN32_OP_D(dc.DrawTextW(status.c_str(), -1, &status_rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) > 0);
                dc.SetTextColor(foreground);
            }
            SelectObjectScope name_font(dc, section_font_.m_hFont != nullptr ? section_font_.m_hFont : popup_font());
            WIN32_OP_D(dc.DrawTextW(name.c_str(), -1, &name_rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) > 0);
        } else if (item->CtlID == kRefreshCommand) {
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            CBrush button_brush;
            WIN32_OP_D(button_brush.CreateSolidBrush(pressed ? blend_color(footer_color(), accent_color(), 18) : footer_color()) != nullptr);
            WIN32_OP_D(dc.FillRect(&rc, button_brush));
            dc.SetTextColor(text_color());
            SelectObjectScope title_scope(dc, section_font_.m_hFont != nullptr ? section_font_.m_hFont : popup_font());
            WIN32_OP_D(dc.DrawTextW(L"Refresh Speakers & TVs", -1, &rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX) > 0);
        } else if (item->CtlID == kPairCommand || item->CtlID == kCloseCommand) {
            wchar_t text[64] = {};
            ::GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            CBrush button_brush;
            WIN32_OP_D(button_brush.CreateSolidBrush(
                pressed ? blend_color(background_color(), accent_color(), 25) : blend_color(background_color(), text_color(), 8)) != nullptr);
            SelectObjectScope button_scope(dc, button_brush);
            SelectObjectScope pen_scope(dc, GetStockObject(NULL_PEN));
            dc.RoundRect(&rc, CPoint(px(10), px(10)));
            dc.SetTextColor(text_color());
            if (item->CtlID == kCloseCommand) wcscpy_s(text, L"\x00d7");
            WIN32_OP_D(dc.DrawTextW(text, -1, &rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX) > 0);
        }

        if ((item->itemState & ODS_FOCUS) != 0) {
            CRect focus = rc;
            focus.DeflateRect(px(3), px(3));
            dc.DrawFocusRect(&focus);
        }
        return TRUE;
    }

    LRESULT on_notify(UINT, WPARAM, LPARAM lp, BOOL&) {
        auto* header = reinterpret_cast<NMHDR*>(lp);
        if (header == nullptr || header->code != NM_CUSTOMDRAW) return 0;
        if (header->idFrom < kSpeakerVolumeBase || header->idFrom >= kSpeakerVolumeBase + outputs_.size()) return 0;

        auto* custom = reinterpret_cast<NMCUSTOMDRAW*>(lp);
        if (custom->dwDrawStage == CDDS_PREPAINT) {
            CBrush background;
            WIN32_OP_D(background.CreateSolidBrush(background_color()) != nullptr);
            ::FillRect(custom->hdc, &custom->rc, background);
            return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
        }
        if (custom->dwDrawStage == CDDS_POSTPAINT && ::GetFocus() == header->hwndFrom) {
            CRect focus(custom->rc);
            focus.DeflateRect(px(2), px(2));
            ::DrawFocusRect(custom->hdc, &focus);
        }
        if (custom->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;

        CDCHandle dc(custom->hdc);
        if (custom->dwItemSpec == TBCD_CHANNEL) {
            CRect channel;
            ::SendMessageW(header->hwndFrom, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
            channel.top = channel.CenterPoint().y - px(3);
            channel.bottom = channel.top + px(6);

            CBrush track;
            WIN32_OP_D(track.CreateSolidBrush(subtle_color()) != nullptr);
            SelectObjectScope track_scope(dc, track);
            SelectObjectScope pen_scope(dc, GetStockObject(NULL_PEN));
            dc.RoundRect(&channel, CPoint(px(6), px(6)));

            const int minimum = static_cast<int>(::SendMessageW(header->hwndFrom, TBM_GETRANGEMIN, 0, 0));
            const int maximum = static_cast<int>(::SendMessageW(header->hwndFrom, TBM_GETRANGEMAX, 0, 0));
            const int position = static_cast<int>(::SendMessageW(header->hwndFrom, TBM_GETPOS, 0, 0));
            CRect active = channel;
            active.right = active.left + MulDiv(channel.Width(), position - minimum, (std::max)(1, maximum - minimum));
            CBrush active_brush;
            WIN32_OP_D(active_brush.CreateSolidBrush(accent_color()) != nullptr);
            SelectObjectScope active_scope(dc, active_brush);
            dc.RoundRect(&active, CPoint(px(6), px(6)));
            return CDRF_SKIPDEFAULT;
        }
        if (custom->dwItemSpec == TBCD_THUMB) {
            CRect thumb(custom->rc);
            thumb.DeflateRect(2, 2);
            CBrush thumb_brush;
            WIN32_OP_D(thumb_brush.CreateSolidBrush(accent_color()) != nullptr);
            SelectObjectScope brush_scope(dc, thumb_brush);
            CPen thumb_pen;
            WIN32_OP_D(thumb_pen.CreatePen(PS_SOLID, 1, blend_color(accent_color(), RGB(255, 255, 255), 35)) != nullptr);
            SelectObjectScope pen_scope(dc, thumb_pen);
            dc.Ellipse(&thumb);
            return CDRF_SKIPDEFAULT;
        }
        return CDRF_SKIPDEFAULT;
    }

    LRESULT on_command(UINT, WPARAM wp, LPARAM lp, BOOL&) {
        const auto id = static_cast<UINT_PTR>(LOWORD(wp));
        const auto code = HIWORD(wp);

        if (id == kCloseCommand && code == BN_CLICKED) {
            close_picker();
            return 0;
        }

        if (id >= kSpeakerOffsetBase && id < kSpeakerOffsetBase + outputs_.size()) {
            const auto edit = reinterpret_cast<HWND>(lp);
            if (code == EN_SETFOCUS) {
                offset_edit_ = edit;
                ::SendMessageW(edit, EM_SETSEL, 0, -1);
            } else if (code == EN_KILLFOCUS && edit == offset_edit_) {
                finish_offset_edit();
            }
            return 0;
        }

        if (id == kRefreshCommand && code == BN_CLICKED) {
            finish_offset_edit();
            MultiroomComponentState::instance().refresh_outputs();
            refresh_picker_state();
            return 0;
        }

        if (id == kPairCommand && code == BN_CLICKED) {
            pair_selected_output();
            return 0;
        }

        if (id == kGroupComboId && code == CBN_SELENDOK) {
            finish_offset_edit();
            HWND combo = ::GetDlgItem(m_hWnd, static_cast<int>(kGroupComboId));
            const int selected = combo != nullptr ? static_cast<int>(::SendMessageW(combo, CB_GETCURSEL, 0, 0)) : 0;
            if (selected > 0 && selected <= static_cast<int>(groups_.size())) {
                const bool activated = MultiroomComponentState::instance().activate_speaker_group(
                    groups_[static_cast<size_t>(selected - 1)].id);
                if (!activated) ::SendMessageW(combo, CB_SETCURSEL, 0, 0);
                // The open dropdown may send this before its list closes.
                // Apply the latest snapshot on the regular popup timer.
            }
            return 0;
        }

        if (id >= kSpeakerCheckBase && id < kSpeakerCheckBase + outputs_.size() && code == BN_CLICKED) {
            finish_offset_edit();
            const auto index = static_cast<size_t>(id - kSpeakerCheckBase);
            MultiroomComponentState::instance().toggle_output(outputs_[index].id);
            refresh_picker_state();
            return 0;
        }

        return 0;
    }

    LRESULT on_timer(UINT, WPARAM wp, LPARAM, BOOL&) {
        if (wp == kRefreshTimer) refresh_picker_state();
        return 0;
    }

    void refresh_picker_state() {
        // Discovery/control can complete while the user is dragging a thumb.
        // Rebuilding child windows at that point would destroy the active
        // trackbar and discard its not-yet-committed local position.
        if (volume_drag_active_) return;

        auto& state = MultiroomComponentState::instance();
        auto outputs = state.outputs();
        auto groups = state.speaker_groups();
        auto group_signature = groups_signature(groups);
        const bool rebuild = !same_speaker_rows(outputs_, outputs) || group_signature != groups_signature_;
        const HWND combo = ::GetDlgItem(m_hWnd, static_cast<int>(kGroupComboId));
        const HWND pin_edit = ::GetDlgItem(m_hWnd, static_cast<int>(kPairPinEditId));
        const bool editing_pin = pin_edit != nullptr && pin_edit == ::GetFocus();
        if (rebuild && (offset_edit_ != nullptr || editing_pin || ::SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != FALSE)) return;

        const int previous_height = popup_height();
        const auto focus = focused_control();
        const auto previous_summary = selection_summary();
        auto previous_outputs = std::exchange(outputs_, std::move(outputs));
        auto status = outputs_.empty() ? state.status_text() : std::wstring{};
        const bool status_changed = status != status_;
        status_ = std::move(status);
        if (rebuild) {
            groups_ = std::move(groups);
            groups_signature_ = std::move(group_signature);
            if (previous_height != popup_height()) resize_to_content();
            rebuild_controls();
            restore_focus(focus);
            redraw_all();
        } else {
            if (status_changed && outputs_.empty()) {
                ::SetDlgItemTextW(m_hWnd, static_cast<int>(kEmptyStatusId), empty_status_text());
            }
            if (previous_summary != selection_summary()) {
                const CRect summary(client_width() / 2, header_height(), client_width() - px(kPopupPadding), list_top());
                InvalidateRect(&summary, FALSE);
            }
            sync_control_values(&previous_outputs);
        }
        update_pairing_controls(state.pairing_in_progress());
    }

    LRESULT on_scroll(UINT, WPARAM wp, LPARAM lp, BOOL&) {
        HWND slider = reinterpret_cast<HWND>(lp);
        if (slider == nullptr) return 0;

        const int id = ::GetDlgCtrlID(slider);
        if (id < static_cast<int>(kSpeakerVolumeBase) ||
            id >= static_cast<int>(kSpeakerVolumeBase + outputs_.size())) {
            return 0;
        }

        const auto index = static_cast<size_t>(id - kSpeakerVolumeBase);
        const int volume = static_cast<int>(::SendMessageW(slider, TBM_GETPOS, 0, 0));
        outputs_[index].volume = volume;

        if (index < volume_labels_.size() && volume_labels_[index] != nullptr) {
            const auto text = volume_label(volume);
            ::SetWindowTextW(volume_labels_[index], text.c_str());
        }

        const auto notification = LOWORD(wp);
        if (notification == TB_THUMBTRACK) {
            volume_drag_active_ = true;
            return 0;
        }

        volume_drag_active_ = false;
        MultiroomComponentState::instance().set_output_volume(outputs_[index].id, volume);
        if (HWND check = ::GetDlgItem(m_hWnd, static_cast<int>(kSpeakerCheckBase + index))) {
            ::InvalidateRect(check, nullptr, FALSE);
        }
        return 0;
    }

    LRESULT on_vertical_scroll(UINT, WPARAM wp, LPARAM, BOOL&) {
        int next_row = static_cast<int>(first_visible_row_);
        switch (LOWORD(wp)) {
        case SB_LINEUP:
            --next_row;
            break;
        case SB_LINEDOWN:
            ++next_row;
            break;
        case SB_PAGEUP:
            next_row -= static_cast<int>(visible_row_count());
            break;
        case SB_PAGEDOWN:
            next_row += static_cast<int>(visible_row_count());
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:
            next_row = static_cast<int>(HIWORD(wp));
            break;
        default:
            return 0;
        }

        set_first_visible_row(next_row);
        return 0;
    }

    LRESULT on_mouse_wheel(UINT, WPARAM wp, LPARAM, BOOL&) {
        wheel_delta_ += GET_WHEEL_DELTA_WPARAM(wp);
        const int detents = wheel_delta_ / WHEEL_DELTA;
        wheel_delta_ %= WHEEL_DELTA;
        if (detents == 0) return 0;

        UINT lines = 3;
        ::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const auto rows = lines == WHEEL_PAGESCROLL ? visible_row_count() :
            (std::min)(visible_row_count(), static_cast<size_t>(lines));
        set_first_visible_row(static_cast<int>(first_visible_row_) - detents * static_cast<int>(rows));
        return 0;
    }

    LRESULT on_activate(UINT, WPARAM wp, LPARAM, BOOL&) {
        if (LOWORD(wp) == WA_INACTIVE) {
            finish_offset_edit();
            DestroyWindow();
        }
        return 0;
    }

    LRESULT on_key_down(UINT, WPARAM wp, LPARAM, BOOL& handled) {
        if (wp == VK_ESCAPE) {
            close_picker();
            return 0;
        }
        if (wp == VK_TAB) {
            move_keyboard_focus(0, (::GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        }
        handled = FALSE;
        return 0;
    }

    enum class PickerKey : WPARAM { Next, Previous, Close, Click };

    static LRESULT CALLBACK control_input(
        HWND control, UINT message, WPARAM wp, LPARAM lp, UINT_PTR subclass, DWORD_PTR data) {
        auto& popup = *reinterpret_cast<SpeakerPickerPopup*>(data);
        if (message == WM_NCDESTROY) {
            ::RemoveWindowSubclass(control, control_input, subclass);
            return ::DefSubclassProc(control, message, wp, lp);
        }

        const auto id = static_cast<UINT_PTR>(::GetDlgCtrlID(control));
        const bool combo = id == kGroupComboId;
        const bool dropdown_open = combo && ::SendMessageW(control, CB_GETDROPPEDSTATE, 0, 0);
        const bool offset = id >= kSpeakerOffsetBase && id < kSpeakerOffsetBase + popup.outputs_.size();
        const auto post_key = [&](PickerKey key) {
            ::PostMessageW(popup.m_hWnd, kPickerKeyMessage, static_cast<WPARAM>(key), reinterpret_cast<LPARAM>(control));
        };

        if (message == WM_MOUSEWHEEL && !dropdown_open) {
            // Scrolling the list must not change a focused speaker's volume.
            ::PostMessageW(popup.m_hWnd, WM_MOUSEWHEEL, wp, lp);
            return 0;
        }
        if (message == WM_KEYDOWN) {
            if (wp == VK_TAB) {
                if (dropdown_open) ::SendMessageW(control, CB_SHOWDROPDOWN, FALSE, 0);
                post_key((::GetKeyState(VK_SHIFT) & 0x8000) ? PickerKey::Previous : PickerKey::Next);
                return 0;
            }
            if (wp == VK_ESCAPE && !dropdown_open) {
                if (offset && popup.offset_edit_ == control &&
                    text_from_window(control) != std::to_wstring(popup.outputs_[id - kSpeakerOffsetBase].offset_ms)) {
                    popup.finish_offset_edit(false);
                    popup.offset_edit_ = control;
                    ::SendMessageW(control, EM_SETSEL, 0, -1);
                } else {
                    post_key(PickerKey::Close);
                }
                return 0;
            }
            if (wp == VK_RETURN && !dropdown_open) {
                if (offset) {
                    popup.finish_offset_edit();
                    popup.offset_edit_ = control;
                    ::SendMessageW(control, EM_SETSEL, 0, -1);
                    return 0;
                }
                if (combo) {
                    ::SendMessageW(control, CB_SHOWDROPDOWN, TRUE, 0);
                    return 0;
                }
                if (id == kPairPinEditId || id == kRefreshCommand || id == kPairCommand || id == kCloseCommand ||
                    (id >= kSpeakerCheckBase && id < kSpeakerCheckBase + popup.outputs_.size())) {
                    post_key(PickerKey::Click);
                    return 0;
                }
            }
        } else if (message == WM_CHAR && !dropdown_open && (wp == VK_TAB || wp == VK_ESCAPE || wp == VK_RETURN)) {
            return 0;
        }
        return ::DefSubclassProc(control, message, wp, lp);
    }

    LRESULT on_picker_key(UINT, WPARAM wp, LPARAM lp, BOOL&) {
        auto control = reinterpret_cast<HWND>(lp);
        if (::GetParent(control) != m_hWnd) return 0;
        const auto id = static_cast<UINT_PTR>(::GetDlgCtrlID(control));
        switch (static_cast<PickerKey>(wp)) {
        case PickerKey::Next:
        case PickerKey::Previous:
            move_keyboard_focus(id, static_cast<PickerKey>(wp) == PickerKey::Previous);
            break;
        case PickerKey::Close:
            close_picker();
            break;
        case PickerKey::Click:
            if (id == kPairPinEditId) control = ::GetDlgItem(m_hWnd, static_cast<int>(kPairCommand));
            if (::IsWindowEnabled(control)) ::SendMessageW(control, BM_CLICK, 0, 0);
            break;
        }
        return 0;
    }

    std::optional<size_t> speaker_row(UINT_PTR id) const {
        for (const auto base : {kSpeakerCheckBase, kSpeakerVolumeBase, kSpeakerOffsetBase}) {
            if (id >= base && id < base + outputs_.size()) return id - base;
        }
        return std::nullopt;
    }

    struct SavedFocus {
        UINT_PTR id = 0;
        std::string speaker;
    };

    SavedFocus focused_control() const {
        const auto control = ::GetFocus();
        if (::GetParent(control) != m_hWnd) return {};
        const auto id = static_cast<UINT_PTR>(::GetDlgCtrlID(control));
        if (const auto row = speaker_row(id)) return {id - *row, outputs_[*row].id};
        return {id, {}};
    }

    bool focus_control(UINT_PTR id) {
        if (id == 0) return false;
        const auto control = ::GetDlgItem(m_hWnd, static_cast<int>(id));
        if (control == nullptr || !::IsWindowEnabled(control)) return false;
        ::SetFocus(control);
        return true;
    }

    void reveal_control(UINT_PTR id) {
        const auto row = speaker_row(id);
        if (!row || visible_row_count() == 0) return;
        if (*row < first_visible_row_) {
            set_first_visible_row(static_cast<int>(*row), false);
        } else if (*row - first_visible_row_ >= visible_row_count()) {
            set_first_visible_row(static_cast<int>(*row + 1 - visible_row_count()), false);
        }
    }

    UINT_PTR resolve_focus(const SavedFocus& focus) const {
        if (focus.speaker.empty()) return focus.id;
        const auto output = std::find_if(outputs_.begin(), outputs_.end(), [&](const auto& entry) {
            return entry.id == focus.speaker;
        });
        return output == outputs_.end() ? 0 : focus.id + std::distance(outputs_.begin(), output);
    }

    void restore_focus(const SavedFocus& focus) {
        const auto id = resolve_focus(focus);
        reveal_control(id);
        if (!focus_control(id)) move_keyboard_focus(0, false);
    }

    void move_keyboard_focus(UINT_PTR from, bool backwards) {
        std::vector<UINT_PTR> order;
        if (!groups_.empty()) order.push_back(kGroupComboId);
        for (size_t index = 0; index < outputs_.size(); ++index) {
            const auto playable = output_playable(outputs_[index]);
            if (playable || outputs_[index].selected) order.push_back(kSpeakerCheckBase + index);
            if (playable) {
                order.push_back(kSpeakerVolumeBase + index);
                order.push_back(kSpeakerOffsetBase + index);
            }
        }
        if (pairing_controls_visible()) {
            order.push_back(kPairPinEditId);
            if (::IsWindowEnabled(::GetDlgItem(m_hWnd, static_cast<int>(kPairCommand)))) order.push_back(kPairCommand);
        }
        order.push_back(kRefreshCommand);
        order.push_back(kCloseCommand);

        const auto it = std::find(order.begin(), order.end(), from);
        size_t position = it == order.end() ? (backwards ? 0 : order.size() - 1) :
            static_cast<size_t>(std::distance(order.begin(), it));
        for (size_t attempt = 0; attempt < order.size(); ++attempt) {
            position = (position + (backwards ? order.size() - 1 : 1)) % order.size();
            reveal_control(order[position]);
            if (focus_control(order[position])) return;
        }
    }

    void close_picker() {
        const HWND focus = opener_;
        finish_offset_edit();
        DestroyWindow();
        if (focus != nullptr && ::IsWindow(focus)) ::SetFocus(focus);
    }

    void rebuild_controls() {
        finish_offset_edit();
        const auto pin = text_from_window(::GetDlgItem(m_hWnd, static_cast<int>(kPairPinEditId)));
        for (HWND control : controls_) {
            if (control != nullptr) ::DestroyWindow(control);
        }
        controls_.clear();
        first_visible_row_ = (std::min)(first_visible_row_, max_first_visible_row());
        update_scrollbar();

        const int padding = px(kPopupPadding);
        const int width = client_width();
        const int right = width - padding;
        create_control(0, L"BUTTON", L"Close", WS_TABSTOP | BS_OWNERDRAW,
            kCloseCommand, right - px(44), px(4), px(44), px(44));

        const int group_left = padding + px(74);
        create_control(0, L"STATIC", L"Group", SS_CENTERIMAGE,
            kGroupLabelId, padding, px(49), px(64), px(36));
        const auto group_flags = groups_.empty() ? WS_DISABLED : 0;
        const auto combo = create_control(0, WC_COMBOBOXW, L"Group",
            WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL | group_flags,
            kGroupComboId, group_left, px(49), right - group_left, px(260));
        ::SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(30));
        ::SendMessageW(combo, CB_SETITEMHEIGHT, 0, px(30));
        ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(
            groups_.empty() ? L"No saved groups" : L"Choose a speaker group\x2026"));
        for (const auto& group : groups_) {
            const auto label = widen_utf8(group.name);
            ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }

        if (outputs_.empty()) {
            create_control(0, L"STATIC", empty_status_text(), SS_CENTER | SS_EDITCONTROL,
                kEmptyStatusId, padding, list_top() + px(10), width - 2 * padding, px(50));
        }

        rebuild_speaker_rows();

        if (pairing_controls_visible()) {
            const int footer_top = popup_height() - footer_height();
            const int content_width = right - padding;
            const int pin_left = padding + px(56);
            create_control(0, L"STATIC", L"PIN", SS_CENTERIMAGE,
                kPairPinLabelId, padding, footer_top + px(6), px(50), px(36));
            create_control(WS_EX_CLIENTEDGE, L"EDIT", pin.c_str(),
                WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, kPairPinEditId,
                pin_left, footer_top + px(6), (std::min)(px(98), content_width / 3), px(36));
            const int pair_width = (std::min)(px(128), content_width / 3);
            create_control(0, L"BUTTON", L"Pair", WS_TABSTOP | BS_OWNERDRAW,
                kPairCommand, right - pair_width, footer_top + px(4), pair_width, px(42));
        }

        create_control(0, L"BUTTON", L"Refresh Speakers & TVs", WS_TABSTOP | BS_OWNERDRAW,
            kRefreshCommand, 0, popup_height() - px(kFooterHeight), width, px(kFooterHeight));
    }

    void rebuild_speaker_rows() {
        finish_offset_edit();
        for (HWND control : speaker_controls_) {
            if (control != nullptr) ::DestroyWindow(control);
        }
        speaker_controls_.clear();
        volume_labels_.assign(outputs_.size(), nullptr);

        int row_top = list_top();
        const auto row_end = (std::min)(outputs_.size(), first_visible_row_ + visible_row_count());
        for (size_t index = first_visible_row_; index < row_end; ++index) {
            create_speaker_row(index, row_top);
            row_top += row_height();
        }
        sync_control_values();
    }

    void redraw_all() {
        RedrawWindow(
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    const wchar_t* empty_status_text() const {
        return status_.empty() ? L"No compatible speakers found" : status_.c_str();
    }

    CRect pairing_message_rect() const {
        return CRect(px(kPopupPadding), px(87), client_width() - px(kPopupPadding), header_height());
    }

    void set_pairing_message(std::wstring message) {
        if (message == pairing_message_) return;
        pairing_message_ = std::move(message);
        const auto rect = pairing_message_rect();
        InvalidateRect(&rect, FALSE);
    }

    void update_pairing_controls(bool pairing) {
        if (HWND button = ::GetDlgItem(m_hWnd, static_cast<int>(kPairCommand));
            button != nullptr && (::IsWindowEnabled(button) != FALSE) == pairing) {
            ::EnableWindow(button, !pairing);
        }
        if (!pairing && pairing_requested_) {
            pairing_requested_ = false;
            set_pairing_message({});
        }
    }

    std::string pair_target_id() const {
        std::string fallback;
        std::string selected;
        size_t fallback_count = 0;
        size_t selected_count = 0;

        for (const auto& output : outputs_) {
            if (!output_playable(output) || !output.has_password) {
                continue;
            }
            if (output.selected) {
                selected = output.id;
                ++selected_count;
            }
            fallback = output.id;
            ++fallback_count;
        }

        if (selected_count == 1) return selected;
        return selected_count == 0 && fallback_count == 1 ? fallback : std::string{};
    }

    void pair_selected_output() {
        auto& state = MultiroomComponentState::instance();
        if (state.pairing_in_progress()) return;

        const auto target_id = pair_target_id();
        if (target_id.empty()) {
            set_pairing_message(L"Select one speaker to pair.");
            return;
        }

        const auto pin = narrow_pin(text_from_window(::GetDlgItem(m_hWnd, static_cast<int>(kPairPinEditId))));
        if (pin.empty()) {
            set_pairing_message(L"Enter the PIN shown by the speaker or TV.");
            if (HWND pin_control = ::GetDlgItem(m_hWnd, static_cast<int>(kPairPinEditId)); pin_control != nullptr) {
                ::SetFocus(pin_control);
                ::SendMessageW(pin_control, EM_SETSEL, 0, -1);
            }
            return;
        }

        state.pair_output(target_id, pin);
        pairing_requested_ = true;
        set_pairing_message(L"Pairing speaker...");
        update_pairing_controls(state.pairing_in_progress());
    }

    void create_speaker_row(size_t index, int row_top) {
        const auto& output = outputs_[index];
        const auto name = widen_utf8(output.name.empty() ? output.id : output.name);
        const bool playable = output_playable(output);
        const bool can_toggle = playable || output.selected;
        const int padding = px(kPopupPadding);
        const int right = client_width() - padding;

        create_control(0, L"BUTTON", speaker_action_name(output).c_str(),
            WS_TABSTOP | BS_OWNERDRAW | (can_toggle ? 0 : WS_DISABLED), kSpeakerCheckBase + index,
            padding, row_top + px(2), right - padding, px(56), true);

        if (!playable) return;

        const bool stacked = layout_.stack_controls(layout_width());
        const int slider_left = padding + px(72);
        const int percent_left = right - px(stacked ? 50 : 232);
        const int controls_top = row_top + px(64);
        create_control(0, L"STATIC", L"Volume", SS_CENTERIMAGE,
            0, padding, controls_top, px(66), px(38), true);
        const auto slider = create_control(0, TRACKBAR_CLASSW, (L"Volume for " + name).c_str(),
            WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_FIXEDLENGTH | TBS_TRANSPARENTBKGND,
            kSpeakerVolumeBase + index, slider_left, controls_top,
            percent_left - slider_left - px(6), px(38), true);
        volume_labels_[index] = create_control(0, L"STATIC", L"", SS_RIGHT | SS_CENTERIMAGE,
            kVolumeLabelBase + index, percent_left, controls_top, px(50), px(38), true);
        ::SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        ::SendMessageW(slider, TBM_SETTHUMBLENGTH, px(22), 0);
        ::SendMessageW(slider, TBM_SETPAGESIZE, 0, 10);

        const int delay_left = stacked ? padding : right - px(166);
        const int delay_top = stacked ? row_top + px(108) : controls_top;
        create_control(0, L"STATIC", L"Delay (ms)", SS_CENTERIMAGE,
            0, delay_left, delay_top, px(90), px(38), true);
        const auto edit = create_control(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_TABSTOP | ES_RIGHT | ES_AUTOHSCROLL, kSpeakerOffsetBase + index,
            delay_left + px(94), delay_top + px(1), px(72), px(36), true);
        ::SendMessageW(edit, EM_SETLIMITTEXT, 5, 0);
    }

    std::wstring speaker_action_name(const multiroom::OutputDevice& output) const {
        return (output.selected ? L"Deselect " : L"Select ") +
            widen_utf8(output.name.empty() ? output.id : output.name);
    }

    void finish_offset_edit(bool commit = true) {
        if (offset_edit_ == nullptr) return;

        const HWND edit = offset_edit_;
        offset_edit_ = nullptr;
        const auto id = static_cast<UINT_PTR>(::GetDlgCtrlID(edit));
        if (id < kSpeakerOffsetBase || id >= kSpeakerOffsetBase + outputs_.size()) return;

        auto& output = outputs_[id - kSpeakerOffsetBase];
        if (commit) {
            const auto text = text_from_window(edit);
            wchar_t* end = nullptr;
            const auto parsed = std::wcstol(text.c_str(), &end, 10);
            if (end != text.c_str() && *end == L'\0') {
                constexpr long limit = multiroom::kMaxOutputOffsetMs;
                const auto offset_ms = static_cast<int>(std::clamp(parsed, -limit, limit));
                if (offset_ms != output.offset_ms) {
                    MultiroomComponentState::instance().set_output_offset_ms(output.id, offset_ms);
                    output.offset_ms = offset_ms;
                }
            }
        }
        const auto offset = std::to_wstring(output.offset_ms);
        ::SetWindowTextW(edit, offset.c_str());
    }

    HWND create_control(
        DWORD ex_style, const wchar_t* class_name, const wchar_t* text, DWORD style,
        UINT_PTR id, int x, int y, int width, int height, bool speaker_row = false) {
        const auto control = ::CreateWindowExW(ex_style, class_name, text, WS_CHILD | WS_VISIBLE | style,
            x, y, width, height, m_hWnd, reinterpret_cast<HMENU>(id), core_api::get_my_instance(), nullptr);
        WIN32_OP_D(control != nullptr);
        if (control == nullptr) return nullptr;

        ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(popup_font()), FALSE);
        if ((style & WS_TABSTOP) != 0) {
            WIN32_OP_D(::SetWindowSubclass(control, control_input, 0, reinterpret_cast<DWORD_PTR>(this)) != FALSE);
        }
        (speaker_row ? speaker_controls_ : controls_).push_back(control);
        return control;
    }

    void sync_control_values(const std::vector<multiroom::OutputDevice>* previous = nullptr) {
        if (HWND combo = ::GetDlgItem(m_hWnd, static_cast<int>(kGroupComboId));
            combo != nullptr && ::SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) == FALSE) {
            const auto active_group_id = MultiroomComponentState::instance().active_speaker_group_id();
            const auto group = std::find_if(groups_.begin(), groups_.end(), [&](const auto& entry) {
                return entry.id == active_group_id;
            });
            const auto index = group == groups_.end() ? 0 : std::distance(groups_.begin(), group) + 1;
            if (::SendMessageW(combo, CB_GETCURSEL, 0, 0) != index) {
                ::SendMessageW(combo, CB_SETCURSEL, index, 0);
            }
        }

        const auto row_end = (std::min)(outputs_.size(), first_visible_row_ + visible_row_count());
        for (size_t index = first_visible_row_; index < row_end; ++index) {
            const auto& output = outputs_[index];
            const auto* old = previous == nullptr ? nullptr : &(*previous)[index];
            if (old == nullptr || old->selected != output.selected || output_status_label(*old) != output_status_label(output)) {
                if (HWND check = ::GetDlgItem(m_hWnd, static_cast<int>(kSpeakerCheckBase + index))) {
                    const bool can_toggle = output.selected || output_playable(output);
                    if ((::IsWindowEnabled(check) != FALSE) != can_toggle) ::EnableWindow(check, can_toggle);
                    if (old != nullptr && old->selected != output.selected) {
                        ::SetWindowTextW(check, speaker_action_name(output).c_str());
                    }
                    ::InvalidateRect(check, nullptr, FALSE);
                }
            }
            if (old == nullptr || old->volume != output.volume) {
                if (HWND slider = ::GetDlgItem(m_hWnd, static_cast<int>(kSpeakerVolumeBase + index))) {
                    ::SendMessageW(slider, TBM_SETPOS, TRUE, output.volume);
                }
                if (index < volume_labels_.size() && volume_labels_[index] != nullptr) {
                    const auto text = volume_label(output.volume);
                    ::SetWindowTextW(volume_labels_[index], text.c_str());
                }
            }
            if (old == nullptr || old->offset_ms != output.offset_ms) {
                if (HWND edit = ::GetDlgItem(m_hWnd, static_cast<int>(kSpeakerOffsetBase + index)); edit != nullptr && edit != offset_edit_) {
                    const auto offset = std::to_wstring(output.offset_ms);
                    ::SetWindowTextW(edit, offset.c_str());
                }
            }
        }
    }

    size_t visible_row_count() const {
        return layout_.visible_rows(outputs_.size(), list_top() + footer_height() + px(8), layout_width());
    }

    size_t max_first_visible_row() const {
        const auto visible = visible_row_count();
        return outputs_.size() > visible ? outputs_.size() - visible : 0;
    }

    int client_width() const {
        CRect rc;
        WIN32_OP_D(GetClientRect(&rc));
        return rc.Width();
    }

    std::wstring selection_summary() const {
        const auto selected = std::count_if(outputs_.begin(), outputs_.end(), [](const auto& output) {
            return output.selected;
        });
        if (selected == 0) return L"None selected";
        return std::to_wstring(selected) + L" selected";
    }

    void update_scrollbar() {
        const auto max_first = max_first_visible_row();
        SCROLLINFO info = {};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        info.nMin = 0;
        info.nMax = outputs_.empty() ? 0 : static_cast<int>(outputs_.size() - 1);
        info.nPage = static_cast<UINT>(visible_row_count());
        info.nPos = static_cast<int>(first_visible_row_);
        SetScrollInfo(SB_VERT, &info, TRUE);
        ShowScrollBar(SB_VERT, max_first != 0);
    }

    void set_first_visible_row(int row, bool preserve_focus = true) {
        if (volume_drag_active_) return;

        const auto clamped = static_cast<size_t>(
            std::clamp(row, 0, static_cast<int>(max_first_visible_row())));
        if (clamped == first_visible_row_) return;

        const auto focus = focused_control();
        first_visible_row_ = clamped;
        update_scrollbar();
        rebuild_speaker_rows();
        if (preserve_focus && !focus.speaker.empty() && !focus_control(resolve_focus(focus))) {
            for (size_t index = first_visible_row_; index < first_visible_row_ + visible_row_count(); ++index) {
                if (focus_control(kSpeakerCheckBase + index)) break;
            }
            if (::GetParent(::GetFocus()) != m_hWnd) focus_control(kRefreshCommand);
        }
        const CRect rows(0, list_top(), client_width(), popup_height() - footer_height());
        RedrawWindow(&rows, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    int popup_height() const {
        const int body = outputs_.empty() ? px(68) : static_cast<int>(visible_row_count()) * row_height() + px(8);
        return list_top() + body + footer_height();
    }

    void resize_to_content() {
        CRect rc;
        WIN32_OP_D(GetWindowRect(&rc));
        resize_to_content(rc.TopLeft());
    }

    void resize_to_content(CPoint position) {
        const auto rc = rectangle_at(position);
        WIN32_OP_D(::SetWindowPos(m_hWnd, nullptr, rc.left, rc.top, rc.Width(), rc.Height(), SWP_NOZORDER | SWP_NOACTIVATE));
        apply_rounded_region();
    }

    LRESULT on_dpi_changed(UINT, WPARAM wp, LPARAM lp, BOOL&) {
        const auto focus = focused_control();
        std::optional<std::wstring> offset_draft;
        if (offset_edit_ != nullptr) {
            offset_draft = text_from_window(offset_edit_);
            offset_edit_ = nullptr;
        }
        if (volume_drag_active_) {
            if (const auto row = speaker_row(resolve_focus(focus)); row && output_playable(outputs_[*row])) {
                MultiroomComponentState::instance().set_output_volume(outputs_[*row].id, outputs_[*row].volume);
            }
            volume_drag_active_ = false;
        }
        const CRect suggested(*reinterpret_cast<const RECT*>(lp));
        configure_layout(suggested, HIWORD(wp));
        update_fonts();
        resize_to_content(suggested.TopLeft());
        rebuild_controls();
        restore_focus(focus);
        if (offset_draft && offset_edit_ != nullptr) {
            ::SetWindowTextW(offset_edit_, offset_draft->c_str());
        }
        redraw_all();
        return 0;
    }

    LRESULT on_setting_changed(UINT, WPARAM wp, LPARAM, BOOL& handled) {
        if (wp == SPI_SETHIGHCONTRAST) {
            update_theme();
            redraw_all();
        }
        handled = FALSE;
        return 0;
    }

    int footer_height() const {
        return px(pairing_controls_visible() ? kPairingFooterHeight : kFooterHeight);
    }

    bool pairing_controls_visible() const {
        return std::any_of(outputs_.begin(), outputs_.end(), [](const auto& output) {
            return output.supports_airplay2 && output.has_password;
        });
    }

    void apply_rounded_region() {
        CRect window;
        WIN32_OP_D(GetWindowRect(&window));
        HRGN region = ::CreateRoundRectRgn(0, 0, window.Width() + 1, window.Height() + 1, px(16), px(16));
        if (region == nullptr) return;
        if (::SetWindowRgn(m_hWnd, region, TRUE) == 0) {
            ::DeleteObject(region);
        }
    }

    void update_theme() {
        HIGHCONTRASTW contrast{};
        contrast.cbSize = sizeof(contrast);
        high_contrast_ = ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
            (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
        if (background_brush_.m_hBrush != nullptr) background_brush_.DeleteObject();
        if (control_background_brush_.m_hBrush != nullptr) control_background_brush_.DeleteObject();
        if (footer_brush_.m_hBrush != nullptr) footer_brush_.DeleteObject();
        WIN32_OP_D(background_brush_.CreateSolidBrush(background_color()) != nullptr);
        WIN32_OP_D(control_background_brush_.CreateSolidBrush(control_background_color()) != nullptr);
        WIN32_OP_D(footer_brush_.CreateSolidBrush(footer_color()) != nullptr);
    }

    COLORREF background_color() const {
        if (high_contrast_) return ::GetSysColor(COLOR_WINDOW);
        return callback_.is_valid() ? callback_->query_std_color(ui_color_background) : RGB(38, 38, 36);
    }

    COLORREF text_color() const {
        if (high_contrast_) return ::GetSysColor(COLOR_WINDOWTEXT);
        return callback_.is_valid() ? callback_->query_std_color(ui_color_text) : RGB(248, 248, 247);
    }

    COLORREF secondary_text_color() const {
        return high_contrast_ ? text_color() : blend_color(text_color(), background_color(), 24);
    }

    COLORREF accent_color() const {
        return high_contrast_ ? ::GetSysColor(COLOR_HIGHLIGHT) : airplay_accent(background_color());
    }

    HGDIOBJ host_font() const {
        return callback_.is_valid() ? reinterpret_cast<HGDIOBJ>(callback_->query_font_ex(ui_font_default)) : GetStockObject(DEFAULT_GUI_FONT);
    }

    HGDIOBJ popup_font() const {
        return body_font_.m_hFont != nullptr ? body_font_.m_hFont : host_font();
    }

    COLORREF subtle_color() const {
        return high_contrast_ ? text_color() : blend_color(background_color(), text_color(), 30);
    }

    COLORREF footer_color() const {
        if (high_contrast_) return background_color();
        return blend_color(background_color(), text_color(), color_is_dark(background_color()) ? 12 : 7);
    }

    COLORREF control_background_color() const {
        if (high_contrast_) return background_color();
        return blend_color(background_color(), text_color(), color_is_dark(background_color()) ? 9 : 4);
    }

    COLORREF pressed_row_color() const {
        if (high_contrast_) return background_color();
        return blend_color(background_color(), accent_color(), color_is_dark(background_color()) ? 15 : 8);
    }

    HWND owner_ = nullptr;
    HWND anchor_ = nullptr;
    HWND opener_ = nullptr;
    ui_element_instance_callback_ptr callback_;
    std::vector<multiroom::OutputDevice> outputs_;
    std::vector<multiroom::SpeakerGroup> groups_;
    std::wstring status_;
    std::wstring pairing_message_;
    std::string groups_signature_;
    std::vector<HWND> controls_;
    std::vector<HWND> speaker_controls_;
    std::vector<HWND> volume_labels_;
    HWND offset_edit_ = nullptr;
    multiroom::ui::SpeakerPickerLayout layout_;
    CRect anchor_rect_;
    CRect work_area_;
    LOGFONTW font_template_{};
    UINT font_template_dpi_ = USER_DEFAULT_SCREEN_DPI;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    CFont body_font_;
    CFont title_font_;
    CFont section_font_;
    CBrush background_brush_;
    CBrush control_background_brush_;
    CBrush footer_brush_;
    size_t first_visible_row_ = 0;
    int wheel_delta_ = 0;
    bool high_contrast_ = false;
    bool volume_drag_active_ = false;
    bool pairing_requested_ = false;
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
        last_destination_label_ = MultiroomComponentState::instance().playback_destination_label();
        SetTimer(kToolbarRefreshTimer, 250);
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
        return ui_element_subclass_playback_information;
    }

    static void g_get_name(pfc::string_base& out) {
        out = "Wireless Speakers";
    }

    static ui_element_config::ptr g_get_default_configuration() {
        return ui_element_config::g_create_empty(g_get_guid());
    }

    static const char* g_get_description() {
        return "Shows the active multiroom destination and opens the speaker picker; place it in the toolbar row next to playback controls, seekbar, or volume.";
    }

    ui_element_min_max_info get_min_max_info() override {
        ui_element_min_max_info info;
        info.m_min_width = 110;
        info.m_min_height = kToolbarMinHeight;
        info.m_max_width = 260;
        info.m_max_height = kToolbarMaxHeight;
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
        MESSAGE_HANDLER(WM_TIMER, on_timer)
        MESSAGE_HANDLER(WM_DESTROY, on_destroy)
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
        const auto label = MultiroomComponentState::instance().playback_destination_label();
        last_destination_label_ = label;
        const bool is_idle = label == L"Idle";
        const COLORREF glyph = is_idle ? text : airplay_accent(background);

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

        if (button.Width() < 70) {
            draw_airplay_audio_glyph(dc.m_hDC, CPoint(button.CenterPoint().x, button.CenterPoint().y - 3), glyph, 9);
        } else {
            draw_airplay_audio_glyph(dc.m_hDC, CPoint(button.left + 18, button.CenterPoint().y - 3), glyph, 8);
            CRect text_rect = button;
            text_rect.DeflateRect(35, 1, 22, 1);
            WIN32_OP_D(dc.DrawTextW(label.c_str(), -1, &text_rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) > 0);

            CRect arrow_rect = button;
            arrow_rect.left = arrow_rect.right - 18;
            WIN32_OP_D(dc.DrawTextW(L"\x25BE", -1, &arrow_rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX) > 0);
        }

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

    LRESULT on_timer(UINT, WPARAM wp, LPARAM, BOOL&) {
        if (wp != kToolbarRefreshTimer) return 0;
        const auto label = MultiroomComponentState::instance().playback_destination_label();
        if (label != last_destination_label_) {
            last_destination_label_ = label;
            Invalidate();
        }
        return 0;
    }

    LRESULT on_destroy(UINT, WPARAM, LPARAM, BOOL&) {
        KillTimer(kToolbarRefreshTimer);
        return 0;
    }

    void open_picker() {
        auto* popup = new SpeakerPickerPopup(m_hWnd, m_callback);
        popup->open_below(m_hWnd);
    }

    ui_element_config::ptr config_;
    std::wstring last_destination_label_;
};

static service_factory_single_t<ui_element_impl<SpeakerSelectorElement>> g_speaker_selector_factory;

}  // namespace

void show_multiroom_speaker_picker(HWND owner, HWND anchor) {
    auto* popup = new SpeakerPickerPopup(owner, nullptr);
    popup->open_below(anchor);
}

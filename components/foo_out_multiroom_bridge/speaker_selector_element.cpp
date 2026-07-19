#include "stdafx.h"
#include "multiroom_component_state.h"
#include "speaker_selector_popup.h"

#include <commctrl.h>
#include <libPPUI/win32_op.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <sstream>

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
static constexpr UINT_PTR kVolumeLabelBase = 33000;
static constexpr UINT_PTR kRefreshTimer = 34000;
static constexpr int kPopupWidth = 408;
static constexpr int kPopupPadding = 22;
static constexpr int kHeaderHeight = 92;
static constexpr int kSectionHeight = 43;
static constexpr int kRowHeight = 82;
static constexpr int kFooterHeight = 54;
static constexpr int kPairingFooterHeight = 94;
static constexpr size_t kMaxVisibleRows = 7;
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

void draw_speaker_glyph(HDC hdc, CRect bounds, COLORREF color) {
    CDCHandle dc(hdc);
    CPen outline;
    WIN32_OP_D(outline.CreatePen(PS_SOLID, 1, color) != nullptr);
    SelectObjectScope pen_scope(dc, outline);
    SelectObjectScope brush_scope(dc, GetStockObject(NULL_BRUSH));

    CRect cabinet(bounds.CenterPoint().x - 8, bounds.top + 2, bounds.CenterPoint().x + 8, bounds.bottom - 2);
    WIN32_OP_D(dc.RoundRect(&cabinet, CPoint(4, 4)));

    CBrush cone;
    WIN32_OP_D(cone.CreateSolidBrush(color) != nullptr);
    SelectObjectScope cone_scope(dc, cone);
    SelectObjectScope cone_pen_scope(dc, GetStockObject(NULL_PEN));
    CRect tweeter(cabinet.CenterPoint().x - 2, cabinet.top + 5, cabinet.CenterPoint().x + 2, cabinet.top + 9);
    dc.Ellipse(&tweeter);
    CRect woofer(cabinet.CenterPoint().x - 4, cabinet.bottom - 11, cabinet.CenterPoint().x + 4, cabinet.bottom - 3);
    dc.Ellipse(&woofer);
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
            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL,
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST) != nullptr);
        apply_rounded_region();
        SetTimer(kRefreshTimer, 250);
        ShowWindow(SW_SHOW);
        SetFocus();
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
    END_MSG_MAP()

    void OnFinalMessage(HWND) override {
        delete this;
    }

private:
    LRESULT on_create(UINT, WPARAM, LPARAM, BOOL&) {
        LOGFONTW popup_logfont = {};
        if (::GetObjectW(popup_font(), sizeof(popup_logfont), &popup_logfont) == sizeof(popup_logfont)) {
            LOGFONTW title_font = popup_logfont;
            title_font.lfWeight = FW_SEMIBOLD;
            title_font.lfHeight = title_font.lfHeight < 0 ? title_font.lfHeight - 3 : title_font.lfHeight + 3;
            WIN32_OP_D(title_font_.CreateFontIndirect(&title_font) != nullptr);

            LOGFONTW section_font = popup_logfont;
            section_font.lfWeight = FW_SEMIBOLD;
            WIN32_OP_D(section_font_.CreateFontIndirect(&section_font) != nullptr);
        }
        WIN32_OP_D(background_brush_.CreateSolidBrush(background_color()) != nullptr);
        WIN32_OP_D(control_background_brush_.CreateSolidBrush(control_background_color()) != nullptr);
        WIN32_OP_D(footer_brush_.CreateSolidBrush(footer_color()) != nullptr);
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

        draw_airplay_audio_glyph(dc.m_hDC, CPoint(rc.CenterPoint().x, 25), text_color(), 10);

        CRect title(kPopupPadding, 42, rc.right - kPopupPadding, 76);
        SelectObjectScope title_scope(dc, title_font_.m_hFont != nullptr ? title_font_.m_hFont : popup_font());
        WIN32_OP_D(dc.DrawTextW(L"AirPlay", -1, &title, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX) > 0);

        SelectObjectScope body_font_scope(dc, popup_font());
        dc.SetTextColor(secondary_text_color());

        CPen divider;
        WIN32_OP_D(divider.CreatePen(PS_SOLID, 1, subtle_color()) != nullptr);
        SelectObjectScope pen_scope(dc, divider);
        dc.MoveTo(0, kHeaderHeight - 1);
        dc.LineTo(rc.right, kHeaderHeight - 1);

        CRect section(kPopupPadding, kHeaderHeight, rc.right - kPopupPadding, kHeaderHeight + kSectionHeight);
        {
            SelectObjectScope section_scope(dc, section_font_.m_hFont != nullptr ? section_font_.m_hFont : popup_font());
            WIN32_OP_D(dc.DrawTextW(L"Speakers & TVs", -1, &section, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX) > 0);
        }

        const auto summary = selection_summary();
        CRect count_rect(kPopupPadding + 160, kHeaderHeight, rc.right - kPopupPadding, kHeaderHeight + kSectionHeight);
        WIN32_OP_D(dc.DrawTextW(summary.c_str(), -1, &count_rect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) > 0);

        int row_top = kHeaderHeight + kSectionHeight;
        const auto row_end = (std::min)(outputs_.size(), first_visible_row_ + visible_row_count());
        for (size_t index = first_visible_row_; index < row_end; ++index) {
            dc.MoveTo(kPopupPadding + 42, row_top + kRowHeight - 1);
            dc.LineTo(rc.right - kPopupPadding, row_top + kRowHeight - 1);
            row_top += kRowHeight;
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
            draw_speaker_glyph(dc.m_hDC, CRect(rc.left + 3, center_y - 14, rc.left + 29, center_y + 14), foreground);

            CRect indicator(rc.right - 24, center_y - 9, rc.right - 6, center_y + 9);
            CPen ring;
            WIN32_OP_D(ring.CreatePen(PS_SOLID, 2, output.selected ? accent_color() : secondary_text_color()) != nullptr);
            SelectObjectScope ring_scope(dc, ring);
            CBrush indicator_brush;
            WIN32_OP_D(indicator_brush.CreateSolidBrush(output.selected ? accent_color() : background_color()) != nullptr);
            SelectObjectScope indicator_scope(dc, indicator_brush);
            dc.Ellipse(&indicator);

            if (output.selected) {
                CPen check;
                WIN32_OP_D(check.CreatePen(PS_SOLID, 2, RGB(255, 255, 255)) != nullptr);
                SelectObjectScope check_scope(dc, check);
                dc.MoveTo(indicator.left + 5, center_y);
                dc.LineTo(indicator.left + 8, center_y + 3);
                dc.LineTo(indicator.left + 14, center_y - 4);
            }

            const auto name = widen_utf8(output.name.empty() ? output.id : output.name);
            dc.SetTextColor(foreground);
            CRect name_rect(rc.left + 42, rc.top, rc.right - 38, rc.bottom);
            WIN32_OP_D(dc.DrawTextW(name.c_str(), -1, &name_rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) > 0);
        } else if (item->CtlID == kRefreshCommand) {
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            CBrush button_brush;
            WIN32_OP_D(button_brush.CreateSolidBrush(pressed ? blend_color(footer_color(), accent_color(), 18) : footer_color()) != nullptr);
            WIN32_OP_D(dc.FillRect(&rc, button_brush));
            dc.SetTextColor(text_color());
            SelectObjectScope title_scope(dc, section_font_.m_hFont != nullptr ? section_font_.m_hFont : popup_font());
            WIN32_OP_D(dc.DrawTextW(L"Refresh Speakers & TVs", -1, &rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX) > 0);
        } else if (item->CtlID == kPairCommand) {
            wchar_t text[64] = {};
            ::GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            CBrush button_brush;
            WIN32_OP_D(button_brush.CreateSolidBrush(
                pressed ? blend_color(background_color(), accent_color(), 25) : blend_color(background_color(), text_color(), 8)) != nullptr);
            SelectObjectScope button_scope(dc, button_brush);
            SelectObjectScope pen_scope(dc, GetStockObject(NULL_PEN));
            dc.RoundRect(&rc, CPoint(12, 12));
            dc.SetTextColor(text_color());
            WIN32_OP_D(dc.DrawTextW(text, -1, &rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX) > 0);
        }

        if ((item->itemState & ODS_FOCUS) != 0) {
            CRect focus = rc;
            focus.DeflateRect(2, 2);
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
            return CDRF_NOTIFYITEMDRAW;
        }
        if (custom->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;

        CDCHandle dc(custom->hdc);
        if (custom->dwItemSpec == TBCD_CHANNEL) {
            CRect channel;
            ::SendMessageW(header->hwndFrom, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
            channel.top = channel.CenterPoint().y - 2;
            channel.bottom = channel.top + 4;

            CBrush track;
            WIN32_OP_D(track.CreateSolidBrush(blend_color(background_color(), text_color(), 18)) != nullptr);
            SelectObjectScope track_scope(dc, track);
            SelectObjectScope pen_scope(dc, GetStockObject(NULL_PEN));
            dc.RoundRect(&channel, CPoint(4, 4));

            const int minimum = static_cast<int>(::SendMessageW(header->hwndFrom, TBM_GETRANGEMIN, 0, 0));
            const int maximum = static_cast<int>(::SendMessageW(header->hwndFrom, TBM_GETRANGEMAX, 0, 0));
            const int position = static_cast<int>(::SendMessageW(header->hwndFrom, TBM_GETPOS, 0, 0));
            CRect active = channel;
            active.right = active.left + MulDiv(channel.Width(), position - minimum, (std::max)(1, maximum - minimum));
            CBrush active_brush;
            WIN32_OP_D(active_brush.CreateSolidBrush(accent_color()) != nullptr);
            SelectObjectScope active_scope(dc, active_brush);
            dc.RoundRect(&active, CPoint(4, 4));
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

        if (id == kPairCommand && code == BN_CLICKED) {
            pair_selected_output();
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

        // Discovery/control can complete while the user is dragging a thumb.
        // Rebuilding child windows at that point would destroy the active
        // trackbar and discard its not-yet-committed local position.
        if (volume_drag_active_) return 0;

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
        if (notification == TB_THUMBTRACK || notification == TB_THUMBPOSITION) {
            volume_drag_active_ = true;
            return 0;
        }

        volume_drag_active_ = false;
        MultiroomComponentState::instance().set_output_volume(outputs_[index].id, volume);
        outputs_signature_ = outputs_signature(outputs_);
        if (owner_ != nullptr) ::InvalidateRect(owner_, nullptr, TRUE);
        SetTimer(kRefreshTimer, 250);
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
        const auto delta = static_cast<short>(HIWORD(wp));
        if (delta != 0) {
            set_first_visible_row(
                static_cast<int>(first_visible_row_) + (delta > 0 ? -1 : 1));
        }
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
        volume_labels_.assign(outputs_.size(), nullptr);
        first_visible_row_ = (std::min)(first_visible_row_, max_first_visible_row());
        update_scrollbar();

        const int width = client_width();

        if (outputs_.empty()) {
            add_control(::CreateWindowExW(
                0,
                L"STATIC",
                status_.empty() ? L"No AirPlay speakers found" : status_.c_str(),
                WS_CHILD | WS_VISIBLE | SS_CENTER | SS_EDITCONTROL,
                kPopupPadding,
                kHeaderHeight + kSectionHeight + 12,
                width - (kPopupPadding * 2),
                42,
                m_hWnd,
                reinterpret_cast<HMENU>(kEmptyStatusId),
                core_api::get_my_instance(),
                nullptr));
        } else {
            int row_top = kHeaderHeight + kSectionHeight;
            const auto row_end = (std::min)(outputs_.size(), first_visible_row_ + visible_row_count());
            for (size_t index = first_visible_row_; index < row_end; ++index) {
                create_speaker_row(index, row_top);
                row_top += kRowHeight;
            }
        }

        const int footer_top = popup_height() - footer_height();
        if (pairing_controls_visible()) {
            add_control(::CreateWindowExW(
                0,
                L"STATIC",
                L"AirPlay code",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                kPopupPadding,
                footer_top + 9,
                92,
                22,
                m_hWnd,
                reinterpret_cast<HMENU>(kPairPinLabelId),
                core_api::get_my_instance(),
                nullptr));

            add_control(::CreateWindowExW(
                WS_EX_CLIENTEDGE,
                L"EDIT",
                L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                kPopupPadding + 100,
                footer_top + 6,
                92,
                24,
                m_hWnd,
                reinterpret_cast<HMENU>(kPairPinEditId),
                core_api::get_my_instance(),
                nullptr));

            add_control(::CreateWindowExW(
                0,
                L"BUTTON",
                L"Pair",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                width - kPopupPadding - 70,
                footer_top + 5,
                70,
                26,
                m_hWnd,
                reinterpret_cast<HMENU>(kPairCommand),
                core_api::get_my_instance(),
                nullptr));
        }

        add_control(::CreateWindowExW(
            0,
            L"BUTTON",
            L"Refresh Speakers & TVs",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0,
            popup_height() - kFooterHeight,
            width,
            kFooterHeight,
            m_hWnd,
            reinterpret_cast<HMENU>(kRefreshCommand),
            core_api::get_my_instance(),
            nullptr));

        sync_control_values();
    }

    std::string pair_target_id() const {
        std::string fallback;
        size_t fallback_count = 0;

        for (const auto& output : outputs_) {
            if (!output.supports_airplay2) {
                continue;
            }
            if (output.selected) {
                return output.id;
            }
            if (output.requires_auth) {
                fallback = output.id;
                ++fallback_count;
            }
        }

        return fallback_count == 1 ? fallback : std::string{};
    }

    void pair_selected_output() {
        const auto target_id = pair_target_id();
        if (target_id.empty()) {
            ::MessageBoxW(m_hWnd, L"Select one AirPlay speaker to pair.", L"Universal Multiroom Bridge", MB_OK | MB_ICONWARNING);
            return;
        }

        const auto pin = narrow_pin(text_from_window(::GetDlgItem(m_hWnd, static_cast<int>(kPairPinEditId))));
        if (pin.empty()) {
            ::MessageBoxW(m_hWnd, L"Enter the AirPlay PIN shown by the speaker or TV.", L"Universal Multiroom Bridge", MB_OK | MB_ICONWARNING);
            if (HWND pin_control = ::GetDlgItem(m_hWnd, static_cast<int>(kPairPinEditId)); pin_control != nullptr) {
                ::SetFocus(pin_control);
                ::SendMessageW(pin_control, EM_SETSEL, 0, -1);
            }
            return;
        }

        MultiroomComponentState::instance().pair_output(target_id, pin);
        status_ = MultiroomComponentState::instance().status_text();
        Invalidate();
        if (owner_ != nullptr) ::InvalidateRect(owner_, nullptr, TRUE);
        SetTimer(kRefreshTimer, 250);
    }

    void create_speaker_row(size_t index, int row_top) {
        const auto& output = outputs_[index];
        const auto name = widen_utf8(output.name.empty() ? output.id : output.name);
        const bool playable = output_playable(output);
        const int width = client_width();

        HWND check = add_control(::CreateWindowExW(
            0,
            L"BUTTON",
            name.c_str(),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY | (playable ? 0 : WS_DISABLED),
            kPopupPadding,
            row_top + 4,
            width - (kPopupPadding * 2),
            35,
            m_hWnd,
            reinterpret_cast<HMENU>(kSpeakerCheckBase + index),
            core_api::get_my_instance(),
            nullptr));

        HWND slider = add_control(::CreateWindowExW(
            0,
            TRACKBAR_CLASSW,
            nullptr,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS | TBS_TRANSPARENTBKGND | (playable ? 0 : WS_DISABLED),
            kPopupPadding + 42,
            row_top + 42,
            width - 164,
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
            width - kPopupPadding - 68,
            row_top + 44,
            68,
            20,
            m_hWnd,
            reinterpret_cast<HMENU>(kVolumeLabelBase + index),
            core_api::get_my_instance(),
            nullptr));

        volume_labels_[index] = label;

        ::SendMessageW(check, BM_SETCHECK, output.selected ? BST_CHECKED : BST_UNCHECKED, 0);
        ::SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        ::SendMessageW(slider, TBM_SETPAGESIZE, 0, 10);
        ::SendMessageW(slider, TBM_SETPOS, TRUE, output.volume);
        const auto text = output_status_label(output);
        ::SetWindowTextW(label, text.c_str());
    }

    HWND add_control(HWND control) {
        WIN32_OP_D(control != nullptr);
        ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(popup_font()), TRUE);
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

    size_t visible_row_count() const {
        return outputs_.empty() ? 0 : (std::min)(outputs_.size(), kMaxVisibleRows);
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
        if (outputs_.empty()) return L"Searching...";
        if (selected == 0) return std::to_wstring(outputs_.size()) + L" available";
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

    void set_first_visible_row(int row) {
        const auto clamped = static_cast<size_t>(
            std::clamp(row, 0, static_cast<int>(max_first_visible_row())));
        if (clamped == first_visible_row_) return;

        first_visible_row_ = clamped;
        rebuild_controls();
        Invalidate();
    }

    int popup_height() const {
        const int body = outputs_.empty() ? 62 : static_cast<int>(visible_row_count()) * kRowHeight + 8;
        return kHeaderHeight + kSectionHeight + body + footer_height();
    }

    void resize_to_content() {
        CRect rc;
        WIN32_OP_D(GetWindowRect(&rc));
        rc.bottom = rc.top + popup_height();
        keep_on_monitor(rc);
        WIN32_OP_D(::SetWindowPos(m_hWnd, HWND_TOPMOST, rc.left, rc.top, rc.Width(), rc.Height(), SWP_NOACTIVATE));
        apply_rounded_region();
    }

    int footer_height() const {
        return pairing_controls_visible() ? kPairingFooterHeight : kFooterHeight;
    }

    bool pairing_controls_visible() const {
        return std::any_of(outputs_.begin(), outputs_.end(), [](const auto& output) {
            return output.supports_airplay2 && output.requires_auth;
        });
    }

    void apply_rounded_region() {
        CRect window;
        WIN32_OP_D(GetWindowRect(&window));
        HRGN region = ::CreateRoundRectRgn(0, 0, window.Width() + 1, window.Height() + 1, 20, 20);
        if (region == nullptr) return;
        if (::SetWindowRgn(m_hWnd, region, TRUE) == 0) {
            ::DeleteObject(region);
        }
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
        return callback_.is_valid() ? callback_->query_std_color(ui_color_background) : RGB(38, 38, 36);
    }

    COLORREF text_color() const {
        return callback_.is_valid() ? callback_->query_std_color(ui_color_text) : RGB(248, 248, 247);
    }

    COLORREF secondary_text_color() const {
        return blend_color(text_color(), background_color(), 42);
    }

    COLORREF accent_color() const {
        return airplay_accent(background_color());
    }

    HGDIOBJ popup_font() const {
        return callback_.is_valid() ? reinterpret_cast<HGDIOBJ>(callback_->query_font_ex(ui_font_default)) : GetStockObject(DEFAULT_GUI_FONT);
    }

    COLORREF subtle_color() const {
        return blend_color(background_color(), text_color(), 18);
    }

    COLORREF footer_color() const {
        return blend_color(background_color(), text_color(), color_is_dark(background_color()) ? 12 : 7);
    }

    COLORREF control_background_color() const {
        return blend_color(background_color(), text_color(), color_is_dark(background_color()) ? 9 : 4);
    }

    COLORREF pressed_row_color() const {
        return blend_color(background_color(), accent_color(), color_is_dark(background_color()) ? 15 : 8);
    }

    HWND owner_ = nullptr;
    ui_element_instance_callback_ptr callback_;
    std::vector<multiroom::OutputDevice> outputs_;
    std::wstring status_;
    std::string outputs_signature_;
    std::vector<HWND> controls_;
    std::vector<HWND> volume_labels_;
    CFont title_font_;
    CFont section_font_;
    CBrush background_brush_;
    CBrush control_background_brush_;
    CBrush footer_brush_;
    size_t first_visible_row_ = 0;
    bool volume_drag_active_ = false;
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
        info.m_min_width = 36;
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
        const auto label = MultiroomComponentState::instance().selected_label();
        const bool has_selection = label != L"No speakers";
        const COLORREF glyph = has_selection ? airplay_accent(background) : text;

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
            const auto display = has_selection ? label : std::wstring(L"AirPlay");
            WIN32_OP_D(dc.DrawTextW(display.c_str(), -1, &text_rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) > 0);

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

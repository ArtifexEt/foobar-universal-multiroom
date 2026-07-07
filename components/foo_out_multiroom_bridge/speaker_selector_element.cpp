#include "stdafx.h"

#include <helpers/BumpableElem.h>
#include <libPPUI/win32_op.h>

#include <set>

namespace {

static constexpr GUID guid_speaker_selector_element = {
    0x4f175c3d, 0x7d32, 0x4bb3, {0xa7, 0x24, 0x4a, 0x24, 0x47, 0x23, 0x43, 0xc3}};
static constexpr UINT_PTR kSpeakerMenuBase = 30000;

struct SpeakerItem {
    const wchar_t* id;
    const wchar_t* name;
};

const SpeakerItem kDemoSpeakers[] = {
    {L"living-room", L"Living Room"},
    {L"kitchen", L"Kitchen"},
    {L"bedroom", L"Bedroom"},
    {L"office", L"Office"},
};

class SpeakerSelectionState {
public:
    SpeakerSelectionState() {
        selected_.insert(L"living-room");
    }

    bool selected(const wchar_t* id) const {
        return selected_.find(id) != selected_.end();
    }

    void toggle(const wchar_t* id) {
        const auto it = selected_.find(id);
        if (it == selected_.end()) {
            selected_.insert(id);
        } else if (selected_.size() > 1) {
            selected_.erase(it);
        }
    }

    std::wstring label() const {
        if (selected_.empty()) return L"No speakers";

        std::wstring first;
        size_t count = 0;
        for (const auto& speaker : kDemoSpeakers) {
            if (!selected(speaker.id)) continue;
            if (first.empty()) first = speaker.name;
            ++count;
        }

        if (count == 0) return L"No speakers";
        if (count == 1) return first;
        return first + L" +" + std::to_wstring(count - 1);
    }

private:
    std::set<std::wstring> selected_;
};

SpeakerSelectionState g_selection;

class SpeakerSelectorElement
    : public ui_element_instance
    , public CWindowImpl<SpeakerSelectorElement> {
public:
    DECLARE_WND_CLASS_EX(TEXT("{4F175C3D-7D32-4BB3-A724-4A24472343C3}"), CS_VREDRAW | CS_HREDRAW, -1);

    SpeakerSelectorElement(ui_element_config::ptr config, ui_element_instance_callback_ptr callback)
        : m_callback(callback)
        , config_(config) {}

    void initialize_window(HWND parent) {
        WIN32_OP(Create(parent) != nullptr);
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
        out = "Multiroom speaker selector";
    }

    static ui_element_config::ptr g_get_default_configuration() {
        return ui_element_config::g_create_empty(g_get_guid());
    }

    static const char* g_get_description() {
        return "Shows the selected multiroom speakers and opens a checkbox speaker picker.";
    }

    ui_element_min_max_info get_min_max_info() override {
        ui_element_min_max_info info;
        info.m_min_width = 120;
        info.m_min_height = 24;
        info.m_max_width = 1000;
        info.m_max_height = 44;
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
        const COLORREF border = m_callback->query_std_color(ui_color_text);

        CBrush background_brush;
        WIN32_OP_D(background_brush.CreateSolidBrush(background) != nullptr);
        WIN32_OP_D(dc.FillRect(&rc, background_brush));

        CRect button = rc;
        button.DeflateRect(2, 2);
        CPen border_pen;
        WIN32_OP_D(border_pen.CreatePen(PS_SOLID, 1, border) != nullptr);
        SelectObjectScope pen_scope(dc, border_pen);
        SelectObjectScope brush_scope(dc, GetStockObject(NULL_BRUSH));
        WIN32_OP_D(dc.RoundRect(&button, CPoint(6, 6)));

        dc.SetTextColor(text);
        dc.SetBkMode(TRANSPARENT);
        SelectObjectScope font_scope(dc, reinterpret_cast<HGDIOBJ>(m_callback->query_font_ex(ui_font_default)));

        CRect text_rect = button;
        text_rect.DeflateRect(8, 1, 22, 1);
        const auto label = g_selection.label();
        WIN32_OP_D(dc.DrawTextW(label.c_str(), -1, &text_rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) > 0);

        CRect arrow_rect = button;
        arrow_rect.left = arrow_rect.right - 18;
        WIN32_OP_D(dc.DrawTextW(L"v", -1, &arrow_rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX) > 0);

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
        CMenu menu;
        WIN32_OP_D(menu.CreatePopupMenu());

        for (size_t index = 0; index < _countof(kDemoSpeakers); ++index) {
            const auto& speaker = kDemoSpeakers[index];
            UINT flags = MF_STRING;
            if (g_selection.selected(speaker.id)) flags |= MF_CHECKED;
            WIN32_OP_D(menu.AppendMenuW(flags, kSpeakerMenuBase + index, speaker.name));
        }

        CRect rc;
        WIN32_OP_D(GetWindowRect(&rc));
        const UINT command = static_cast<UINT>(menu.TrackPopupMenu(
            TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
            rc.left,
            rc.bottom,
            m_hWnd));
        if (command >= kSpeakerMenuBase && command < kSpeakerMenuBase + _countof(kDemoSpeakers)) {
            const auto index = static_cast<size_t>(command - kSpeakerMenuBase);
            g_selection.toggle(kDemoSpeakers[index].id);
            Invalidate();
        }
    }

    ui_element_config::ptr config_;
};

class SpeakerSelectorElementFactory : public ui_element_impl_withpopup<SpeakerSelectorElement> {};

static service_factory_single_t<SpeakerSelectorElementFactory> g_speaker_selector_factory;

}  // namespace

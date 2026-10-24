#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN

#include "ui.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <uxtheme.h>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

namespace ui {

Palette pal{};
bool keyboard_cues = false;

void note_key(const MSG& m) {
    if (keyboard_cues || m.message != WM_KEYDOWN) return;
    switch (m.wParam) {
        case VK_TAB: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            keyboard_cues = true;
            if (HWND f = GetFocus()) InvalidateRect(f, nullptr, FALSE);
            break;
        default: break;
    }
}

namespace {

Theme g_theme = Theme::Auto;
bool g_dark = false;
int g_dpi = 96;
Fonts g_fonts;
ULONG_PTR g_gdiplus = 0;
HBRUSH g_field_brush = nullptr;
HBRUSH g_window_brush = nullptr;
std::wstring g_face = L"Segoe UI";
std::wstring g_face_display = L"Segoe UI";
std::wstring g_face_mono = L"Consolas";

const wchar_t* kKind = L"ui.kind";
const wchar_t* kHover = L"ui.hover";
const wchar_t* kCheck = L"ui.check";
const wchar_t* kPanel = L"ui.panel";
const wchar_t* kBig = L"ui.big";
const wchar_t* kFrame = L"ui.frame";
const wchar_t* kFont = L"ui.font";

constexpr COLORREF rgb(int r, int g, int b) { return RGB(r, g, b); }

Palette light_palette() {
    Palette p{};
    p.window = rgb(250, 250, 250);
    p.panel = rgb(240, 240, 240);
    p.text = rgb(26, 26, 26);
    p.muted = rgb(98, 98, 98);
    p.line = rgb(222, 222, 222);
    p.control = rgb(255, 255, 255);
    p.control_hover = rgb(245, 245, 245);
    p.control_press = rgb(235, 235, 235);
    p.control_line = rgb(204, 204, 204);
    p.flat_hover = rgb(234, 234, 234);
    p.flat_press = rgb(224, 224, 224);
    p.accent = rgb(52, 108, 62);
    p.accent_hover = rgb(45, 96, 54);
    p.accent_press = rgb(38, 82, 46);
    p.accent_text = rgb(255, 255, 255);
    p.field = rgb(255, 255, 255);
    p.field_line = rgb(196, 196, 196);
    p.error = rgb(176, 40, 36);
    p.warn = rgb(140, 92, 0);
    p.focus = rgb(26, 26, 26);
    p.select = rgb(219, 233, 220);
    p.select_text = rgb(26, 26, 26);
    return p;
}

Palette dark_palette() {
    Palette p{};
    p.window = rgb(32, 32, 32);
    p.panel = rgb(26, 26, 26);
    p.text = rgb(236, 236, 236);
    p.muted = rgb(160, 160, 160);
    p.line = rgb(54, 54, 54);
    p.control = rgb(45, 45, 45);
    p.control_hover = rgb(52, 52, 52);
    p.control_press = rgb(40, 40, 40);
    p.control_line = rgb(64, 64, 64);
    p.flat_hover = rgb(46, 46, 46);
    p.flat_press = rgb(40, 40, 40);
    p.accent = rgb(74, 128, 80);
    p.accent_hover = rgb(84, 142, 90);
    p.accent_press = rgb(64, 112, 70);
    p.accent_text = rgb(255, 255, 255);
    p.field = rgb(38, 38, 38);
    p.field_line = rgb(70, 70, 70);
    p.error = rgb(240, 112, 100);
    p.warn = rgb(224, 180, 90);
    p.focus = rgb(236, 236, 236);
    p.select = rgb(48, 68, 50);
    p.select_text = rgb(236, 236, 236);
    return p;
}

bool system_dark() {
    DWORD v = 1, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return false;
    return v == 0;
}

bool high_contrast() {
    HIGHCONTRASTW hc{sizeof hc};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

Palette contrast_palette() {
    Palette p{};
    p.window = p.panel = p.control = p.control_hover = p.control_press = p.field = GetSysColor(COLOR_WINDOW);
    p.flat_hover = p.flat_press = GetSysColor(COLOR_HIGHLIGHT);
    p.text = p.focus = GetSysColor(COLOR_WINDOWTEXT);
    p.muted = GetSysColor(COLOR_GRAYTEXT);
    p.line = p.control_line = p.field_line = GetSysColor(COLOR_WINDOWTEXT);
    p.accent = p.accent_hover = p.accent_press = GetSysColor(COLOR_HIGHLIGHT);
    p.accent_text = GetSysColor(COLOR_HIGHLIGHTTEXT);
    p.error = p.warn = GetSysColor(COLOR_WINDOWTEXT);
    p.select = p.select_text = CLR_INVALID;
    return p;
}

void rebuild_brushes() {
    if (g_field_brush) DeleteObject(g_field_brush);
    if (g_window_brush) DeleteObject(g_window_brush);
    g_field_brush = CreateSolidBrush(pal.field);
    g_window_brush = CreateSolidBrush(pal.window);
}

bool apply_palette() {
    bool want_dark = g_theme == Theme::Dark || (g_theme == Theme::Auto && system_dark());
    Palette next = high_contrast() ? contrast_palette() : want_dark ? dark_palette() : light_palette();
    bool changed = memcmp(&next, &pal, sizeof pal) != 0 || want_dark != g_dark;
    pal = next;
    g_dark = want_dark && !high_contrast();
    rebuild_brushes();
    return changed;
}

int CALLBACK font_found(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) {
    *reinterpret_cast<bool*>(lp) = true;
    return 0;
}

bool have_font(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, font_found, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

HFONT make_font(const std::wstring& face, int px, int weight) {
    return CreateFontW(-MulDiv(px, g_dpi, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.c_str());
}

void delete_fonts() {
    for (HFONT f : {g_fonts.body, g_fonts.body_strong, g_fonts.small, g_fonts.overline, g_fonts.title, g_fonts.big, g_fonts.mono})
        if (f) DeleteObject(f);
    g_fonts = {};
}

template <class Fn>
Fn proc(const wchar_t* dll, const char* name) {
    HMODULE m = GetModuleHandleW(dll);
    if (!m) m = LoadLibraryW(dll);
    if (!m) return nullptr;
    FARPROC raw = GetProcAddress(m, name);
    Fn fn = nullptr;
    memcpy(&fn, &raw, sizeof fn);
    return fn;
}

void path_round(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    if (r <= 0.5f) {
        p.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    float d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

Gdiplus::Color gc(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

LRESULT CALLBACK button_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_LBUTTONDBLCLK:
            // Owner-drawn buttons report a fast second click as a double click. Treat it as a click.
            msg = WM_LBUTTONDOWN;
            break;
        case WM_MOUSEMOVE:
            if (!GetPropW(h, kHover)) {
                SetPropW(h, kHover, reinterpret_cast<HANDLE>(1));
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            RemovePropW(h, kHover);
            InvalidateRect(h, nullptr, FALSE);
            break;
        case WM_ERASEBKGND: return 1;
        case WM_NCDESTROY:
            for (const wchar_t* p : {kKind, kHover, kCheck, kPanel, kBig, kFont}) RemovePropW(h, p);
            RemoveWindowSubclass(h, button_sub, 1);
            break;
        default: break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK header_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PaintBuffer pb(h);
            fill(pb.dc, pb.rc, pal.field);
            int n = static_cast<int>(SendMessageW(h, HDM_GETITEMCOUNT, 0, 0));
            for (int i = 0; i < n; i++) {
                RECT r{};
                SendMessageW(h, HDM_GETITEMRECT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&r));
                wchar_t buf[128] = {};
                HDITEMW it{};
                it.mask = HDI_TEXT;
                it.pszText = buf;
                it.cchTextMax = 127;
                SendMessageW(h, HDM_GETITEMW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&it));
                RECT t = r;
                t.left += S(8);
                t.right -= S(4);
                draw_text(pb.dc, buf, t, g_fonts.small, pal.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            hline(pb.dc, 0, pb.rc.right, pb.rc.bottom - 1, pal.line);
            return 0;
        }
        case WM_NCDESTROY: RemoveWindowSubclass(h, header_sub, 2); break;
        default: break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

}  // namespace

LRESULT list_custom_draw(NMHDR* hdr) {
    if (hdr->code != NM_CUSTOMDRAW || pal.select == CLR_INVALID) return 0;
    wchar_t cls[32] = {};
    GetClassNameW(hdr->hwndFrom, cls, 32);
    if (lstrcmpiW(cls, WC_LISTVIEWW) != 0) return 0;
    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(hdr);
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        int item = static_cast<int>(cd->nmcd.dwItemSpec);
        if (ListView_GetItemState(hdr->hwndFrom, item, LVIS_SELECTED)) {
            cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
            cd->clrTextBk = pal.select;
            cd->clrText = pal.select_text;
        } else {
            cd->clrTextBk = pal.field;
            cd->clrText = pal.text;
        }
    }
    return CDRF_DODEFAULT;
}

void startup() {
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&g_gdiplus, &in, nullptr);
    if (have_font(L"Segoe UI Variable Text")) g_face = L"Segoe UI Variable Text";
    if (have_font(L"Segoe UI Variable Display")) g_face_display = L"Segoe UI Variable Display";
    else g_face_display = g_face;
    if (have_font(L"Cascadia Mono")) g_face_mono = L"Cascadia Mono";
    apply_palette();
    rebuild_fonts();
}

void shutdown() {
    delete_fonts();
    if (g_field_brush) DeleteObject(g_field_brush);
    if (g_window_brush) DeleteObject(g_window_brush);
    g_field_brush = g_window_brush = nullptr;
    if (g_gdiplus) Gdiplus::GdiplusShutdown(g_gdiplus);
    g_gdiplus = 0;
}

void set_theme(Theme t) {
    g_theme = t;
    apply_palette();
}

bool dark() { return g_dark; }
bool refresh_theme() { return apply_palette(); }

void apply_frame(HWND w) {
    using SetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto set = proc<SetAttr>(L"dwmapi.dll", "DwmSetWindowAttribute");
    if (!set) return;
    BOOL on = g_dark ? TRUE : FALSE;
    if (FAILED(set(w, 20, &on, sizeof on))) set(w, 19, &on, sizeof on);
}

int dpi() { return g_dpi; }
void set_dpi(int d) { g_dpi = d > 0 ? d : 96; }
int S(int px) { return MulDiv(px, g_dpi, 96); }

int window_dpi(HWND w) {
    using Fn = UINT(WINAPI*)(HWND);
    auto fn = proc<Fn>(L"user32.dll", "GetDpiForWindow");
    if (fn && w) return static_cast<int>(fn(w));
    HDC dc = GetDC(w);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(w, dc);
    return d > 0 ? d : 96;
}

const Fonts& fonts() { return g_fonts; }

void rebuild_fonts() {
    delete_fonts();
    g_fonts.body = make_font(g_face, 14, FW_NORMAL);
    g_fonts.body_strong = make_font(g_face, 14, FW_SEMIBOLD);
    g_fonts.small = make_font(g_face, 12, FW_NORMAL);
    g_fonts.overline = make_font(g_face, 13, FW_NORMAL);
    g_fonts.title = make_font(g_face_display, 28, FW_SEMIBOLD);
    g_fonts.big = make_font(g_face, 16, FW_SEMIBOLD);
    g_fonts.mono = make_font(g_face_mono, 13, FW_NORMAL);
}

int text_width(HFONT f, const std::wstring& s) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return sz.cx;
}

int line_height(HFONT f) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return tm.tmHeight;
}

HWND button(HWND parent, int id, const wchar_t* text, Kind kind, bool on_panel, bool big) {
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SetPropW(b, kKind, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(static_cast<int>(kind) + 1)));
    if (on_panel) SetPropW(b, kPanel, reinterpret_cast<HANDLE>(1));
    if (big) SetPropW(b, kBig, reinterpret_cast<HANDLE>(1));
    SetWindowSubclass(b, button_sub, 1, 0);
    return b;
}

static Kind kind_of(HWND b) {
    auto v = reinterpret_cast<INT_PTR>(GetPropW(b, kKind));
    return v ? static_cast<Kind>(v - 1) : Kind::Normal;
}

bool is_button(HWND b) { return b && GetPropW(b, kKind) != nullptr; }
bool is_check(HWND b) { return is_button(b) && kind_of(b) == Kind::Check; }

void set_button_kind(HWND b, Kind kind) {
    if (!b) return;
    SetPropW(b, kKind, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(static_cast<int>(kind) + 1)));
    InvalidateRect(b, nullptr, FALSE);
}

void set_checked(HWND b, bool on) {
    if (on) SetPropW(b, kCheck, reinterpret_cast<HANDLE>(1));
    else RemovePropW(b, kCheck);
    InvalidateRect(b, nullptr, FALSE);
}

bool checked(HWND b) { return GetPropW(b, kCheck) != nullptr; }

int button_width(HWND b) {
    wchar_t buf[256];
    GetWindowTextW(b, buf, 256);
    Kind k = kind_of(b);
    HFONT f = GetPropW(b, kBig) ? g_fonts.big : k == Kind::Primary ? g_fonts.body_strong : g_fonts.body;
    int tw = text_width(f, buf);
    switch (k) {
        case Kind::Flat: return tw + S(24);
        case Kind::Check: return tw + S(18 + 8 + 4);
        default: return std::max(S(96), tw + S(40));
    }
}

bool draw_item(const DRAWITEMSTRUCT* d) {
    if (d->CtlType != ODT_BUTTON || !is_button(d->hwndItem)) return false;
    HWND b = d->hwndItem;
    RECT r = d->rcItem;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return true;
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ old = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    RECT a{0, 0, w, h};
    fill(dc, a, GetPropW(b, kPanel) ? pal.panel : pal.window);

    Kind k = kind_of(b);
    bool hover = GetPropW(b, kHover) != nullptr;
    bool press = (d->itemState & ODS_SELECTED) != 0;
    bool off = (d->itemState & ODS_DISABLED) != 0;
    bool focus = keyboard_cues && (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);
    wchar_t text[256];
    GetWindowTextW(b, text, 256);
    int radius = S(4);
    UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;

    switch (k) {
        case Kind::Primary: {
            COLORREF bg = off ? pal.control : press ? pal.accent_press : hover ? pal.accent_hover : pal.accent;
            round_rect(dc, a, radius, bg, off ? pal.control_line : CLR_INVALID);
            draw_text(dc, text, a, GetPropW(b, kBig) ? g_fonts.big : g_fonts.body_strong, off ? pal.muted : pal.accent_text, flags);
            break;
        }
        case Kind::Normal: {
            COLORREF bg = off ? pal.control : press ? pal.control_press : hover ? pal.control_hover : pal.control;
            round_rect(dc, a, radius, bg, pal.control_line);
            draw_text(dc, text, a, g_fonts.body, off ? pal.muted : pal.text, flags);
            break;
        }
        case Kind::Flat: {
            if ((hover || press) && !off) round_rect(dc, a, radius, press ? pal.flat_press : pal.flat_hover, CLR_INVALID);
            draw_text(dc, text, a, g_fonts.body, off ? pal.muted : pal.text, flags);
            break;
        }
        case Kind::Check: {
            int box = S(18);
            RECT br{S(1), (h - box) / 2, S(1) + box, (h - box) / 2 + box};
            bool on = checked(b);
            if (on) round_rect(dc, br, S(3), off ? pal.muted : (hover ? pal.accent_hover : pal.accent), CLR_INVALID);
            else round_rect(dc, br, S(3), hover && !off ? pal.control_hover : pal.field, off ? pal.line : pal.muted);
            if (on) {
                Gdiplus::Graphics g(dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::Pen pen(gc(pal.accent_text), std::max(1.5f, S(2) * 0.85f));
                pen.SetLineJoin(Gdiplus::LineJoinRound);
                pen.SetStartCap(Gdiplus::LineCapRound);
                pen.SetEndCap(Gdiplus::LineCapRound);
                float x = static_cast<float>(br.left), y = static_cast<float>(br.top), s = static_cast<float>(box);
                Gdiplus::PointF pts[3] = {{x + s * 0.24f, y + s * 0.52f}, {x + s * 0.43f, y + s * 0.70f}, {x + s * 0.77f, y + s * 0.32f}};
                g.DrawLines(&pen, pts, 3);
            }
            RECT t{br.right + S(8), 0, w, h};
            draw_text(dc, text, t, g_fonts.body, off ? pal.muted : pal.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (focus) {
                RECT fr{0, static_cast<LONG>(br.top - S(2)), std::min<LONG>(w, t.left + text_width(g_fonts.body, text) + S(4)), br.bottom + S(2)};
                round_rect(dc, fr, S(4), CLR_INVALID, pal.focus, std::max(1, S(1)));
                focus = false;
            }
            break;
        }
    }
    if (focus) round_rect(dc, a, radius, CLR_INVALID, pal.focus, std::max(2, S(2)));
    BitBlt(d->hDC, r.left, r.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return true;
}

HWND edit(HWND parent, int id, DWORD style, FontRole role) {
    HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | style, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (role != FontRole::Body) SetPropW(e, kFont, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(role)));
    return e;
}

HWND list(HWND parent, int id, DWORD ex) {
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0,
                              10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | ex);
    HWND hdr = ListView_GetHeader(lv);
    if (hdr) SetWindowSubclass(hdr, header_sub, 2, 0);
    style_list(lv);
    return lv;
}

void style_list(HWND lv) {
    SetWindowTheme(lv, g_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    if (HWND hdr = ListView_GetHeader(lv)) SetWindowTheme(hdr, g_dark ? L"DarkMode_ItemsView" : L"ItemsView", nullptr);
    ListView_SetBkColor(lv, pal.field);
    ListView_SetTextBkColor(lv, pal.field);
    ListView_SetTextColor(lv, pal.text);
    InvalidateRect(lv, nullptr, TRUE);
}

void place_field(HWND e, const RECT& f) {
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(e, WM_GETFONT, 0, 0));
    int th = line_height(font ? font : g_fonts.body);
    int h = f.bottom - f.top;
    int y = f.top + std::max(0, (h - th) / 2);
    SetPropW(e, kFrame, reinterpret_cast<HANDLE>(1));
    MoveWindow(e, f.left + S(10), y, std::max(1, static_cast<int>(f.right - f.left) - S(20)), th, TRUE);
    // Remember the frame relative to the edit so the parent can draw it.
    SetPropW(e, L"ui.fx", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(S(10))));
    SetPropW(e, L"ui.fy", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(y - f.top)));
    SetPropW(e, L"ui.fh", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(h)));
}

void place_framed(HWND c, const RECT& f) {
    int b = std::max(1, S(1));
    SetPropW(c, kFrame, reinterpret_cast<HANDLE>(2));
    MoveWindow(c, f.left + b, f.top + b, std::max(1, static_cast<int>(f.right - f.left) - 2 * b), std::max(1, static_cast<int>(f.bottom - f.top) - 2 * b),
               TRUE);
}

void paint_frames(HWND parent, HDC dc) {
    struct Ctx {
        HWND parent;
        HDC dc;
    } ctx{parent, dc};
    EnumChildWindows(
        parent,
        [](HWND c, LPARAM lp) -> BOOL {
            auto* x = reinterpret_cast<Ctx*>(lp);
            if (GetParent(c) != x->parent || !IsWindowVisible(c)) return TRUE;
            auto kind = reinterpret_cast<INT_PTR>(GetPropW(c, kFrame));
            if (!kind) return TRUE;
            RECT r{};
            GetWindowRect(c, &r);
            MapWindowPoints(nullptr, x->parent, reinterpret_cast<POINT*>(&r), 2);
            bool focused = GetFocus() == c;
            bool enabled = IsWindowEnabled(c) != FALSE;
            if (kind == 1) {
                int fx = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(c, L"ui.fx")));
                int fy = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(c, L"ui.fy")));
                int fh = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(c, L"ui.fh")));
                RECT f{r.left - fx, r.top - fy, r.right + fx, r.top - fy + fh};
                round_rect(x->dc, f, S(4), enabled ? pal.field : pal.window, pal.field_line);
                if (focused) {
                    RECT u{f.left + S(1), f.bottom - S(2), f.right - S(1), f.bottom};
                    fill(x->dc, u, pal.accent);
                }
            } else {
                int b = std::max(1, S(1));
                RECT f{r.left - b, r.top - b, r.right + b, r.bottom + b};
                fill(x->dc, f, pal.field_line);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
}

LRESULT ctl_color(UINT msg, WPARAM wp, LPARAM lp) {
    HDC dc = reinterpret_cast<HDC>(wp);
    HWND c = reinterpret_cast<HWND>(lp);
    bool enabled = IsWindowEnabled(c) != FALSE;
    SetTextColor(dc, enabled ? pal.text : pal.muted);
    if (msg == WM_CTLCOLORSTATIC && !GetPropW(c, kFrame) && !(GetWindowLongW(c, GWL_STYLE) & ES_READONLY)) {
        SetBkColor(dc, pal.window);
        return reinterpret_cast<LRESULT>(g_window_brush);
    }
    SetBkColor(dc, pal.field);
    return reinterpret_cast<LRESULT>(g_field_brush);
}

void fill(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void hline(HDC dc, int x0, int x1, int y, COLORREF c) {
    RECT r{x0, y, x1, y + std::max(1, S(1))};
    fill(dc, r, c);
}

void round_rect(HDC dc, const RECT& r, int radius, COLORREF fillc, COLORREF border, int bw) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    float x = static_cast<float>(r.left), y = static_cast<float>(r.top);
    float w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
    radius = std::min(radius, static_cast<int>(std::min(w, h) / 2));
    if (fillc != CLR_INVALID) {
        Gdiplus::GraphicsPath p;
        path_round(p, x, y, w, h, static_cast<float>(radius));
        Gdiplus::SolidBrush brush(gc(fillc));
        g.FillPath(&brush, &p);
    }
    if (border != CLR_INVALID) {
        float half = bw / 2.0f;
        Gdiplus::GraphicsPath p;
        path_round(p, x + half, y + half, w - bw, h - bw, std::max(0.0f, radius - half));
        Gdiplus::Pen pen(gc(border), static_cast<float>(bw));
        g.DrawPath(&pen, &p);
    }
}

void draw_text(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT flags) {
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, flags);
    SelectObject(dc, old);
}

void draw_bitmap(HDC dc, const RECT& r, const uint8_t* bgra, int w, int h) {
    if (!bgra || w <= 0 || h <= 0) return;
    std::vector<uint8_t> copy(bgra, bgra + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    Gdiplus::Bitmap src(w, h, w * 4, PixelFormat32bppARGB, copy.data());
    Gdiplus::Graphics g(dc);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::ImageAttributes attr;
    attr.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    g.DrawImage(&src, Gdiplus::Rect(r.left, r.top, r.right - r.left, r.bottom - r.top), 0, 0, w, h, Gdiplus::UnitPixel, &attr);
}

PaintBuffer::PaintBuffer(HWND w) : hwnd_(w) {
    win_ = BeginPaint(w, &ps_);
    GetClientRect(w, &rc);
    dc = CreateCompatibleDC(win_);
    bmp_ = CreateCompatibleBitmap(win_, std::max(1L, rc.right), std::max(1L, rc.bottom));
    old_ = SelectObject(dc, bmp_);
    SetBkMode(dc, TRANSPARENT);
}

PaintBuffer::~PaintBuffer() {
    BitBlt(win_, ps_.rcPaint.left, ps_.rcPaint.top, ps_.rcPaint.right - ps_.rcPaint.left, ps_.rcPaint.bottom - ps_.rcPaint.top, dc, ps_.rcPaint.left,
           ps_.rcPaint.top, SRCCOPY);
    SelectObject(dc, old_);
    DeleteObject(bmp_);
    DeleteDC(dc);
    EndPaint(hwnd_, &ps_);
}

void set_fonts(HWND top) {
    EnumChildWindows(
        top,
        [](HWND c, LPARAM) -> BOOL {
            auto role = static_cast<FontRole>(reinterpret_cast<INT_PTR>(GetPropW(c, kFont)));
            HFONT f = role == FontRole::Mono ? g_fonts.mono : role == FontRole::Small ? g_fonts.small : g_fonts.body;
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
            return TRUE;
        },
        0);
}

void center_on(HWND w, HWND owner) {
    RECT o{}, r{};
    if (owner) GetWindowRect(owner, &o);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &o, 0);
    GetWindowRect(w, &r);
    int x = o.left + ((o.right - o.left) - (r.right - r.left)) / 2;
    int y = o.top + ((o.bottom - o.top) - (r.bottom - r.top)) / 2;
    HMONITOR mon = MonitorFromWindow(owner ? owner : w, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi};
    if (GetMonitorInfoW(mon, &mi)) {
        x = std::max(static_cast<int>(mi.rcWork.left), std::min(x, static_cast<int>(mi.rcWork.right - (r.right - r.left))));
        y = std::max(static_cast<int>(mi.rcWork.top), std::min(y, static_cast<int>(mi.rcWork.bottom - (r.bottom - r.top))));
    }
    SetWindowPos(w, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void run_modal(HWND dlg, HWND owner) {
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    MSG m;
    bool quit = false;
    WPARAM code = 0;
    while (IsWindow(dlg)) {
        BOOL r = GetMessageW(&m, nullptr, 0, 0);
        if (r == 0) {
            quit = true;
            code = m.wParam;
            break;
        }
        if (r < 0) break;
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && (m.hwnd == dlg || IsChild(dlg, m.hwnd))) {
            SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
            continue;
        }
        note_key(m);
        if (IsDialogMessageW(dlg, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (IsWindow(dlg)) end_modal(dlg);
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (quit) PostQuitMessage(static_cast<int>(code));
}

void end_modal(HWND dlg) {
    HWND owner = GetWindow(dlg, GW_OWNER);
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
}

void retheme_all() {
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND w, LPARAM) -> BOOL {
            apply_frame(w);
            EnumChildWindows(
                w,
                [](HWND c, LPARAM) -> BOOL {
                    wchar_t cls[64];
                    if (GetClassNameW(c, cls, 64) && _wcsicmp(cls, WC_LISTVIEWW) == 0) style_list(c);
                    return TRUE;
                },
                0);
            RedrawWindow(w, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
            return TRUE;
        },
        0);
}

}  // namespace ui

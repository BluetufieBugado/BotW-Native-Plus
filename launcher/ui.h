// Drawing and control helpers for the launcher: colors for light and dark mode, fonts per DPI,
// flat owner-drawn buttons, framed edits and lists, and a small modal loop.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace ui {

struct Palette {
    COLORREF window, panel, text, muted, line;
    COLORREF control, control_hover, control_press, control_line;
    COLORREF flat_hover, flat_press;
    COLORREF accent, accent_hover, accent_press, accent_text;
    COLORREF field, field_line, error, warn, focus;
    COLORREF select, select_text;  // CLR_INVALID keeps the system list highlight
};

enum class Theme { Auto, Light, Dark };

extern Palette pal;
// Focus rings are drawn only after Tab or an arrow key has been used.
extern bool keyboard_cues;
void note_key(const MSG& m);

void startup();
void shutdown();

void set_theme(Theme t);
bool dark();
// Re-reads the Windows app mode when the theme is Auto. True if the palette changed.
bool refresh_theme();
// Dark or light title bar to match.
void apply_frame(HWND w);

int dpi();
void set_dpi(int dpi);
int S(int px);
int window_dpi(HWND w);

struct Fonts {
    HFONT body = nullptr;
    HFONT body_strong = nullptr;
    HFONT small = nullptr;
    HFONT overline = nullptr;
    HFONT title = nullptr;
    HFONT big = nullptr;
    HFONT mono = nullptr;
};
const Fonts& fonts();
void rebuild_fonts();
int text_width(HFONT f, const std::wstring& s);
int line_height(HFONT f);

enum class Kind { Normal, Primary, Flat, Check };
HWND button(HWND parent, int id, const wchar_t* text, Kind kind, bool on_panel = false, bool big = false);
bool is_check(HWND b);
void set_button_kind(HWND b, Kind kind);
void set_checked(HWND b, bool on);
bool checked(HWND b);
bool is_button(HWND b);
int button_width(HWND b);
bool draw_item(const DRAWITEMSTRUCT* d);

enum class FontRole { Body = 0, Mono = 1, Small = 2 };
HWND edit(HWND parent, int id, DWORD style = 0, FontRole role = FontRole::Body);
HWND list(HWND parent, int id, DWORD lv_ex_style);
void style_list(HWND lv);
// Places an edit inside a 32 px field frame, or any child inside a 1 px frame.
void place_field(HWND edit, const RECT& frame);
void place_framed(HWND child, const RECT& frame);
void paint_frames(HWND parent, HDC dc);

LRESULT ctl_color(UINT msg, WPARAM wp, LPARAM lp);

void fill(HDC dc, const RECT& r, COLORREF c);
// Paints selected list rows in the quiet selection colour. Returns 0 when it does nothing.
LRESULT list_custom_draw(NMHDR* hdr);
void hline(HDC dc, int x0, int x1, int y, COLORREF c);
void round_rect(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border, int border_w = 1);
void draw_text(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT flags);
void draw_bitmap(HDC dc, const RECT& r, const uint8_t* bgra, int w, int h);

// Double-buffered WM_PAINT.
class PaintBuffer {
public:
    explicit PaintBuffer(HWND w);
    ~PaintBuffer();
    PaintBuffer(const PaintBuffer&) = delete;
    PaintBuffer& operator=(const PaintBuffer&) = delete;
    HDC dc = nullptr;
    RECT rc{};

private:
    HWND hwnd_;
    PAINTSTRUCT ps_{};
    HDC win_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
};

void set_fonts(HWND top);
void center_on(HWND w, HWND owner);
// Runs until dlg is destroyed. Tab, Enter (IDOK) and Esc (IDCANCEL) go through IsDialogMessage.
void run_modal(HWND dlg, HWND owner);
void end_modal(HWND dlg);
// Re-applies colors to every window of this thread after a theme change.
void retheme_all();

}  // namespace ui

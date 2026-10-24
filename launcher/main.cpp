// Small Win32 launcher for BotWRecomp. It is not linked into the game. SDL3 is only used to
// name keys and controller buttons the same way the game reads controls.ini.
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include "game_info.h"
#include "input/controls_file.h"
#include "mods.h"
#include "save_import.h"
#include "ui.h"
#include "unzip.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef BIF_NONEWFOLDERBUTTON
#define BIF_NONEWFOLDERBUTTON 0x00000200
#endif
#ifndef BIF_BROWSEINCLUDEFILES
#define BIF_BROWSEINCLUDEFILES 0x00004000
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

using ui::S;

namespace {

const wchar_t* kAppTitle = L"BotW Recomp Launcher";

// Child ids start at 100 so IDOK and IDCANCEL stay free for Enter and Esc.
enum {
    ID_PLAY = 100,
    ID_SETTINGS,
    ID_CONTROLS_OPEN,
    ID_SAVES_OPEN,
    ID_MODS_OPEN,
    ID_USER_OPEN,
    ID_LOG_OPEN,
    ID_GAME,
    ID_GAME_BROWSE,
    ID_EXE,
    ID_EXE_BROWSE,
    ID_USER,
    ID_USER_BROWSE,
    ID_SECONDS,
    ID_FULLSCREEN,
    ID_GAMEPAD_VIEW,
    ID_SAVE_SETTINGS,
    ID_CANCEL_SETTINGS,
    ID_CONTROLS,
    ID_SET_KEY,
    ID_SET_PAD,
    ID_RESET,
    ID_CLOSE,
    ID_SAVES,
    ID_IMPORT,
    ID_SWITCH,
    ID_DELETE,
    ID_OPEN,
    ID_REFRESH,
    ID_MIGRATE,
    ID_MODS,
    ID_MOD_UP,
    ID_MOD_DOWN,
    ID_MODS_FOLDER,
    ID_MODS_REFRESH,
    ID_NO_MODS,
    ID_LOG_TEXT,
    ID_LOG_FILE,
};

struct Settings {
    std::wstring game, exe, user, seconds = L"0", theme = L"auto";
    bool fullscreen = false;
    bool gamepad_view = false;
    bool no_mods = false;
    std::vector<std::string> other;  // lines this version does not know, written back as they were
};

enum class Tone { Normal, Good, Warn, Error };

struct Window;
struct SavesWindow;

struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    Settings cfg;
    gameinfo::Info info;
    mods::State mods;
    std::wstring status;
    Tone tone = Tone::Normal;
    bool sdl = false;
    bool in_capture = false;
    bool was_running = false;
    std::vector<SDL_Gamepad*> pads;
    std::vector<controlsfile::Binding> rows;
    std::vector<saveimport::SetInfo> sets;
    SavesWindow* saves_win = nullptr;
};

App g;

std::wstring get_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return {};
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(wcslen(s.c_str()));
    return s;
}

void set_text(HWND h, const std::wstring& s) { SetWindowTextW(h, s.c_str()); }

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

HWND alert_owner() {
    HWND a = GetActiveWindow();
    return a ? a : g.hwnd;
}

void alert(const std::wstring& text, UINT flags = MB_OK | MB_ICONWARNING) { MessageBoxW(alert_owner(), text.c_str(), kAppTitle, flags); }

int ask(const std::wstring& text, UINT flags) { return MessageBoxW(alert_owner(), text.c_str(), kAppTitle, flags); }

std::filesystem::path exe_dir() {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    if (!n || n >= 32768) return {};
    return std::filesystem::path(buf).parent_path();
}

std::filesystem::path launcher_ini() { return exe_dir() / L"BotWLauncher.ini"; }

std::wstring appdata_root() {
    wchar_t buf[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf) != S_OK) return L"BotWRecomp";
    return std::wstring(buf) + L"\\BotWRecomp";
}

std::filesystem::path user_root() {
    if (g.cfg.user.empty()) return {};
    return std::filesystem::path(g.cfg.user);
}

bool has_quote(const std::wstring& s) { return s.find(L'"') != std::wstring::npos; }

std::wstring pick_browse(HWND owner, const wchar_t* title, bool files_ok) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON | BIF_EDITBOX;
    if (files_ok) bi.ulFlags |= BIF_BROWSEINCLUDEFILES;
    else bi.ulFlags |= BIF_RETURNONLYFSDIRS;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return {};
    wchar_t path[32768];
    path[0] = 0;
    using ExFn = BOOL(WINAPI*)(PCIDLIST_ABSOLUTE, PWSTR, DWORD, DWORD);
    FARPROC raw = GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHGetPathFromIDListEx");
    ExFn ex = nullptr;
    memcpy(&ex, &raw, sizeof ex);
    BOOL ok = ex ? ex(pidl, path, 32768, 0) : SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    return ok ? std::wstring(path) : std::wstring();
}

std::wstring pick_exe(HWND owner, const std::wstring& cur) {
    wchar_t buf[32768];
    buf[0] = 0;
    if (cur.size() < 32767) wcsncpy(buf, cur.c_str(), 32767);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Programs\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 32768;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"BotWRecomp.exe";
    if (!GetOpenFileNameW(&ofn)) return {};
    return buf;
}

bool process_running(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

bool game_running() { return process_running(L"BotWRecomp.exe"); }

bool read_user_gamepad_view(const std::filesystem::path& root, bool fallback) {
    if (root.empty()) return fallback;
    std::ifstream in(root / "settings.ini");
    if (!in) return fallback;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("gamepad_view=", 0) == 0) return line.substr(13) != "0";
    }
    return fallback;
}

void write_user_gamepad_view(const std::filesystem::path& root, bool on) {
    if (root.empty()) return;
    std::filesystem::path p = root / "settings.ini";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return;
    std::ifstream in(p);
    if (!in) return;
    std::vector<std::string> lines;
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("gamepad_view=", 0) == 0) {
            lines.push_back(std::string("gamepad_view=") + (on ? "1" : "0"));
            found = true;
        } else {
            lines.push_back(line);
        }
    }
    in.close();
    if (!found) lines.push_back(std::string("gamepad_view=") + (on ? "1" : "0"));
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return;
    for (const auto& l : lines) out << l << "\n";
}

void save_launcher_ini() {
    std::ofstream out(launcher_ini(), std::ios::binary | std::ios::trunc);
    if (!out) return;
    auto line = [&](const char* k, const std::wstring& v) { out << k << "=" << narrow(v) << "\n"; };
    line("game", g.cfg.game);
    line("exe", g.cfg.exe);
    line("user", g.cfg.user);
    line("seconds", g.cfg.seconds);
    line("fullscreen", g.cfg.fullscreen ? L"1" : L"0");
    line("gamepad_view", g.cfg.gamepad_view ? L"1" : L"0");
    line("no_mods", g.cfg.no_mods ? L"1" : L"0");
    line("theme", g.cfg.theme);
    for (auto& l : g.cfg.other) out << l << "\n";
}

void load_launcher_ini() {
    std::ifstream in(launcher_ini());
    Settings s;
    s.user = appdata_root();
    std::string line;
    bool saw_gpv = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "game") s.game = widen(v);
        else if (k == "exe") s.exe = widen(v);
        else if (k == "user") {
            if (!v.empty()) s.user = widen(v);
        } else if (k == "seconds") s.seconds = widen(v);
        else if (k == "fullscreen") s.fullscreen = v == "1";
        else if (k == "gamepad_view") { s.gamepad_view = v == "1"; saw_gpv = true; }
        else if (k == "no_mods") s.no_mods = v == "1";
        else if (k == "theme") s.theme = widen(v);
        else s.other.push_back(line);
    }
    s.gamepad_view = read_user_gamepad_view(std::filesystem::path(s.user), saw_gpv ? s.gamepad_view : false);
    if (s.exe.empty()) {
        auto guess = exe_dir() / L"BotWRecomp.exe";
        std::error_code ec;
        if (std::filesystem::is_regular_file(guess, ec)) s.exe = guess.wstring();
    }
    if (s.seconds.empty()) s.seconds = L"0";
    g.cfg = s;
}

ui::Theme theme_pref() {
    if (_wcsicmp(g.cfg.theme.c_str(), L"dark") == 0) return ui::Theme::Dark;
    if (_wcsicmp(g.cfg.theme.c_str(), L"light") == 0) return ui::Theme::Light;
    return ui::Theme::Auto;
}

// Every window here is one Window object behind one window procedure. Dialogs are modal.
struct Window {
    HWND hwnd = nullptr;
    bool is_main = false;
    int bar = 0;  // bottom button bar height at 96 dpi, 0 for none
    int min_w = 0, min_h = 0;
    virtual ~Window() = default;
    virtual void create() = 0;
    virtual void layout(int w, int h) = 0;
    virtual void paint(HDC, const RECT&) {}
    virtual void command(int, int, HWND) {}
    virtual LRESULT notify(NMHDR*) { return 0; }
    virtual void ok() {}
    virtual void cancel() { close(); }
    virtual void on_close() { cancel(); }
    virtual void dropped(const std::wstring&) {}
    virtual void timer() {}
    void close() {
        if (is_main) DestroyWindow(hwnd);
        else ui::end_modal(hwnd);
    }
    void relayout() {
        if (!hwnd) return;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        layout(rc.right, rc.bottom);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
};

void on_theme_change() {
    if (ui::refresh_theme()) ui::retheme_all();
}

void apply_dpi_everywhere(int dpi) {
    if (dpi == ui::dpi()) return;
    ui::set_dpi(dpi);
    ui::rebuild_fonts();
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND w, LPARAM) -> BOOL {
            ui::set_fonts(w);
            return TRUE;
        },
        0);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* w = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        w = static_cast<Window*>(cs->lpCreateParams);
        w->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
    }
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_CREATE:
            ui::apply_frame(hwnd);
            w->create();
            ui::set_fonts(hwnd);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            ui::PaintBuffer pb(hwnd);
            ui::fill(pb.dc, pb.rc, ui::pal.window);
            if (w->bar) {
                RECT b = pb.rc;
                b.top = b.bottom - S(w->bar);
                ui::fill(pb.dc, b, ui::pal.panel);
                ui::hline(pb.dc, 0, b.right, b.top, ui::pal.line);
            }
            ui::paint_frames(hwnd, pb.dc);
            w->paint(pb.dc, pb.rc);
            return 0;
        }
        case WM_SIZE:
            w->layout(LOWORD(lp), HIWORD(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            if (!w->min_w) break;
            RECT r{0, 0, S(w->min_w), S(w->min_h)};
            AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE)), FALSE,
                               static_cast<DWORD>(GetWindowLongW(hwnd, GWL_EXSTYLE)));
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = r.right - r.left;
            mm->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }
        case WM_DPICHANGED: {
            apply_dpi_everywhere(HIWORD(wp));
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            w->relayout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) on_theme_change();
            break;
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE: on_theme_change(); break;
        case WM_DRAWITEM:
            if (ui::draw_item(reinterpret_cast<DRAWITEMSTRUCT*>(lp))) return TRUE;
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: return ui::ctl_color(msg, wp, lp);
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) InvalidateRect(hwnd, nullptr, FALSE);
            if (id == IDOK && !ctl) {
                HWND f = GetFocus();
                if (f && GetParent(f) == hwnd && ui::is_button(f)) SendMessageW(f, BM_CLICK, 0, 0);
                else w->ok();
                return 0;
            }
            if (id == IDCANCEL && !ctl) {
                w->cancel();
                return 0;
            }
            if (ctl && code == BN_CLICKED && ui::is_check(ctl)) ui::set_checked(ctl, !ui::checked(ctl));
            w->command(id, code, ctl);
            return 0;
        }
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (LRESULT r = ui::list_custom_draw(hdr)) return r;
            return w->notify(hdr);
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            wchar_t path[32768];
            std::wstring p;
            if (DragQueryFileW(drop, 0, path, 32768)) p = path;
            DragFinish(drop);
            if (!p.empty()) w->dropped(p);
            return 0;
        }
        case WM_TIMER: w->timer(); return 0;
        case WM_CLOSE: w->on_close(); return 0;
        case WM_DESTROY:
            if (w->is_main) {
                KillTimer(hwnd, 1);
                PostQuitMessage(0);
            }
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            w->hwnd = nullptr;
            break;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

const wchar_t* kMainClass = L"BotWLauncher";
const wchar_t* kDialogClass = L"BotWLauncherDialog";

void register_classes() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(g.inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);
    wc.lpszClassName = kDialogClass;
    RegisterClassExW(&wc);
}

HWND make_window(Window* w, const wchar_t* cls, const wchar_t* title, int cw, int ch, bool resizable, HWND owner) {
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    if (resizable) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    if (!owner) style |= WS_MINIMIZEBOX;
    DWORD ex = WS_EX_CONTROLPARENT | (owner ? WS_EX_DLGMODALFRAME : 0);
    RECT r{0, 0, S(cw), S(ch)};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    HWND h = CreateWindowExW(ex, cls, title, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, owner, nullptr, g.inst, w);
    if (h && owner) ui::center_on(h, owner);
    return h;
}

void run_dialog(Window* w, const wchar_t* title, int cw, int ch, bool resizable) {
    HWND owner = GetActiveWindow();
    if (!owner) owner = g.hwnd;
    HWND h = make_window(w, kDialogClass, title, cw, ch, resizable, owner);
    if (!h) return;
    ui::run_modal(h, owner);
}

void add_columns(HWND list, const wchar_t** names, int n) {
    for (int i = 0; i < n; i++) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<wchar_t*>(names[i]);
        col.cx = 120;
        col.iSubItem = i;
        ListView_InsertColumn(list, i, &col);
    }
}

// Buttons left to right from x.
int place_row(std::initializer_list<HWND> buttons, int x, int y, int h, int gap) {
    for (HWND b : buttons) {
        int bw = ui::button_width(b);
        MoveWindow(b, x, y, bw, h, TRUE);
        x += bw + gap;
    }
    return x;
}

// Buttons right to left from right; the first one ends up rightmost.
int place_row_right(std::initializer_list<HWND> buttons, int right, int y, int h, int gap) {
    for (HWND b : buttons) {
        int bw = ui::button_width(b);
        MoveWindow(b, right - bw, y, bw, h, TRUE);
        right -= bw + gap;
    }
    return right;
}

int text_height(HFONT f, const std::wstring& text, int width) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{0, 0, width, 0};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return r.bottom;
}

void paint_lines(HDC dc, const std::wstring& text, RECT r, HFONT f, COLORREF c) {
    ui::draw_text(dc, text, r, f, c, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
}

void set_status(const std::wstring& s, Tone t = Tone::Normal) {
    g.status = s;
    g.tone = t;
    if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE);
}

void update_play_button();

void refresh_game() {
    g.info = gameinfo::read(std::filesystem::path(g.cfg.game));
    g.mods = mods::load(user_root());
    const auto& i = g.info;
    std::error_code ec;
    bool exe_ok = !g.cfg.exe.empty() && std::filesystem::is_regular_file(std::filesystem::path(g.cfg.exe), ec);
    if (g.cfg.game.empty()) set_status(L"Choose the folder with your game files (code, content and meta) to get started.");
    else if (!i.folder_exists) set_status(L"Game folder not found. Choose it again in Settings.", Tone::Error);
    else if (!i.has_rpx) set_status(L"No code\\U-King.rpx in the game folder. Pick the folder that has code, content and meta.", Tone::Error);
    else if (!exe_ok) set_status(L"BotWRecomp.exe not found. Set it in Settings.", Tone::Error);
    else if (game_running()) set_status(L"BotWRecomp.exe is running. Save import, switch, delete, and migrate are blocked until it closes.");
    else if (saveimport::decimal_legacy_present(user_root()))
        set_status(L"A save from an older build is in save\\2147483649. Open Saves and use Migrate old save.", Tone::Warn);
    else if (!i.has_meta || !i.has_title_version) set_status(L"Game folder found. No meta\\meta.xml, so the update version was not checked.", Tone::Good);
    else if (i.title_version == gameinfo::kExpectedTitleVersion) set_status(L"Game folder found. Update v1.5.0 detected.", Tone::Good);
    else
        set_status(L"Game folder found, but it has update " + widen(gameinfo::version_name(i.title_version)) +
                       L". This build needs v1.5.0.",
                   Tone::Warn);
    update_play_button();
}

// Lists live in their dialogs. These keep g.rows and g.sets current and refresh any open list.
void fill_saves();
int selected_set();
void note_legacy_save();

// Controls capture: a small SDL window takes the next key or button.
const char* token_for_button(SDL_GamepadButton b) {
    switch (b) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "south";
        case SDL_GAMEPAD_BUTTON_EAST: return "east";
        case SDL_GAMEPAD_BUTTON_WEST: return "west";
        case SDL_GAMEPAD_BUTTON_NORTH: return "north";
        case SDL_GAMEPAD_BUTTON_BACK: return "back";
        case SDL_GAMEPAD_BUTTON_START: return "start";
        case SDL_GAMEPAD_BUTTON_GUIDE: return "guide";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "leftstick";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "rightstick";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "leftshoulder";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "rightshoulder";
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "dpup";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "dpdown";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "dpleft";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "dpright";
        case SDL_GAMEPAD_BUTTON_MISC1: return "misc1";
        case SDL_GAMEPAD_BUTTON_MISC2: return "misc2";
        case SDL_GAMEPAD_BUTTON_MISC3: return "misc3";
        case SDL_GAMEPAD_BUTTON_MISC4: return "misc4";
        case SDL_GAMEPAD_BUTTON_MISC5: return "misc5";
        case SDL_GAMEPAD_BUTTON_MISC6: return "misc6";
        case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "touchpad";
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "paddle1";
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "paddle2";
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "paddle3";
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "paddle4";
        default: return nullptr;
    }
}

const char* token_for_axis(int axis, Sint16 value) {
    switch (axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: return value > 0 ? "leftx+" : "leftx-";
        case SDL_GAMEPAD_AXIS_LEFTY: return value < 0 ? "lefty+" : "lefty-";
        case SDL_GAMEPAD_AXIS_RIGHTX: return value > 0 ? "rightx+" : "rightx-";
        case SDL_GAMEPAD_AXIS_RIGHTY: return value < 0 ? "righty+" : "righty-";
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "lefttrigger";
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "righttrigger";
        default: return nullptr;
    }
}

void remember_pad(SDL_JoystickID id) {
    for (auto* p : g.pads)
        if (SDL_GetGamepadID(p) == id) return;
    if (SDL_Gamepad* p = SDL_OpenGamepad(id)) g.pads.push_back(p);
}

void forget_pad(SDL_JoystickID id) {
    for (auto it = g.pads.begin(); it != g.pads.end(); ++it) {
        if (SDL_GetGamepadID(*it) == id) {
            SDL_CloseGamepad(*it);
            g.pads.erase(it);
            return;
        }
    }
}

void pump_sdl() {
    if (!g.sdl || g.in_capture) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) remember_pad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) forget_pad(e.gdevice.which);
    }
}

enum class CaptureSide { Key, Pad };

bool capture_binding(HWND owner, int row, CaptureSide side) {
    if (!g.sdl) {
        alert(L"SDL did not start, so keys and controller buttons cannot be captured.");
        return false;
    }
    const auto* action = controlsfile::find_action(g.rows[static_cast<size_t>(row)].id);
    std::string label = action ? action->label : g.rows[static_cast<size_t>(row)].id;
    std::string title = std::string(side == CaptureSide::Key ? "Key for " : "Controller for ") + label + "  -  Esc cancels, Backspace clears";
    g.in_capture = true;
    EnableWindow(owner, FALSE);
    SDL_Window* sw = SDL_CreateWindow(title.c_str(), 640, 180, SDL_WINDOW_UTILITY | SDL_WINDOW_ALWAYS_ON_TOP);
    SDL_Renderer* renderer = sw ? SDL_CreateRenderer(sw, nullptr) : nullptr;
    if (sw) SDL_SetWindowPosition(sw, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    const char* line1 = side == CaptureSide::Key ? "Press a key. Esc cancels. Backspace clears."
                                                  : "Press a button, trigger, or stick. Esc cancels. Backspace clears.";
    const char* line2 = "Release it first if you are already holding it.";
    bool done = false, accept = false, clear = false;
    std::string key, pad;
    struct Arm {
        SDL_Gamepad* pad = nullptr;
        bool axis_ready[8]{};
        bool button_was[32]{};
        bool primed = false;
    };
    std::vector<Arm> arms;
    while (!done) {
        if (renderer) {
            SDL_SetRenderDrawColor(renderer, 24, 26, 32, 255);
            SDL_RenderClear(renderer);
            SDL_SetRenderDrawColor(renderer, 236, 236, 236, 255);
            SDL_RenderDebugText(renderer, 16, 24, line1);
            SDL_RenderDebugText(renderer, 16, 48, line2);
            SDL_RenderPresent(renderer);
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) done = true;
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED) remember_pad(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) forget_pad(e.gdevice.which);
            else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) done = true;
                else if (e.key.scancode == SDL_SCANCODE_BACKSPACE) {
                    clear = true;
                    done = true;
                } else if (side == CaptureSide::Key) {
                    const char* name = SDL_GetScancodeName(e.key.scancode);
                    if (name && name[0]) {
                        key = name;
                        accept = true;
                        done = true;
                    }
                }
            }
        }
        if (side == CaptureSide::Pad && !done) {
            for (auto* padp : g.pads) {
                Arm* arm = nullptr;
                for (auto& a : arms)
                    if (a.pad == padp) arm = &a;
                if (!arm) {
                    arms.push_back({});
                    arm = &arms.back();
                    arm->pad = padp;
                }
                if (!arm->primed) {
                    arm->primed = true;
                    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++)
                        arm->button_was[b] = SDL_GetGamepadButton(padp, static_cast<SDL_GamepadButton>(b));
                    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && a < 8; a++) {
                        int v = SDL_GetGamepadAxis(padp, static_cast<SDL_GamepadAxis>(a));
                        if (v < 0) v = -v;
                        int center = (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) ? 4000 : 8000;
                        arm->axis_ready[a] = v < center;
                    }
                    continue;
                }
                for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++) {
                    bool down = SDL_GetGamepadButton(padp, static_cast<SDL_GamepadButton>(b));
                    if (down && !arm->button_was[b]) {
                        if (const char* tok = token_for_button(static_cast<SDL_GamepadButton>(b))) {
                            pad = tok;
                            accept = true;
                            done = true;
                        }
                    }
                    arm->button_was[b] = down;
                }
                if (done) break;
                for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && a < 8; a++) {
                    Sint16 v = SDL_GetGamepadAxis(padp, static_cast<SDL_GamepadAxis>(a));
                    int mag = v < 0 ? -v : v;
                    int center = (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) ? 4000 : 8000;
                    if (mag < center) arm->axis_ready[a] = true;
                    else if (arm->axis_ready[a] && mag > 20000) {
                        if (const char* tok = token_for_axis(a, v)) {
                            pad = tok;
                            accept = true;
                            done = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!done) SDL_Delay(10);
    }
    if (renderer) SDL_DestroyRenderer(renderer);
    if (sw) SDL_DestroyWindow(sw);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    g.in_capture = false;
    if (clear) {
        if (side == CaptureSide::Key) {
            g.rows[static_cast<size_t>(row)].key.clear();
            g.rows[static_cast<size_t>(row)].key2.clear();
        } else {
            g.rows[static_cast<size_t>(row)].pad.clear();
        }
        return true;
    }
    if (!accept) return false;
    if (side == CaptureSide::Key) {
        g.rows[static_cast<size_t>(row)].key = key;
        g.rows[static_cast<size_t>(row)].key2.clear();
    } else {
        g.rows[static_cast<size_t>(row)].pad = pad;
    }
    return true;
}

// Save sets.
bool ensure_stopped() {
    if (!game_running()) return true;
    alert(L"BotWRecomp.exe is running. Close the game before changing saves.");
    return false;
}

bool have_user() {
    if (!user_root().empty()) return true;
    alert(L"Set the user data folder first. Saves and controls.ini go there.");
    return false;
}

void cleanup_temp(const std::filesystem::path& tmp) {
    if (tmp.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

std::filesystem::path make_temp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"botw-import-%04u%02u%02u-%02u%02u%02u-%lu", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, GetCurrentProcessId());
    return std::filesystem::temp_directory_path() / name;
}

bool is_zip_path(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return false;
    auto ext = p.extension().wstring();
    for (wchar_t& c : ext)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return ext == L".zip";
}

void import_path(const std::wstring& picked) {
    if (picked.empty()) return;
    if (!have_user() || !ensure_stopped()) return;
    std::filesystem::path path(picked);
    std::filesystem::path tmp, root = path;
    std::string err;
    if (is_zip_path(path)) {
        tmp = make_temp();
        if (!unzip_to(path, tmp, err)) {
            alert(widen(err));
            cleanup_temp(tmp);
            return;
        }
        root = tmp;
    }
    saveimport::Located loc;
    if (!saveimport::locate(root, loc, err)) {
        alert(widen(err));
        cleanup_temp(tmp);
        return;
    }
    if (loc.dangerous || loc.caution || !loc.note.empty()) {
        std::wstring text;
        if (!loc.warnings.empty()) text += widen(loc.warnings);
        if (!loc.note.empty()) {
            if (!text.empty()) text += L"\n\n";
            text += widen(loc.note);
        }
        text += L"\n\nImport anyway? The current save is backed up first.";
        UINT def = loc.dangerous ? MB_DEFBUTTON2 : MB_DEFBUTTON1;
        if (MessageBoxW(alert_owner(), text.c_str(), kAppTitle, MB_YESNO | MB_ICONWARNING | def) != IDYES) {
            cleanup_temp(tmp);
            set_status(L"Import cancelled.");
            return;
        }
    }
    auto result = saveimport::import_account(user_root(), loc, narrow(picked));
    cleanup_temp(tmp);
    fill_saves();
    set_status(widen(result.message));
    alert(widen(result.message), result.ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
}

void on_migrate() {
    if (!have_user() || !ensure_stopped()) return;
    saveimport::Located loc;
    if (!saveimport::peek_decimal(user_root(), loc)) {
        alert(L"No save in save\\2147483649. That is the folder an older build used for account slot 1.");
        return;
    }
    std::wstring text =
        L"Copy save\\2147483649 to save\\80000001?\nThe old folder stays where it is.\nIf save\\80000001 already has files, it is backed up first.";
    if (!loc.warnings.empty()) text += L"\n\n" + widen(loc.warnings);
    UINT def = loc.dangerous ? MB_DEFBUTTON2 : MB_DEFBUTTON1;
    if (MessageBoxW(alert_owner(), text.c_str(), kAppTitle, MB_YESNO | MB_ICONWARNING | def) != IDYES) return;
    auto result = saveimport::migrate_decimal(user_root());
    fill_saves();
    note_legacy_save();
    set_status(widen(result.message));
    alert(widen(result.message), result.ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
}

void on_import() {
    std::wstring picked = pick_browse(alert_owner(), L"Select a save folder, a .zip, or option.sav / game_data.sav inside the save.", true);
    import_path(picked);
}

void on_switch() {
    int i = selected_set();
    if (i < 0) {
        alert(L"Select a save set first.");
        return;
    }
    if (!have_user() || !ensure_stopped()) return;
    std::wstring title = widen(g.sets[static_cast<size_t>(i)].title);
    std::wstring text = L"Switch to \"" + title + L"\"?\nThe current save is backed up first.";
    if (!g.sets[static_cast<size_t>(i)].warnings.empty()) text += L"\n\n" + widen(g.sets[static_cast<size_t>(i)].warnings);
    if (MessageBoxW(alert_owner(), text.c_str(), kAppTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    auto result = saveimport::switch_to(user_root(), g.sets[static_cast<size_t>(i)].dir);
    fill_saves();
    set_status(widen(result.message));
    alert(widen(result.message), result.ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
}

void on_delete() {
    int i = selected_set();
    if (i < 0) {
        alert(L"Select a save set first.");
        return;
    }
    if (!have_user() || !ensure_stopped()) return;
    std::wstring title = widen(g.sets[static_cast<size_t>(i)].title);
    if (MessageBoxW(alert_owner(), (L"Delete \"" + title + L"\"? This does not change the live save.").c_str(), kAppTitle,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    auto result = saveimport::delete_set(user_root(), g.sets[static_cast<size_t>(i)].dir);
    fill_saves();
    set_status(widen(result.message));
    if (!result.ok) alert(widen(result.message));
}

void on_open_saves() {
    auto root = user_root();
    if (root.empty()) {
        alert(L"Set the user data folder first.");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(root / "save" / "80000001", ec);
    std::filesystem::create_directories(root / "save" / "common", ec);
    ShellExecuteW(alert_owner(), L"open", (root / "save").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring tail_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return L"No runtime.log yet. Launch the game once.";
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    const std::streamoff cap = 256 * 1024;
    if (sz > cap) in.seekg(sz - cap);
    else in.seekg(0);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (sz > cap) {
        auto nl = data.find('\n');
        if (nl != std::string::npos) data.erase(0, nl + 1);
    }
    std::vector<size_t> lines;
    lines.push_back(0);
    for (size_t i = 0; i < data.size(); i++)
        if (data[i] == '\n') lines.push_back(i + 1);
    size_t begin = lines.size() > 250 ? lines[lines.size() - 250] : 0;
    std::string body = data.substr(begin), crlf;
    crlf.reserve(body.size() + body.size() / 32);
    for (size_t i = 0; i < body.size(); i++) {
        if (body[i] == '\n' && (i == 0 || body[i - 1] != '\r')) crlf.push_back('\r');
        crlf.push_back(body[i]);
    }
    return widen(crlf);
}

// Launch.
std::wstring join_path_env(const std::wstring& extra, const std::wstring& existing) {
    if (extra.empty()) return existing;
    if (existing.empty()) return extra;
    if (!extra.empty() && extra.back() == L';') return extra + existing;
    return extra + L";" + existing;
}

std::wstring msys_bin() {
    if (GetFileAttributesW(L"C:\\msys64\\ucrt64\\bin") != INVALID_FILE_ATTRIBUTES) return L"C:\\msys64\\ucrt64\\bin";
    return {};
}

std::wstring build_env(const std::filesystem::path& root, const std::filesystem::path& exe) {
    std::wstring extra;
    std::wstring msys = msys_bin();
    if (!msys.empty()) extra += msys + L";";
    extra += exe.parent_path().wstring() + L";";
    wchar_t* cur = GetEnvironmentStringsW();
    std::wstring out;
    bool saw_path = false, saw_user = false;
    if (cur) {
        for (wchar_t* p = cur; *p;) {
            std::wstring e = p;
            p += e.size() + 1;
            if (e.rfind(L"PATH=", 0) == 0 || e.rfind(L"Path=", 0) == 0) {
                e = L"PATH=" + join_path_env(extra, e.substr(5));
                saw_path = true;
            } else if (e.rfind(L"BOTW_USER_ROOT=", 0) == 0) {
                e = L"BOTW_USER_ROOT=" + root.wstring();
                saw_user = true;
            }
            out += e;
            out.push_back(L'\0');
        }
        FreeEnvironmentStringsW(cur);
    }
    if (!saw_path) {
        out += L"PATH=" + extra;
        out.push_back(L'\0');
    }
    if (!saw_user) {
        out += L"BOTW_USER_ROOT=" + root.wstring();
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

void update_play_button();

std::wstring nice_name() {
    std::wstring n = widen(g.info.long_name);
    if (n.empty()) {
        std::filesystem::path p(g.cfg.game);
        n = g.cfg.game.empty() ? L"No game folder" : p.filename().wstring();
    }
    return n;
}

std::wstring info_line() {
    const wchar_t* dot = L"  \u00B7  ";
    std::wstring s;
    if (g.info.has_title_version) s += L"Update " + widen(gameinfo::version_name(g.info.title_version));
    else s += L"Update unknown";
    std::string region = gameinfo::region_name(g.info.title_id);
    if (!region.empty()) s += dot + widen(region);
    s += dot;
    s += L"Vulkan";
    if (g.cfg.fullscreen) s += dot + std::wstring(L"Fullscreen");
    int on = mods::enabled_count(g.mods);
    if (g.cfg.no_mods) s += dot + std::wstring(L"Mods off");
    else if (on > 0) s += dot + std::to_wstring(on) + (on == 1 ? L" mod on" : L" mods on");
    return s;
}

// Controls rows and capture.
struct ControlsWindow;
ControlsWindow* g_controls_win = nullptr;
void fill_controls();

void load_controls() {
    auto root = user_root();
    std::vector<controlsfile::Binding> file_rows;
    std::string err;
    auto st = controlsfile::load(root / "controls.ini", file_rows, err);
    bool present = st == controlsfile::LoadStatus::Ok && !file_rows.empty();
    if (st == controlsfile::LoadStatus::Error) alert(L"Could not read controls.ini:\n" + widen(err));
    g.rows = controlsfile::editor_rows(present, file_rows);
    fill_controls();
}

bool save_controls() {
    auto root = user_root();
    if (root.empty()) {
        alert(L"Set the saves and logs folder in Settings before saving controls.");
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    std::string err;
    if (!controlsfile::save(root / "controls.ini", g.rows, err)) {
        alert(widen(err));
        return false;
    }
    return true;
}

void on_capture(HWND owner, int item, int sub) {
    if (item < 0 || item >= static_cast<int>(g.rows.size())) return;
    if (sub != 1 && sub != 2) return;
    if (!capture_binding(owner, item, sub == 1 ? CaptureSide::Key : CaptureSide::Pad)) return;
    if (!save_controls()) return;
    fill_controls();
    set_status(L"Saved controls.ini. Start the game again to use it.");
}

void on_reset() {
    if (ask(L"Put every input back to the built-in map? This rewrites controls.ini.", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    g.rows = controlsfile::defaults();
    if (!save_controls()) return;
    fill_controls();
    set_status(L"Controls reset to the built-in map and written to controls.ini. Start the game again to use them.");
}

void note_legacy_save() {
    if (saveimport::decimal_legacy_folder(user_root()))
        set_status(L"A save is in save\\2147483649 from an older build. Migrate old save copies it to save\\80000001 and leaves the old folder in place.",
                   Tone::Warn);
}

void open_folder(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    ShellExecuteW(alert_owner(), L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Settings.
struct SettingsWindow : Window {
    HWND game = nullptr, exe = nullptr, user = nullptr, seconds = nullptr, fullscreen = nullptr, gamepad_view = nullptr;
    HWND game_b = nullptr, exe_b = nullptr, user_b = nullptr, save_b = nullptr, cancel_b = nullptr;
    struct Label {
        RECT title, help;
        const wchar_t* t;
        const wchar_t* h;
    };
    Label labels[4]{};
    RECT seconds_note{};
    SettingsWindow() { bar = 64; }
    void create() override {
        game = ui::edit(hwnd, ID_GAME);
        game_b = ui::button(hwnd, ID_GAME_BROWSE, L"Browse", ui::Kind::Normal);
        exe = ui::edit(hwnd, ID_EXE);
        exe_b = ui::button(hwnd, ID_EXE_BROWSE, L"Browse", ui::Kind::Normal);
        user = ui::edit(hwnd, ID_USER);
        user_b = ui::button(hwnd, ID_USER_BROWSE, L"Browse", ui::Kind::Normal);
        fullscreen = ui::button(hwnd, ID_FULLSCREEN, L"Start in fullscreen", ui::Kind::Check);
        gamepad_view = ui::button(hwnd, ID_GAMEPAD_VIEW, L"Show GamePad screen beside TV (F2 / Guide)", ui::Kind::Check);
        seconds = ui::edit(hwnd, ID_SECONDS, ES_NUMBER);
        save_b = ui::button(hwnd, ID_SAVE_SETTINGS, L"Save", ui::Kind::Primary, true);
        cancel_b = ui::button(hwnd, ID_CANCEL_SETTINGS, L"Cancel", ui::Kind::Normal, true);
        set_text(game, g.cfg.game);
        set_text(exe, g.cfg.exe);
        set_text(user, g.cfg.user);
        set_text(seconds, g.cfg.seconds.empty() ? L"0" : g.cfg.seconds);
        g.cfg.gamepad_view = read_user_gamepad_view(user_root(), g.cfg.gamepad_view);
        ui::set_checked(fullscreen, g.cfg.fullscreen);
        ui::set_checked(gamepad_view, g.cfg.gamepad_view);
        labels[0] = {{}, {}, L"Game folder", L"The folder with code, content and meta from your v1.5.0 dump."};
        labels[1] = {{}, {}, L"Game program", L"BotWRecomp.exe. It is usually next to this launcher."};
        labels[2] = {{}, {}, L"Saves and logs", L"Saves, controls.ini, mods and runtime.log live here (BOTW_USER_ROOT)."};
        labels[3] = {{}, {}, L"Time limit", nullptr};
        SetFocus(game);
    }
    void layout(int w, int h) override {
        int m = S(24), y = S(24), field = S(32), gap = S(8);
        HWND edits[3] = {game, exe, user};
        HWND browse[3] = {game_b, exe_b, user_b};
        int lh = ui::line_height(ui::fonts().body_strong), sh = ui::line_height(ui::fonts().small);
        for (int i = 0; i < 3; i++) {
            labels[i].title = {m, y, w - m, y + lh};
            y += lh + S(2);
            labels[i].help = {m, y, w - m, y + sh};
            y += sh + gap;
            int bw = ui::button_width(browse[i]);
            ui::place_field(edits[i], {m, y, w - m - bw - gap, y + field});
            MoveWindow(browse[i], w - m - bw, y, bw, field, TRUE);
            y += field + S(24);
        }
        labels[3].title = {m, y, w - m, y + lh};
        y += lh + gap;
        ui::place_field(seconds, {m, y, m + S(88), y + field});
        seconds_note = {m + S(88) + gap + S(4), y, w - m, y + field};
        y += field + S(16);
        MoveWindow(fullscreen, m, y, ui::button_width(fullscreen), S(24), TRUE);
        y += S(24) + S(8);
        MoveWindow(gamepad_view, m, y, ui::button_width(gamepad_view), S(24), TRUE);
        int by = h - S(bar) + (S(bar) - field) / 2;
        place_row_right({cancel_b, save_b}, w - m, by, field, gap);
    }
    void paint(HDC dc, const RECT&) override {
        for (auto& l : labels) {
            ui::draw_text(dc, l.t, l.title, ui::fonts().body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            if (l.h) ui::draw_text(dc, l.h, l.help, ui::fonts().small, ui::pal.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        ui::draw_text(dc, L"seconds, then the game closes. 0 means no limit.", seconds_note, ui::fonts().small, ui::pal.muted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    void command(int id, int, HWND) override {
        switch (id) {
            case ID_GAME_BROWSE: {
                auto p = pick_browse(hwnd, L"Folder that contains code, content, and meta.", false);
                if (!p.empty()) set_text(game, p);
                break;
            }
            case ID_EXE_BROWSE: {
                auto p = pick_exe(hwnd, get_text(exe));
                if (!p.empty()) set_text(exe, p);
                break;
            }
            case ID_USER_BROWSE: {
                auto p = pick_browse(hwnd, L"User data folder (BOTW_USER_ROOT). Saves, controls.ini, and the log go here.", false);
                if (!p.empty()) set_text(user, p);
                break;
            }
            case ID_SAVE_SETTINGS: ok(); break;
            case ID_CANCEL_SETTINGS: cancel(); break;
            default: break;
        }
    }
    void ok() override {
        std::wstring ga = get_text(game), ex = get_text(exe), us = get_text(user), se = get_text(seconds);
        if (has_quote(ga) || has_quote(ex) || has_quote(us)) {
            alert(L"Paths cannot contain a double quote.");
            return;
        }
        if (us.empty()) us = appdata_root();
        if (se.empty()) se = L"0";
        bool user_changed = us != g.cfg.user;
        g.cfg.game = ga;
        g.cfg.exe = ex;
        g.cfg.user = us;
        g.cfg.seconds = se;
        g.cfg.fullscreen = ui::checked(fullscreen);
        g.cfg.gamepad_view = ui::checked(gamepad_view);
        write_user_gamepad_view(user_root(), g.cfg.gamepad_view);
        save_launcher_ini();
        if (user_changed) load_controls();
        refresh_game();
        if (user_changed) note_legacy_save();
        close();
    }
};

// Controls.
struct ControlsWindow : Window {
    HWND list = nullptr, set_key = nullptr, set_pad = nullptr, reset = nullptr, close_b = nullptr;
    RECT hint{};
    ControlsWindow() {
        bar = 64;
        min_w = 520;
        min_h = 360;
    }
    void create() override {
        g_controls_win = this;
        list = ui::list(hwnd, ID_CONTROLS, 0);
        const wchar_t* cols[] = {L"Input", L"Keyboard", L"Controller"};
        add_columns(list, cols, 3);
        set_key = ui::button(hwnd, ID_SET_KEY, L"Set key", ui::Kind::Normal, true);
        set_pad = ui::button(hwnd, ID_SET_PAD, L"Set controller button", ui::Kind::Normal, true);
        reset = ui::button(hwnd, ID_RESET, L"Reset to defaults", ui::Kind::Normal, true);
        close_b = ui::button(hwnd, ID_CLOSE, L"Close", ui::Kind::Normal, true);
        load_controls();
        SetFocus(list);
        if (!g.rows.empty()) ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    ~ControlsWindow() override { g_controls_win = nullptr; }
    void layout(int w, int h) override {
        int m = S(24), field = S(32), gap = S(8);
        int th = text_height(ui::fonts().body, hint_text(), w - 2 * m);
        hint = {m, S(20), w - m, S(20) + th};
        int top = hint.bottom + S(12), bottom = h - S(bar) - S(16);
        ui::place_framed(list, {m, top, w - m, std::max(top + S(40), bottom)});
        int lw = w - 2 * m - S(4) - GetSystemMetrics(SM_CXVSCROLL);
        ListView_SetColumnWidth(list, 0, lw * 40 / 100);
        ListView_SetColumnWidth(list, 1, lw * 30 / 100);
        ListView_SetColumnWidth(list, 2, lw - lw * 40 / 100 - lw * 30 / 100);
        int by = h - S(bar) + (S(bar) - field) / 2;
        int x = place_row({set_key, set_pad}, m, by, field, gap);
        MoveWindow(reset, x + S(8), by, ui::button_width(reset), field, TRUE);
        place_row_right({close_b}, w - m, by, field, gap);
    }
    static std::wstring hint_text() {
        return L"Click a Keyboard or Controller cell, then press the new key or button. Backspace clears it, Esc keeps it. Changes are saved to controls.ini right away.";
    }
    void paint(HDC dc, const RECT&) override { paint_lines(dc, hint_text(), hint, ui::fonts().body, ui::pal.muted); }
    int selected() const { return ListView_GetNextItem(list, -1, LVNI_SELECTED); }
    void command(int id, int, HWND) override {
        switch (id) {
            case ID_SET_KEY: on_capture(hwnd, selected(), 1); break;
            case ID_SET_PAD: on_capture(hwnd, selected(), 2); break;
            case ID_RESET: on_reset(); break;
            case ID_CLOSE: close(); break;
            default: break;
        }
    }
    LRESULT notify(NMHDR* hdr) override {
        if (hdr->hwndFrom == list && (hdr->code == NM_CLICK || hdr->code == NM_DBLCLK)) {
            auto* act = reinterpret_cast<NMITEMACTIVATE*>(hdr);
            LVHITTESTINFO ht{};
            ht.pt = act->ptAction;
            if (ListView_SubItemHitTest(list, &ht) >= 0) on_capture(hwnd, ht.iItem, ht.iSubItem);
        }
        return 0;
    }
    void ok() override { on_capture(hwnd, selected(), 1); }
};

void fill_controls() {
    if (!g_controls_win || !g_controls_win->list) return;
    HWND lv = g_controls_win->list;
    int keep = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < static_cast<int>(g.rows.size()); i++) {
        const auto& row = g.rows[static_cast<size_t>(i)];
        const auto* action = controlsfile::find_action(row.id);
        std::wstring label = widen(action ? action->label : row.id.c_str());
        std::wstring key = widen(controlsfile::keys_label(row));
        std::wstring pad = widen(controlsfile::pad_label(row.pad));
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = label.data();
        it.lParam = i;
        ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, i, 1, key.data());
        ListView_SetItemText(lv, i, 2, pad.data());
    }
    if (keep >= 0 && keep < static_cast<int>(g.rows.size())) {
        ListView_SetItemState(lv, keep, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(lv, keep, FALSE);
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
}

// Save sets.
struct SavesWindow : Window {
    HWND list = nullptr, import_b = nullptr, switch_b = nullptr, delete_b = nullptr, migrate_b = nullptr;
    HWND open_b = nullptr, refresh_b = nullptr, close_b = nullptr;
    RECT hint{};
    SavesWindow() {
        bar = 64;
        min_w = 680;
        min_h = 340;
    }
    void create() override {
        g.saves_win = this;
        list = ui::list(hwnd, ID_SAVES, 0);
        const wchar_t* cols[] = {L"Name", L"Kind", L"When", L"Notes"};
        add_columns(list, cols, 4);
        import_b = ui::button(hwnd, ID_IMPORT, L"Import save", ui::Kind::Primary, true);
        switch_b = ui::button(hwnd, ID_SWITCH, L"Switch to", ui::Kind::Normal, true);
        delete_b = ui::button(hwnd, ID_DELETE, L"Delete", ui::Kind::Normal, true);
        migrate_b = ui::button(hwnd, ID_MIGRATE, L"Migrate old save", ui::Kind::Normal, true);
        open_b = ui::button(hwnd, ID_OPEN, L"Open saves folder", ui::Kind::Flat, true);
        refresh_b = ui::button(hwnd, ID_REFRESH, L"Refresh", ui::Kind::Flat, true);
        close_b = ui::button(hwnd, ID_CLOSE, L"Close", ui::Kind::Normal, true);
        DragAcceptFiles(hwnd, TRUE);
        fill_saves();
        sync();
        SetFocus(list);
    }
    ~SavesWindow() override { g.saves_win = nullptr; }
    std::wstring hint_text() const {
        return L"The live save is in " + (user_root() / "save" / "80000001").wstring() +
               L". Import, Switch to and Migrate back it up first. Drop a save folder or .zip on this window to import it.";
    }
    void layout(int w, int h) override {
        int m = S(24), field = S(32), gap = S(8);
        int th = text_height(ui::fonts().body, hint_text(), w - 2 * m);
        hint = {m, S(20), w - m, S(20) + th};
        int top = hint.bottom + S(12), bottom = h - S(bar) - S(16);
        ui::place_framed(list, {m, top, w - m, std::max(top + S(40), bottom)});
        int lw = w - 2 * m - S(4) - GetSystemMetrics(SM_CXVSCROLL);
        ListView_SetColumnWidth(list, 0, lw * 34 / 100);
        ListView_SetColumnWidth(list, 1, lw * 14 / 100);
        ListView_SetColumnWidth(list, 2, lw * 22 / 100);
        ListView_SetColumnWidth(list, 3, lw - lw * 34 / 100 - lw * 14 / 100 - lw * 22 / 100);
        int by = h - S(bar) + (S(bar) - field) / 2;
        place_row({import_b, switch_b, delete_b, migrate_b}, m, by, field, gap);
        place_row_right({close_b, refresh_b, open_b}, w - m, by, field, S(4));
    }
    void paint(HDC dc, const RECT&) override { paint_lines(dc, hint_text(), hint, ui::fonts().body, ui::pal.muted); }
    void sync() {
        BOOL on = game_running() ? FALSE : TRUE;
        for (HWND b : {import_b, switch_b, delete_b, migrate_b}) EnableWindow(b, on);
    }
    void command(int id, int, HWND) override {
        switch (id) {
            case ID_IMPORT: on_import(); break;
            case ID_SWITCH: on_switch(); break;
            case ID_DELETE: on_delete(); break;
            case ID_MIGRATE: on_migrate(); break;
            case ID_OPEN: on_open_saves(); break;
            case ID_REFRESH:
                fill_saves();
                note_legacy_save();
                break;
            case ID_CLOSE: close(); break;
            default: break;
        }
    }
    LRESULT notify(NMHDR* hdr) override {
        if (hdr->hwndFrom == list && hdr->code == NM_DBLCLK && !game_running()) on_switch();
        return 0;
    }
    void dropped(const std::wstring& p) override { import_path(p); }
};

void fill_saves() {
    g.sets = saveimport::list_sets(user_root());
    if (!g.saves_win || !g.saves_win->list) return;
    HWND lv = g.saves_win->list;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < static_cast<int>(g.sets.size()); i++) {
        const auto& s = g.sets[static_cast<size_t>(i)];
        std::wstring title = widen(s.title);
        std::wstring kind = widen(s.kind);
        std::wstring when = widen(s.created);
        std::string note = s.warnings.empty() ? std::to_string(s.slots) + " slots" : s.warnings;
        std::wstring notes = widen(note);
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = title.data();
        it.lParam = i;
        ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, i, 1, kind.data());
        ListView_SetItemText(lv, i, 2, when.data());
        ListView_SetItemText(lv, i, 3, notes.data());
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
}

int selected_set() {
    if (!g.saves_win || !g.saves_win->list) return -1;
    int i = ListView_GetNextItem(g.saves_win->list, -1, LVNI_SELECTED);
    if (i < 0 || i >= static_cast<int>(g.sets.size())) return -1;
    return i;
}

// Mods.
struct ModsWindow : Window {
    HWND list = nullptr, up_b = nullptr, down_b = nullptr, folder_b = nullptr, refresh_b = nullptr, no_mods = nullptr, close_b = nullptr;
    RECT hint{};
    bool filling = false;
    ModsWindow() {
        bar = 64;
        min_w = 620;
        min_h = 360;
    }
    void create() override {
        list = ui::list(hwnd, ID_MODS, LVS_EX_CHECKBOXES);
        const wchar_t* cols[] = {L"Mod", L"Version", L"Description"};
        add_columns(list, cols, 3);
        up_b = ui::button(hwnd, ID_MOD_UP, L"Move up", ui::Kind::Normal, true);
        down_b = ui::button(hwnd, ID_MOD_DOWN, L"Move down", ui::Kind::Normal, true);
        folder_b = ui::button(hwnd, ID_MODS_FOLDER, L"Open mods folder", ui::Kind::Flat, true);
        refresh_b = ui::button(hwnd, ID_MODS_REFRESH, L"Refresh", ui::Kind::Flat, true);
        close_b = ui::button(hwnd, ID_CLOSE, L"Close", ui::Kind::Normal, true);
        no_mods = ui::button(hwnd, ID_NO_MODS, L"Launch without mods", ui::Kind::Check);
        ui::set_checked(no_mods, g.cfg.no_mods);
        g.mods = mods::load(user_root());
        fill(0);
        SetFocus(list);
    }
    std::wstring hint_text() const {
        std::wstring s = L"Each mod is a folder in " + mods::mods_dir(user_root()).wstring() +
                         L" with content or aoc inside. Checked mods load from top to bottom; when two change the same file, the lower one wins.";
        if (g.mods.list.empty()) s += L" No mods found yet.";
        return s;
    }
    void layout(int w, int h) override {
        int m = S(24), field = S(32), gap = S(8);
        int th = text_height(ui::fonts().body, hint_text(), w - 2 * m);
        hint = {m, S(20), w - m, S(20) + th};
        int top = hint.bottom + S(12), bottom = h - S(bar) - S(16) - S(24) - S(12);
        ui::place_framed(list, {m, top, w - m, std::max(top + S(40), bottom)});
        int lw = w - 2 * m - S(4) - GetSystemMetrics(SM_CXVSCROLL);
        ListView_SetColumnWidth(list, 0, lw * 34 / 100);
        ListView_SetColumnWidth(list, 1, lw * 12 / 100);
        ListView_SetColumnWidth(list, 2, lw - lw * 34 / 100 - lw * 12 / 100);
        MoveWindow(no_mods, m, std::max(top + S(40), bottom) + S(12), ui::button_width(no_mods), S(24), TRUE);
        int by = h - S(bar) + (S(bar) - field) / 2;
        place_row({up_b, down_b}, m, by, field, gap);
        place_row_right({close_b, refresh_b, folder_b}, w - m, by, field, S(4));
    }
    void paint(HDC dc, const RECT&) override { paint_lines(dc, hint_text(), hint, ui::fonts().body, ui::pal.muted); }
    void fill(int select) {
        filling = true;
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        for (int i = 0; i < static_cast<int>(g.mods.list.size()); i++) {
            const auto& md = g.mods.list[static_cast<size_t>(i)];
            std::wstring name = widen(md.name), ver = widen(md.version), desc = widen(md.description);
            if (!md.has_content && !md.has_aoc) desc = L"No content or aoc folder, so this mod changes nothing." + (desc.empty() ? L"" : L" " + desc);
            LVITEMW it{};
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = i;
            it.pszText = name.data();
            it.lParam = i;
            ListView_InsertItem(list, &it);
            ListView_SetItemText(list, i, 1, ver.data());
            ListView_SetItemText(list, i, 2, desc.data());
            ListView_SetCheckState(list, i, md.enabled ? TRUE : FALSE);
        }
        if (select >= 0 && select < static_cast<int>(g.mods.list.size())) {
            ListView_SetItemState(list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list, select, FALSE);
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        filling = false;
    }
    void save(bool order) {
        std::string err;
        if (!mods::save(user_root(), g.mods, order, err)) alert(L"Could not write mods.ini:\n" + widen(err));
        if (order) g.mods.has_order = true;
        update_play_button();
    }
    void move(int delta) {
        int i = ListView_GetNextItem(list, -1, LVNI_SELECTED);
        int j = i + delta;
        if (i < 0 || j < 0 || j >= static_cast<int>(g.mods.list.size())) return;
        std::swap(g.mods.list[static_cast<size_t>(i)], g.mods.list[static_cast<size_t>(j)]);
        save(true);
        fill(j);
    }
    void command(int id, int, HWND) override {
        switch (id) {
            case ID_MOD_UP: move(-1); break;
            case ID_MOD_DOWN: move(1); break;
            case ID_MODS_FOLDER: open_folder(mods::mods_dir(user_root())); break;
            case ID_MODS_REFRESH:
                g.mods = mods::load(user_root());
                fill(0);
                relayout();
                update_play_button();
                break;
            case ID_NO_MODS:
                g.cfg.no_mods = ui::checked(no_mods);
                save_launcher_ini();
                update_play_button();
                break;
            case ID_CLOSE: close(); break;
            default: break;
        }
    }
    LRESULT notify(NMHDR* hdr) override {
        if (hdr->hwndFrom != list || hdr->code != LVN_ITEMCHANGED || filling) return 0;
        auto* nm = reinterpret_cast<NMLISTVIEW*>(hdr);
        if (!(nm->uChanged & LVIF_STATE) || !((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK)) return 0;
        if (nm->iItem < 0 || nm->iItem >= static_cast<int>(g.mods.list.size())) return 0;
        bool on = ListView_GetCheckState(list, nm->iItem) != 0;
        auto& md = g.mods.list[static_cast<size_t>(nm->iItem)];
        if (md.enabled == on) return 0;
        md.enabled = on;
        save(false);
        return 0;
    }
};

// Log.
struct LogWindow : Window {
    HWND text = nullptr, file_b = nullptr, close_b = nullptr;
    LogWindow() {
        bar = 64;
        min_w = 520;
        min_h = 300;
    }
    void create() override {
        text = ui::edit(hwnd, ID_LOG_TEXT, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL, ui::FontRole::Mono);
        SetWindowLongW(text, GWL_STYLE, GetWindowLongW(text, GWL_STYLE) & ~ES_AUTOHSCROLL);
        set_text(text, tail_file(user_root() / "runtime.log"));
        file_b = ui::button(hwnd, ID_LOG_FILE, L"Open in editor", ui::Kind::Normal, true);
        close_b = ui::button(hwnd, ID_CLOSE, L"Close", ui::Kind::Normal, true);
        SetFocus(close_b);
    }
    void layout(int w, int h) override {
        int m = S(24), field = S(32);
        ui::place_framed(text, {m, S(24), w - m, std::max(S(64), h - S(bar) - S(16))});
        int by = h - S(bar) + (S(bar) - field) / 2;
        place_row({file_b}, m, by, field, S(8));
        place_row_right({close_b}, w - m, by, field, S(8));
        int n = GetWindowTextLengthW(text);
        SendMessageW(text, EM_SETSEL, n, n);
        SendMessageW(text, EM_SCROLLCARET, 0, 0);
    }
    void command(int id, int, HWND) override {
        if (id == ID_CLOSE) close();
        else if (id == ID_LOG_FILE) {
            auto p = user_root() / "runtime.log";
            std::error_code ec;
            if (!std::filesystem::is_regular_file(p, ec)) alert(L"No runtime.log yet. Launch the game once.");
            else ShellExecuteW(hwnd, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
};

void show_settings() {
    SettingsWindow w;
    run_dialog(&w, L"Settings", 600, 560, false);
}
void show_controls() {
    ControlsWindow w;
    run_dialog(&w, L"Controls", 640, 600, true);
}
void show_saves() {
    SavesWindow w;
    run_dialog(&w, L"Saves", 880, 480, true);
}
void show_mods() {
    ModsWindow w;
    run_dialog(&w, L"Mods", 720, 480, true);
    refresh_game();
}
void show_log() {
    LogWindow w;
    run_dialog(&w, L"runtime.log (last 250 lines)", 800, 540, true);
}

void on_play() {
    if (game_running()) {
        alert(L"BotWRecomp.exe is already running.");
        return;
    }
    if (g.cfg.game.empty()) {
        auto p = pick_browse(g.hwnd, L"Folder that contains code, content, and meta.", false);
        if (p.empty()) return;
        if (has_quote(p)) {
            alert(L"Paths cannot contain a double quote.");
            return;
        }
        g.cfg.game = p;
        save_launcher_ini();
        refresh_game();
        return;
    }
    std::wstring game = g.cfg.game, exe = g.cfg.exe, user = g.cfg.user;
    if (has_quote(game) || has_quote(exe) || has_quote(user)) {
        alert(L"Paths cannot contain a double quote.");
        return;
    }
    std::error_code ec;
    if (exe.empty() || !std::filesystem::is_regular_file(std::filesystem::path(exe), ec)) {
        alert(L"Set the path to BotWRecomp.exe in Settings.");
        return;
    }
    std::filesystem::path game_path(game);
    bool looks = std::filesystem::is_regular_file(game_path / "code" / "U-King.rpx", ec);
    if (!looks) {
        if (ask(L"That game folder does not contain code\\U-King.rpx.\nLaunch anyway?", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    if (user.empty()) {
        alert(L"Set the saves and logs folder in Settings. Saves and controls.ini go there.");
        return;
    }
    std::filesystem::create_directories(std::filesystem::path(user), ec);
    int seconds = g.cfg.seconds.empty() ? 0 : _wtoi(g.cfg.seconds.c_str());
    if (seconds < 0) seconds = 0;
    std::wstring cmd = L"\"" + exe + L"\" --game \"" + game + L"\" --renderer vulkan";
    if (seconds > 0) cmd += L" --seconds " + std::to_wstring(seconds);
    if (g.cfg.fullscreen) cmd += L" --fullscreen";
    if (g.cfg.gamepad_view) cmd += L" --gamepad-view";
    if (g.cfg.no_mods) cmd += L" --no-mods";
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');
    std::wstring env = build_env(std::filesystem::path(user), std::filesystem::path(exe));
    std::wstring cwd = std::filesystem::path(exe).parent_path().wstring();
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.data(), cwd.c_str(), &si, &pi);
    if (!ok) {
        DWORD e = GetLastError();
        wchar_t* msg = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                       reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
        std::wstring text = L"Could not start the game.";
        if (msg) {
            text += L"\n";
            text += msg;
            LocalFree(msg);
        }
        text += L"\nIf this was a missing DLL, start build\\BotWLauncher.cmd so C:\\msys64\\ucrt64\\bin is on PATH.";
        alert(text);
        set_status(L"Could not start the game.", Tone::Error);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    save_launcher_ini();
    set_status(L"Launched. Saves stay locked until BotWRecomp.exe closes.");
    g.was_running = true;
    if (g.saves_win) g.saves_win->sync();
    update_play_button();
}

// Main window.
constexpr int kMainW = 680, kMainH = 264;

struct MainWindow : Window {
    HWND play = nullptr, settings = nullptr, controls = nullptr, saves = nullptr, mods_b = nullptr, user_b = nullptr, log_b = nullptr;
    RECT icon_r{}, over_r{}, title_r{}, info_r{}, status_r{};
    int divider = 0;
    MainWindow() {
        is_main = true;
        bar = 36;
    }
    void create() override {
        play = ui::button(hwnd, ID_PLAY, L"Play", ui::Kind::Primary, false, true);
        settings = ui::button(hwnd, ID_SETTINGS, L"Settings", ui::Kind::Flat);
        controls = ui::button(hwnd, ID_CONTROLS_OPEN, L"Controls", ui::Kind::Flat);
        saves = ui::button(hwnd, ID_SAVES_OPEN, L"Saves", ui::Kind::Flat);
        mods_b = ui::button(hwnd, ID_MODS_OPEN, L"Mods", ui::Kind::Flat);
        user_b = ui::button(hwnd, ID_USER_OPEN, L"Open user folder", ui::Kind::Flat);
        log_b = ui::button(hwnd, ID_LOG_OPEN, L"Open log", ui::Kind::Flat);
        SetTimer(hwnd, 1, 400, nullptr);
        SetFocus(play);
    }
    void layout(int w, int h) override {
        int m = S(24), icon = S(128);
        icon_r = {m, m, m + icon, m + icon};
        int x = icon_r.right + S(24);
        int pw = std::max(S(152), ui::button_width(play)), ph = S(40);
        int mid = (icon_r.top + icon_r.bottom) / 2;
        MoveWindow(play, w - m - pw, mid - ph / 2 - S(12), pw, ph, TRUE);
        int oh = ui::line_height(ui::fonts().overline), ih = ui::line_height(ui::fonts().body);
        over_r = {x, m + S(4), w - m - pw - S(24), m + S(4) + oh};
        info_r = {x, icon_r.bottom - ih, w - m, icon_r.bottom};
        title_r = {x, over_r.bottom + S(4), w - m - pw - S(24), info_r.top - S(4)};
        divider = icon_r.bottom + S(24);
        int ty = divider + S(8), th = S(32);
        place_row({settings, controls, saves, mods_b}, m - S(12), ty, th, S(0));
        place_row_right({log_b, user_b}, w - m + S(12), ty, th, S(0));
        status_r = {m, h - S(bar), w - m, h};
    }
    void paint(HDC dc, const RECT& rc) override {
        const auto& f = ui::fonts();
        if (!g.info.icon.empty()) {
            ui::draw_bitmap(dc, icon_r, g.info.icon.bgra.data(), g.info.icon.width, g.info.icon.height);
        } else {
            ui::round_rect(dc, icon_r, S(8), ui::pal.panel, ui::pal.line);
            ui::draw_text(dc, L"No icon", icon_r, f.small, ui::pal.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        std::wstring over = L"WII U";
        if (!g.info.title_id.empty()) over += L"  \u00B7  " + widen(g.info.title_id);
        ui::draw_text(dc, over, over_r, f.overline, ui::pal.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        ui::draw_text(dc, nice_name(), title_r, f.title, ui::pal.text, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (!g.cfg.game.empty() && g.info.folder_exists)
            ui::draw_text(dc, info_line(), info_r, f.body, ui::pal.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        ui::hline(dc, 0, rc.right, divider, ui::pal.line);
        COLORREF dot = g.tone == Tone::Good ? ui::pal.accent : g.tone == Tone::Warn ? ui::pal.warn : g.tone == Tone::Error ? ui::pal.error : ui::pal.muted;
        int cy = (status_r.top + status_r.bottom) / 2;
        RECT d{status_r.left, cy - S(4), status_r.left + S(8), cy + S(4)};
        ui::round_rect(dc, d, S(8), dot, CLR_INVALID);
        RECT t = status_r;
        t.left += S(16);
        ui::draw_text(dc, g.status, t, f.small, ui::pal.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    void command(int id, int, HWND) override {
        switch (id) {
            case ID_PLAY: on_play(); break;
            case ID_SETTINGS: show_settings(); break;
            case ID_CONTROLS_OPEN: show_controls(); break;
            case ID_SAVES_OPEN: show_saves(); break;
            case ID_MODS_OPEN: show_mods(); break;
            case ID_USER_OPEN:
                if (!user_root().empty()) open_folder(user_root());
                break;
            case ID_LOG_OPEN: show_log(); break;
            default: break;
        }
    }
    void ok() override {
        if (IsWindowEnabled(play)) on_play();
    }
    void cancel() override {}
    void on_close() override {
        save_launcher_ini();
        DestroyWindow(hwnd);
    }
    void timer() override {
        pump_sdl();
        if (g.in_capture) return;
        bool run = game_running();
        if (g.saves_win) g.saves_win->sync();
        if (run != g.was_running) {
            g.was_running = run;
            if (run) set_status(L"BotWRecomp.exe is running. Save import, switch, delete, and migrate are blocked until it closes.");
            else set_status(L"Game closed. Saves can be changed again.");
            update_play_button();
        }
    }
};

MainWindow* g_main = nullptr;

void update_play_button() {
    if (!g_main || !g_main->play) return;
    bool run = game_running();
    const wchar_t* label = g.cfg.game.empty() ? L"Choose game folder" : run ? L"Running" : L"Play";
    if (get_text(g_main->play) != label) {
        set_text(g_main->play, label);
        g_main->relayout();
    }
    EnableWindow(g_main->play, run ? FALSE : TRUE);
    InvalidateRect(g_main->play, nullptr, FALSE);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void set_process_dpi_awareness() {
    HMODULE user = GetModuleHandleW(L"user32.dll");
    using CtxFn = BOOL(WINAPI*)(HANDLE);
    FARPROC raw = user ? GetProcAddress(user, "SetProcessDpiAwarenessContext") : nullptr;
    CtxFn fn = nullptr;
    memcpy(&fn, &raw, sizeof fn);
    if (fn && fn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return;
    SetProcessDPIAware();
}

int system_dpi() {
    HDC dc = GetDC(nullptr);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return d > 0 ? d : 96;
}

}  // namespace

int main(int, char**) {
    SDL_SetMainReady();
    set_process_dpi_awareness();
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        g.sdl = true;
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        if (ids) {
            for (int i = 0; i < n; i++) remember_pad(ids[i]);
            SDL_free(ids);
        }
    }
    g.inst = GetModuleHandleW(nullptr);
    load_launcher_ini();
    ui::startup();
    ui::set_theme(theme_pref());
    ui::set_dpi(system_dpi());
    ui::rebuild_fonts();
    register_classes();
    MainWindow main_win;
    g_main = &main_win;
    HWND hwnd = make_window(&main_win, kMainClass, kAppTitle, kMainW, kMainH, false, nullptr);
    if (!hwnd) return 1;
    g.hwnd = hwnd;
    int wd = ui::window_dpi(hwnd);
    if (wd != ui::dpi()) {
        apply_dpi_everywhere(wd);
        RECT r{0, 0, S(kMainW), S(kMainH)};
        AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_EXSTYLE)));
        SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    }
    ui::center_on(hwnd, nullptr);
    refresh_game();
    note_legacy_save();
    main_win.relayout();
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        ui::note_key(msg);
        if (IsWindow(hwnd) && IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_main = nullptr;
    for (auto* p : g.pads) SDL_CloseGamepad(p);
    g.pads.clear();
    if (g.sdl) SDL_Quit();
    ui::shutdown();
    OleUninitialize();
    return 0;
}

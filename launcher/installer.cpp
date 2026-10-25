// Standalone Windows Setup && Bootstrapper for BotW-Native (BotW-Native-Setup.exe).
// Installs or configures BotWLauncher.exe + project files, creates optional Desktop and
// Start Menu shortcuts, and launches the integrated Setup Assistant (--setup).
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "ui.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef BIF_NONEWFOLDERBUTTON
#define BIF_NONEWFOLDERBUTTON 0x00000200
#endif

using ui::S;

namespace {

const wchar_t* kTitle = L"BotW-Native Setup";
const wchar_t* kClass = L"BotWNativeInstaller";

enum {
    ID_DIR_EDIT = 200,
    ID_DIR_BROWSE,
    ID_CHK_DESKTOP,
    ID_CHK_STARTMENU,
    ID_CHK_LAUNCH,
    ID_INSTALL,
    ID_CANCEL,
};

struct InstallerState {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND dir_e = nullptr, dir_b = nullptr;
    HWND chk_desktop = nullptr, chk_start = nullptr, chk_launch = nullptr;
    HWND install_b = nullptr, cancel_b = nullptr;
    int progress = 0;
    std::wstring status = L"Ready to install BotW-Native Launcher & Setup Assistant.";
    bool installing = false;
    bool finished = false;
};

InstallerState g_ins;

std::wstring get_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return {};
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(wcslen(s.c_str()));
    return s;
}

std::filesystem::path exe_dir() {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    if (!n || n >= 32768) return {};
    return std::filesystem::path(buf).parent_path();
}

std::wstring default_install_dir() {
    auto cur = exe_dir();
    std::error_code ec;
    if (std::filesystem::is_regular_file(cur / "BotWLauncher.exe", ec) &
        std::filesystem::is_regular_file(cur / "CMakeLists.txt", ec)) {
        return cur.wstring();
    }
    wchar_t buf[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf) == S_OK) {
        return std::wstring(buf) + L"\\BotW-Native";
    }
    return L"C:\\BotW-Native";
}

std::wstring pick_folder(HWND owner, const wchar_t* title) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_NEWDIALOGSTYLE | BIF_EDITBOX | BIF_RETURNONLYFSDIRS;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return {};
    wchar_t path[32768];
    path[0] = 0;
    BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    return ok ? std::wstring(path) : std::wstring();
}

bool create_shortcut(const std::filesystem::path& lnk_path,
                     const std::filesystem::path& target_exe,
                     const std::filesystem::path& work_dir,
                     const wchar_t* desc) {
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&psl));
    if (FAILED(hr) || !psl) return false;
    psl->SetPath(target_exe.c_str());
    psl->SetWorkingDirectory(work_dir.c_str());
    psl->SetDescription(desc);

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
    bool ok = false;
    if (SUCCEEDED(hr) && ppf) {
        ok = SUCCEEDED(ppf->Save(lnk_path.c_str(), TRUE));
        ppf->Release();
    }
    psl->Release();
    return ok;
}

void layout_installer(int w, int h) {
    int m = S(24), field = S(32), gap = S(8);
    int y = S(104);
    int bw = ui::button_width(g_ins.dir_b);
    ui::place_field(g_ins.dir_e, {m, y, w - m - bw - gap, y + field});
    MoveWindow(g_ins.dir_b, w - m - bw, y, bw, field, TRUE);

    y += field + S(20);
    MoveWindow(g_ins.chk_desktop, m, y, ui::button_width(g_ins.chk_desktop), S(24), TRUE);
    y += S(28);
    MoveWindow(g_ins.chk_start, m, y, ui::button_width(g_ins.chk_start), S(24), TRUE);
    y += S(28);
    MoveWindow(g_ins.chk_launch, m, y, ui::button_width(g_ins.chk_launch), S(24), TRUE);

    int bar = S(64);
    int by = h - bar + (bar - field) / 2;
    int cw = ui::button_width(g_ins.cancel_b);
    int iw = ui::button_width(g_ins.install_b);
    MoveWindow(g_ins.cancel_b, m, by, cw, field, TRUE);
    MoveWindow(g_ins.install_b, w - m - iw, by, iw, field, TRUE);
}

void set_progress(int pct, const std::wstring& msg) {
    g_ins.progress = std::clamp(pct, 0, 100);
    g_ins.status = msg;
    InvalidateRect(g_ins.hwnd, nullptr, FALSE);
    UpdateWindow(g_ins.hwnd);
}

void run_install() {
    if (g_ins.installing) return;
    std::wstring dest_str = get_text(g_ins.dir_e);
    if (dest_str.empty()) return;

    g_ins.installing = true;
    EnableWindow(g_ins.install_b, FALSE);
    EnableWindow(g_ins.dir_b, FALSE);
    EnableWindow(g_ins.dir_e, FALSE);

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path src = exe_dir();
    fs::path dst(dest_str);

    set_progress(10, L"Creating installation directory...");
    fs::create_directories(dst, ec);

    // Copy BotWLauncher.exe, SDL3.dll, gamecontrollerdb.txt, and BotWRecomp.exe if present alongside installer
    std::vector<fs::path> files;
    for (const char* f : {"BotWLauncher.exe", "BotWLauncherClassic.exe", "BotWRecomp.exe", "gamecontrollerdb.txt"})
        files.push_back(src / f);
    // Every DLL shipped beside the installer (SDL3 and the compiler runtime the game needs).
    for (auto it = fs::directory_iterator(src, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        auto ext = it->path().extension().wstring();
        if (_wcsicmp(ext.c_str(), L".dll") == 0) files.push_back(it->path());
    }
    std::wstring failed;
    if (!fs::is_directory(dst, ec)) failed += L"  " + dst.wstring() + L" (could not create the folder)\n";
    int idx = 0;
    for (const fs::path& s : files) {
        idx++;
        fs::path d = dst / s.filename();
        if (!fs::is_regular_file(s, ec)) continue;
        set_progress(15 + idx * 60 / (int)files.size(), L"Installing " + s.filename().wstring() + L"...");
        if (fs::equivalent(s, d, ec)) continue;
        std::error_code cec;
        if (!fs::copy_file(s, d, fs::copy_options::overwrite_existing, cec) || cec)
            failed += L"  " + s.filename().wstring() + L"\n";
    }
    if (!failed.empty()) {
        set_progress(0, L"Installation failed.");
        MessageBoxW(g_ins.hwnd, (L"These files could not be copied:\n" + failed +
            L"\nChoose a folder you can write to (not under Program Files), and close the game if it is running.").c_str(),
            kTitle, MB_OK | MB_ICONERROR);
        g_ins.installing = false;
        EnableWindow(g_ins.install_b, TRUE);
        EnableWindow(g_ins.dir_b, TRUE);
        EnableWindow(g_ins.dir_e, TRUE);
        return;
    }

    // Copy tools/ and cafe/ if present alongside installer and not already in dst
    for (const char* dir_name : {"tools", "cafe"}) {
        fs::path sdir = src / dir_name;
        fs::path ddir = dst / dir_name;
        if (fs::is_directory(sdir, ec) && !fs::equivalent(sdir, ddir, ec)) {
            set_progress(80, L"Copying recompiler scripts (" + std::wstring(sdir.filename().wstring()) + L")...");
            fs::copy(sdir, ddir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        }
    }

    fs::path launcher_exe = dst / "BotWLauncher.exe";
    if (!fs::is_regular_file(launcher_exe, ec) && fs::is_regular_file(src / "BotWLauncher.exe", ec)) {
        launcher_exe = src / "BotWLauncher.exe";
    }

    if (ui::checked(g_ins.chk_desktop)) {
        set_progress(88, L"Creating Desktop shortcut...");
        wchar_t desk[MAX_PATH];
        if (SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, desk) == S_OK) {
            create_shortcut(fs::path(desk) / L"BotW Recomp Launcher.lnk", launcher_exe, launcher_exe.parent_path(),
                            L"Launch BotW Recomp Launcher & Setup Assistant");
        }
    }

    if (ui::checked(g_ins.chk_start)) {
        set_progress(94, L"Creating Start Menu shortcut...");
        wchar_t progs[MAX_PATH];
        if (SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, SHGFP_TYPE_CURRENT, progs) == S_OK) {
            create_shortcut(fs::path(progs) / L"BotW Recomp Launcher.lnk", launcher_exe, launcher_exe.parent_path(),
                            L"Launch BotW Recomp Launcher & Setup Assistant");
        }
    }

    set_progress(100, L"Installation complete! Ready to launch Setup Assistant.");
    g_ins.installing = false;
    g_ins.finished = true;
    SetWindowTextW(g_ins.install_b, L"Finish");
    EnableWindow(g_ins.install_b, TRUE);

    if (ui::checked(g_ins.chk_launch) && fs::is_regular_file(launcher_exe, ec)) {
        ShellExecuteW(g_ins.hwnd, L"open", launcher_exe.c_str(), L"--setup", launcher_exe.parent_path().c_str(), SW_SHOWNORMAL);
        DestroyWindow(g_ins.hwnd);
    }
}

void paint_installer(HDC dc, const RECT& rc) {
    const auto& f = ui::fonts();
    int w = rc.right, h = rc.bottom;
    int m = S(24), bar = S(64);

    // Header banner
    RECT hdr{0, 0, w, S(68)};
    ui::fill(dc, hdr, ui::pal.panel);
    ui::hline(dc, 0, w, S(68), ui::pal.line);
    ui::draw_text(dc, L"BotW-Native Setup & Recompiler Installer",
                  {m, S(12), w - m, S(38)}, f.big, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    ui::draw_text(dc, L"Installs the launcher, Wii U (U-King.rpx) \u2192 C++ recompiler assistant, and shortcuts.",
                  {m, S(38), w - m, S(60)}, f.small, ui::pal.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    // Bottom bar
    RECT bbar{0, h - bar, w, h};
    ui::fill(dc, bbar, ui::pal.panel);
    ui::hline(dc, 0, w, h - bar, ui::pal.line);

    ui::paint_frames(g_ins.hwnd, dc);

    ui::draw_text(dc, L"Installation Folder", {m, S(80), w - m, S(102)},
                  f.body_strong, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    // Progress card
    int py = h - bar - S(86);
    RECT pcard{m, py, w - m, h - bar - S(16)};
    ui::round_rect(dc, pcard, S(6), ui::pal.panel, ui::pal.line);

    ui::draw_text(dc, g_ins.status, {m + S(14), py + S(10), w - m - S(60), py + S(30)},
                  f.small, ui::pal.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    std::wstring pct_s = std::to_wstring(g_ins.progress) + L"%";
    ui::draw_text(dc, pct_s, {w - m - S(60), py + S(10), w - m - S(14), py + S(30)},
                  f.body_strong, ui::pal.accent, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    RECT track{m + S(14), py + S(36), w - m - S(14), py + S(54)};
    ui::round_rect(dc, track, S(4), ui::pal.field, ui::pal.field_line);
    if (g_ins.progress > 0) {
        int fw = std::max(S(8), static_cast<int>((track.right - track.left) * g_ins.progress / 100));
        RECT fill_r{track.left, track.top, std::min(track.right, track.left + fw), track.bottom};
        ui::round_rect(dc, fill_r, S(4), ui::pal.accent, CLR_INVALID);
    }
}

LRESULT CALLBACK installer_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_ins.hwnd = hwnd;
            ui::apply_frame(hwnd);
            g_ins.dir_e = ui::edit(hwnd, ID_DIR_EDIT);
            g_ins.dir_b = ui::button(hwnd, ID_DIR_BROWSE, L"Browse", ui::Kind::Normal);
            g_ins.chk_desktop = ui::button(hwnd, ID_CHK_DESKTOP, L"Create Desktop shortcut", ui::Kind::Check);
            g_ins.chk_start = ui::button(hwnd, ID_CHK_STARTMENU, L"Create Start Menu shortcut", ui::Kind::Check);
            g_ins.chk_launch = ui::button(hwnd, ID_CHK_LAUNCH, L"Launch BotW Recomp Setup Assistant when finished", ui::Kind::Check);
            g_ins.install_b = ui::button(hwnd, ID_INSTALL, L"Install & Launch Setup", ui::Kind::Primary, true);
            g_ins.cancel_b = ui::button(hwnd, ID_CANCEL, L"Cancel", ui::Kind::Normal, true);
            SetWindowTextW(g_ins.dir_e, default_install_dir().c_str());
            ui::set_checked(g_ins.chk_desktop, true);
            ui::set_checked(g_ins.chk_start, true);
            ui::set_checked(g_ins.chk_launch, true);
            ui::set_fonts(hwnd);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            layout_installer(LOWORD(lp), HIWORD(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT: {
            ui::PaintBuffer pb(hwnd);
            ui::fill(pb.dc, pb.rc, ui::pal.window);
            paint_installer(pb.dc, pb.rc);
            return 0;
        }
        case WM_DRAWITEM:
            if (ui::draw_item(reinterpret_cast<DRAWITEMSTRUCT*>(lp))) return TRUE;
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            return ui::ctl_color(msg, wp, lp);
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            if (ctl && code == BN_CLICKED && ui::is_check(ctl)) ui::set_checked(ctl, !ui::checked(ctl));
            if (id == ID_DIR_BROWSE) {
                auto p = pick_folder(hwnd, L"Choose installation folder for BotW-Native.");
                if (!p.empty()) SetWindowTextW(g_ins.dir_e, p.c_str());
            } else if (id == ID_INSTALL) {
                if (g_ins.finished) DestroyWindow(hwnd);
                else run_install();
            } else if (id == ID_CANCEL || id == IDCANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int main(int argc, char** argv) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    bool demo_progress = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i] ? argv[i] : "";
        if (a == "--demo-progress") demo_progress = true;
    }

    g_ins.inst = GetModuleHandleW(nullptr);
    ui::startup();
    ui::set_theme(ui::Theme::Auto);
    HDC dc = GetDC(nullptr);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    ui::set_dpi(dpi > 0 ? dpi : 96);
    ui::rebuild_fonts();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = installer_proc;
    wc.hInstance = g_ins.inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r{0, 0, S(600), S(410)};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    HWND hwnd = CreateWindowExW(0, kClass, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, g_ins.inst, nullptr);
    if (!hwnd) return 1;
    ui::center_on(hwnd, nullptr);
    RECT crc{};
    GetClientRect(hwnd, &crc);
    layout_installer(crc.right, crc.bottom);

    if (demo_progress) {
        g_ins.progress = 68;
        g_ins.status = L"Installing BotWLauncher.exe & Wii U -> C++ recompiler scripts...";
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        ui::note_key(msg);
        if (IsWindow(hwnd) && IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ui::shutdown();
    CoUninitialize();
    return 0;
}

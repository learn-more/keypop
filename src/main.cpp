// PROJECT:     keypop
// LICENSE:     MIT (https://spdx.org/licenses/MIT.html)
// PURPOSE:     Shows pressed keys and mouse buttons as toasts in the bottom right corner
// COPYRIGHT:   Copyright 2026 Mark Jansen <mark.jansen@reactos.org>

// Built without the CRT (see CMakeLists.txt), so: no static constructors, no printf family,
// no exceptions, no floating point.
// wsprintfW does no bounds checking; buffer sizes are picked to fit the worst case.
// Keep this file ASCII only, MSVC reads it in the system codepage.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <intrin.h>

// The compiler emits calls to these for struct init and copy.
extern "C"
{
#pragma function(memset, memcpy)
void* memset(void* d, int c, size_t n)
{
    __stosb((unsigned char*)d, (unsigned char)c, n);
    return d;
}

void* memcpy(void* d, const void* s, size_t n)
{
    __movsb((unsigned char*)d, (const unsigned char*)s, n);
    return d;
}
}

namespace
{

const DWORD kHoldMs = 1500;
const DWORD kFadeMs = 300;
const int kAlpha = 225;     // 0-255
const int kMaxToasts = 6;
const int kRollChars = 60;  // rolling/words: max chars in one toast
const COLORREF kBack = RGB(32, 32, 32);
const COLORREF kText = RGB(255, 255, 255);    // typed characters
const COLORREF kSpecial = RGB(255, 196, 64);  // named keys, combos, mouse
const int kCharWeight = FW_BOLD;
const int kSpecialWeight = FW_NORMAL;

// Wrap a special token inside toast text. Never drawn, see DrawRuns.
const wchar_t kOn = 1, kOff = 2;

enum { WM_KEY_EVT = WM_APP, WM_MOUSE_EVT, WM_TRAY };
enum { ID_STACKED = 1, ID_ROLLING, ID_WORDS, ID_PAUSE, ID_EXIT };  // mode items are ID_STACKED + mode
enum
{
    kStacked,  // one toast per key, repeats counted: "A x3"
    kRolling,  // one line: "A  A  Space  Ctrl+C"
    kWords,    // typed characters share a toast ("HELLO"), special keys get their own
};
enum { kCtrl = 1, kAlt = 2, kShift = 4, kWin = 8 };

struct Toast
{
    HWND hwnd;
    DWORD born;
    int count;           // repeats of `last`
    bool special;        // words mode: typed characters must not join this toast
    wchar_t prefix[96];  // older tokens, at most kRollChars + 2
    wchar_t last[64];    // newest token: mods (19) + key name (31) + markers (2)
    wchar_t text[200];   // prefix + last + repeat count
};

Toast g_toasts[kMaxToasts];  // oldest first
int g_n;
HINSTANCE g_inst;
HWND g_main;
HFONT g_font;      // special tokens
HFONT g_fontChar;  // typed characters
int g_dpi = 96;
int g_mode;
bool g_paused;
UINT g_taskbarCreated;
NOTIFYICONDATAW g_nid;
wchar_t g_ini[MAX_PATH];

int Px(int v)
{
    return MulDiv(v, g_dpi, 96);
}

void Remove(int i)
{
    DestroyWindow(g_toasts[i].hwnd);
    --g_n;
    for (; i < g_n; ++i) g_toasts[i] = g_toasts[i + 1];
}

// Returns the width in pixels. With draw=false it only measures.
// Changes the selected font, the caller restores it.
int DrawRuns(HDC dc, const wchar_t* s, int x, int y, bool draw)
{
    int x0 = x;
    bool special = false;
    while (*s)
    {
        if (*s == kOn || *s == kOff)
        {
            special = *s++ == kOn;
            continue;
        }
        int n = 0;
        while (s[n] && s[n] != kOn && s[n] != kOff) ++n;
        SIZE sz;
        SelectObject(dc, special ? g_font : g_fontChar);
        GetTextExtentPoint32W(dc, s, n, &sz);
        if (draw)
        {
            SetTextColor(dc, special ? kSpecial : kText);
            TextOutW(dc, x, y, s, n);
        }
        x += sz.cx;
        s += n;
    }
    return x - x0;
}

void Layout()
{
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, g_font);
    SIZE line;
    GetTextExtentPoint32W(dc, L"Ag", 2, &line);
    int y = wa.bottom - Px(12);
    for (int i = g_n - 1; i >= 0; --i)
    {
        Toast& t = g_toasts[i];
        // Padding must match the text offset in ToastProc.
        int w = DrawRuns(dc, t.text, 0, 0, false) + Px(28);
        int h = line.cy + Px(14);
        y -= h;
        SetWindowRgn(t.hwnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, Px(10), Px(10)), FALSE);
        SetWindowPos(t.hwnd, HWND_TOPMOST, wa.right - Px(12) - w, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(t.hwnd, nullptr, TRUE);
        y -= Px(6);
    }
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
}

// special: anything but a single typed character.
// wheel: counted in every mode, one scroll gesture is dozens of events.
void Push(const wchar_t* tok, bool special, bool wheel)
{
    Toast* t = g_n ? &g_toasts[g_n - 1] : nullptr;
    bool same = t && !lstrcmpW(t->last, tok);
    if (same && (wheel || g_mode == kStacked || (g_mode == kWords && special)))
    {
        ++t->count;
    }
    else if (t && g_mode == kWords && !special && !t->special && lstrlenW(t->text) < kRollChars)
    {
        lstrcpynW(t->prefix, t->text, ARRAYSIZE(t->prefix));
        lstrcpynW(t->last, tok, ARRAYSIZE(t->last));
        t->count = 1;
    }
    else if (t && g_mode == kRolling)
    {
        wchar_t buf[ARRAYSIZE(t->text) + 4];
        int len = wsprintfW(buf, L"%s  ", t->text);
        // Too long: drop whole tokens from the front. Cutting at the two-space separator
        // never splits a key name or a kOn/kOff pair.
        const wchar_t* p = buf;
        if (len > kRollChars)
        {
            int cut = len - kRollChars - 2;  // -2 so a separator ending right at the cut is found
            p = buf + (cut > 0 ? cut : 0);
            while (p[0] != L' ' || p[1] != L' ') ++p;  // terminates: buf ends with a separator
            p += 2;
        }
        lstrcpynW(t->prefix, p, ARRAYSIZE(t->prefix));
        lstrcpynW(t->last, tok, ARRAYSIZE(t->last));
        t->count = 1;
    }
    else
    {
        if (g_n == kMaxToasts) Remove(0);
        t = &g_toasts[g_n];
        ZeroMemory(t, sizeof(*t));
        t->hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            L"KeypopToast", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
        if (!t->hwnd) return;
        ++g_n;
        lstrcpynW(t->last, tok, ARRAYSIZE(t->last));
        t->count = 1;
        t->special = special;
    }
    if (t->count > 1)
        wsprintfW(t->text, L"%s%s x%d", t->prefix, t->last, t->count);
    else
        wsprintfW(t->text, L"%s%s", t->prefix, t->last);
    t->born = GetTickCount();
    SetLayeredWindowAttributes(t->hwnd, 0, kAlpha, LWA_ALPHA);
    Layout();
    SetTimer(g_main, 1, 30, nullptr);
}

void Tick()
{
    DWORD now = GetTickCount();
    bool removed = false;
    for (int i = g_n - 1; i >= 0; --i)
    {
        DWORD age = now - g_toasts[i].born;
        if (age >= kHoldMs + kFadeMs)
        {
            Remove(i);
            removed = true;
        }
        else if (age > kHoldMs)
        {
            BYTE alpha = (BYTE)(kAlpha * (kHoldMs + kFadeMs - age) / kFadeMs);
            SetLayeredWindowAttributes(g_toasts[i].hwnd, 0, alpha, LWA_ALPHA);
        }
    }
    if (removed) Layout();
    if (!g_n) KillTimer(g_main, 1);
}

void PushWithMods(int mods, const wchar_t* name, bool wheel)
{
    wchar_t tok[64];
    bool special = mods || name[1];
    wsprintfW(tok, special ? L"\x01%s%s%s%s%s\x02" : L"%s%s%s%s%s",
              mods & kCtrl ? L"Ctrl+" : L"", mods & kAlt ? L"Alt+" : L"",
              mods & kShift ? L"Shift+" : L"", mods & kWin ? L"Win+" : L"", name);
    Push(tok, special, wheel);
}

void KeyName(UINT vk, LPARAM lp, wchar_t* out, int cch)
{
    // Injected keys can arrive without a scancode.
    if (!(lp & 0xFF0000)) lp |= MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
    if (vk == VK_PAUSE)
        lstrcpynW(out, L"Pause", cch);
    else if (!GetKeyNameTextW((LONG)lp, out, cch))
        wsprintfW(out, L"VK 0x%02X", vk);
}

int Mods()
{
    int m = 0;
    if (GetAsyncKeyState(VK_CONTROL) < 0) m |= kCtrl;
    if (GetAsyncKeyState(VK_MENU) < 0) m |= kAlt;
    if (GetAsyncKeyState(VK_SHIFT) < 0) m |= kShift;
    if (GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0) m |= kWin;
    return m;
}

bool IsModifier(DWORD vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
           (vk >= VK_LSHIFT && vk <= VK_RMENU);
}

// Windows silently removes low-level hooks that respond slowly, so the hooks only post.
LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && !g_paused && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN))
    {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lp;
        if (!IsModifier(k->vkCode))
        {
            // lParam in the layout GetKeyNameTextW expects.
            LPARAM name = ((k->scanCode & 0xFF) << 16) | (k->flags & LLKHF_EXTENDED ? 1 << 24 : 0);
            PostMessageW(g_main, WM_KEY_EVT, MAKEWPARAM(k->vkCode, Mods()), name);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && !g_paused)
    {
        const MSLLHOOKSTRUCT* ms = (const MSLLHOOKSTRUCT*)lp;
        int btn = -1;  // index into names[] in MainProc
        switch (wp)
        {
            case WM_LBUTTONDOWN: btn = 0; break;
            case WM_RBUTTONDOWN: btn = 1; break;
            case WM_MBUTTONDOWN: btn = 2; break;
            case WM_XBUTTONDOWN: btn = 2 + HIWORD(ms->mouseData); break;
            case WM_MOUSEWHEEL: btn = (short)HIWORD(ms->mouseData) > 0 ? 5 : 6; break;
            case WM_MOUSEHWHEEL: btn = (short)HIWORD(ms->mouseData) > 0 ? 7 : 8; break;
        }
        if (btn >= 0) PostMessageW(g_main, WM_MOUSE_EVT, MAKEWPARAM(btn, Mods()), 0);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void TrayMenu(HWND h)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_STACKED, L"Stacked toasts");
    AppendMenuW(m, MF_STRING, ID_ROLLING, L"Rolling line");
    AppendMenuW(m, MF_STRING, ID_WORDS, L"Words");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), ID_PAUSE, L"Pause");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    CheckMenuRadioItem(m, ID_STACKED, ID_WORDS, ID_STACKED + g_mode, MF_BYCOMMAND);
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(h);  // without this the menu stays open when clicking elsewhere
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, 0, h, nullptr);
    DestroyMenu(m);
    switch (cmd)
    {
        case ID_STACKED:
        case ID_ROLLING:
        case ID_WORDS:
        {
            g_mode = cmd - ID_STACKED;
            wchar_t v[2] = {(wchar_t)(L'0' + g_mode), 0};
            WritePrivateProfileStringW(L"keypop", L"mode", v, g_ini);
            break;
        }
        case ID_PAUSE: g_paused = !g_paused; break;
        case ID_EXIT: DestroyWindow(h); break;
    }
}

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m)
    {
        case WM_KEY_EVT:
        {
            wchar_t name[32];
            KeyName(LOWORD(wp), lp, name, ARRAYSIZE(name));
            PushWithMods(HIWORD(wp), name, false);
            return 0;
        }
        case WM_MOUSE_EVT:
        {
            static const wchar_t* const names[] = {L"LMB", L"RMB", L"MMB", L"X1", L"X2",
                                                   L"Wheel Up", L"Wheel Down", L"Wheel Right", L"Wheel Left"};
            UINT btn = LOWORD(wp);
            if (btn < ARRAYSIZE(names)) PushWithMods(HIWORD(wp), names[btn], btn >= 5);
            return 0;
        }
        case WM_TIMER:
            Tick();
            return 0;
        case WM_TRAY:
            if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) TrayMenu(h);
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
    }
    if (m == g_taskbarCreated) Shell_NotifyIconW(NIM_ADD, &g_nid);  // explorer restarted
    return DefWindowProcW(h, m, wp, lp);
}

LRESULT CALLBACK ToastProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        for (int i = 0; i < g_n; ++i)
        {
            if (g_toasts[i].hwnd != h) continue;
            HGDIOBJ old = SelectObject(dc, g_font);
            SetBkMode(dc, TRANSPARENT);
            DrawRuns(dc, g_toasts[i].text, Px(14), Px(7), true);  // half the padding from Layout
            SelectObject(dc, old);
        }
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int Main()
{
    CreateMutexW(nullptr, TRUE, L"Keypop.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    HINSTANCE inst = g_inst = GetModuleHandleW(nullptr);
    // ReactOS doesn't export SetProcessDPIAware
    if (auto p = (BOOL(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware")) p();
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    g_font = CreateFontW(-Px(22), 0, 0, 0, kSpecialWeight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                         L"Segoe UI");
    g_fontChar = CreateFontW(-Px(22), 0, 0, 0, kCharWeight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                             L"Segoe UI");

    // keypop.ini next to the exe
    DWORD len = GetModuleFileNameW(nullptr, g_ini, MAX_PATH);
    if (len > 4 && len < MAX_PATH) lstrcpyW(g_ini + len - 4, L".ini");
    g_mode = GetPrivateProfileIntW(L"keypop", L"mode", kStacked, g_ini);
    if (g_mode < kStacked || g_mode > kWords) g_mode = kStacked;

    WNDCLASSW wc = {};
    wc.hInstance = inst;
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"KeypopMain";
    RegisterClassW(&wc);
    wc.lpfnWndProc = ToastProc;
    wc.lpszClassName = L"KeypopToast";
    wc.hbrBackground = CreateSolidBrush(kBack);
    RegisterClassW(&wc);

    // Hidden top-level window. Message-only windows never receive TaskbarCreated.
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_main = CreateWindowExW(0, L"KeypopMain", L"Keypop", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_main) return 1;

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), 0);
    lstrcpyW(g_nid.szTip, L"Keypop");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    HHOOK kh = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, inst, 0);
    HHOOK mh = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, inst, 0);
    if (!kh || !mh)
    {
        MessageBoxW(nullptr, L"Could not install input hooks.", L"Keypop", MB_ICONERROR);
        DestroyWindow(g_main);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);

    if (kh) UnhookWindowsHookEx(kh);
    if (mh) UnhookWindowsHookEx(mh);
    return 0;
}

}  // namespace

// Named in CMakeLists.txt via /ENTRY.
extern "C" void __cdecl Entry()
{
    ExitProcess((UINT)Main());
}

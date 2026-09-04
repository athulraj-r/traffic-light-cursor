/*
 * Traffic Light Cursor — C++ Win32 Rewrite
 *
 * Uses WH_MOUSE_LL (low-level mouse hook) which fires INSIDE the OS input
 * pipeline — before the cursor actually moves — enabling true freeze (RED)
 * and true fractional slowdown (YELLOW) with zero reaction delay.
 *
 * Build: MinGW/g++  (see CMakeLists.txt / build.bat)
 * OS:    Windows only
 */

#ifndef WINVER
#  define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#define OEMRESOURCE   // for OCR_NORMAL
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mfplay.h>
#include <timeapi.h>
#include "resource.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <random>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
enum class LightState { RED, YELLOW, GREEN };

static std::atomic<LightState> g_state{LightState::RED};
static std::atomic<bool>       g_enabled{true};
static std::atomic<bool>       g_chaos{false};
static std::atomic<bool>       g_running{true};
static std::atomic<bool>       g_reverse_controls{false};
static std::atomic<bool>       g_speeding_penalized{false};
static std::atomic<bool>       g_video_playing{false};

// Fractional accumulator for YELLOW sub-pixel movement
static double g_accum_x = 0.0;
static double g_accum_y = 0.0;

// Suppress flag: set when WE call SetCursorPos so the hook ignores that event.
static std::atomic<bool> g_suppress_next{false};

// Last cursor position tracked entirely from ms->pt (physical hook coordinates).
// Using ms->pt avoids the DPI mismatch of GetCursorPos() on DPI-unaware processes.
static LONG g_our_x = -1, g_our_y = -1;

// Speed tracking variables: accumulate over 150ms window
static LONG g_last_raw_x = -1, g_last_raw_y = -1;
static double g_speed_accum_dist = 0.0;
static std::chrono::steady_clock::time_point g_speed_window_start = std::chrono::steady_clock::now();

// ---------------------------------------------------------------------------
// Window handles / IDs
// ---------------------------------------------------------------------------
static HWND  g_hwnd        = nullptr;
static HWND  g_vid_hwnd    = nullptr;
static HHOOK g_mouse_hook  = nullptr;
static HHOOK g_kbd_hook    = nullptr;

// Custom message to signal state change from cycle thread → main thread
#define WM_TRAFFICSTATE (WM_APP + 1)
#define WM_CHAOSBTN     (WM_APP + 2)

// Control IDs
#define ID_BTN_TOGGLE 101
#define ID_BTN_CHAOS  102

static HWND g_btn_toggle = nullptr;
static HWND g_btn_chaos  = nullptr;

// ---------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------
struct RGB3 { BYTE r, g, b; };
static const RGB3 COL_RED     = {235,  87,  87};
static const RGB3 COL_YELLOW  = {242, 201,  76};
static const RGB3 COL_GREEN   = {39,  174,  96};
static const RGB3 COL_MAGENTA = {255,   0, 128};
static const RGB3 COL_DIM_R   = {51,    0,   0};
static const RGB3 COL_DIM_Y   = {51,   51,   0};
static const RGB3 COL_DIM_G   = {0,    51,   0};

// Current lamp colours (set from main thread only)
static RGB3 g_lamp_r = COL_DIM_R;
static RGB3 g_lamp_y = COL_DIM_Y;
static RGB3 g_lamp_g = COL_DIM_G;

static std::wstring g_status_line1 = L"INIT...";
static std::wstring g_status_line2 = L"";
static COLORREF     g_status_color = RGB(255,255,255);

// ---------------------------------------------------------------------------
// Cursor helpers
// ---------------------------------------------------------------------------
static HCURSOR g_cursor_red    = nullptr;
static HCURSOR g_cursor_yellow = nullptr;
static HCURSOR g_cursor_green  = nullptr;

/*
 * Build a 32×32 color cursor using CreateBitmap + CreateIconIndirect.
 * The arrow shape is identical to the Python version's polygon points.
 * We draw into a 32×32 DIB section then create a monochrome AND mask.
 */
static HCURSOR MakeColorCursor(BYTE r, BYTE g, BYTE b)
{
    const int W = 32, H = 32;

    // ---- color bitmap (XOR plane) ----
    BITMAPV4HEADER bmi = {};
    bmi.bV4Size        = sizeof(bmi);
    bmi.bV4Width       = W;
    bmi.bV4Height      = -H; // top-down
    bmi.bV4Planes      = 1;
    bmi.bV4BitCount    = 32;
    bmi.bV4V4Compression = BI_BITFIELDS;
    bmi.bV4RedMask     = 0x00FF0000;
    bmi.bV4GreenMask   = 0x0000FF00;
    bmi.bV4BlueMask    = 0x000000FF;
    bmi.bV4AlphaMask   = 0xFF000000;

    VOID* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hbmColor = CreateDIBSection(hdc, (BITMAPINFO*)&bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);

    if (!hbmColor) return nullptr;

    // Start fully transparent
    memset(bits, 0, W * H * 4);
    DWORD* px = (DWORD*)bits;

    // Arrow polygon (same shape as Python version):
    // (0,0),(0,24),(6,18),(12,30),(16,28),(10,16),(18,16)
    // We rasterize via GDI into a memory DC
    HDC memDC  = CreateCompatibleDC(nullptr);
    HBITMAP old = (HBITMAP)SelectObject(memDC, hbmColor);

    // Fill background transparent (already done via memset)
    // Draw polygon filled
    POINT pts[] = {{0,0},{0,24},{6,18},{12,30},{16,28},{10,16},{18,16}};
    HBRUSH hBrush = CreateSolidBrush(RGB(r,g,b));
    HPEN   hPen   = CreatePen(PS_SOLID, 1, RGB(20,20,20));
    HBRUSH oldBr  = (HBRUSH)SelectObject(memDC, hBrush);
    HPEN   oldPen = (HPEN)SelectObject(memDC, hPen);

    // Make background transparent first (SRCCOPY will stomp it — use
    // a GDI polygon then patch the alpha channel manually below)
    HBRUSH bgBr = CreateSolidBrush(RGB(0,0,0));
    RECT rc = {0,0,W,H};
    FillRect(memDC, &rc, bgBr);
    DeleteObject(bgBr);

    Polygon(memDC, pts, 7);

    SelectObject(memDC, oldBr);
    SelectObject(memDC, oldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);
    SelectObject(memDC, old);
    DeleteDC(memDC);

    // Patch alpha: any pixel that is NOT pure black → opaque; black → transparent
    for (int i = 0; i < W*H; i++) {
        DWORD p = px[i];
        BYTE pr = (p >> 16) & 0xFF;
        BYTE pg = (p >>  8) & 0xFF;
        BYTE pb = (p      ) & 0xFF;
        if (pr == 0 && pg == 0 && pb == 0)
            px[i] = 0x00000000; // transparent
        else
            px[i] = 0xFF000000 | (pr << 16) | (pg << 8) | pb; // opaque
    }

    // ---- AND mask bitmap (all zeros = opaque everywhere cursor drawn) ----
    HBITMAP hbmMask = CreateBitmap(W, H, 1, 1, nullptr);
    // Fill mask with 0 (opaque)
    HDC mdc2 = CreateCompatibleDC(nullptr);
    HBITMAP oldm = (HBITMAP)SelectObject(mdc2, hbmMask);
    HBRUSH wBr = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(mdc2, &rc, wBr);
    SelectObject(mdc2, oldm);
    DeleteDC(mdc2);

    ICONINFO ii = {};
    ii.fIcon    = FALSE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask  = hbmMask;
    ii.hbmColor = hbmColor;
    HCURSOR hCursor = (HCURSOR)CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    return hCursor;
}

static void ApplyOsCursor(LightState state)
{
    if (!g_enabled.load()) return;
    HCURSOR hc = nullptr;
    switch (state) {
        case LightState::RED:    hc = g_cursor_red;    break;
        case LightState::YELLOW: hc = g_cursor_yellow; break;
        case LightState::GREEN:  hc = g_cursor_green;  break;
    }
    if (hc) SetSystemCursor(CopyCursor(hc), OCR_NORMAL);
}

static void RestoreOsCursor()
{
    SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
}

// ---------------------------------------------------------------------------
// Troll Popups & Chaos Messages
// ---------------------------------------------------------------------------
static const wchar_t* TROLL_POPUPS[] = {
    L"Error 418: I am a coconut.",
    L"Please wait while we do nothing...",
    L"Task failed successfully.",
    L"Your disable request has been submitted to the Department of Motor Vehicles.\nEstimated wait time: 4 to 6 weeks.",
    L"Access Denied: Traffic rules are eternal.",
    L"Are you sure you want to not close this application?",
    L"Section 4(a) Violation: Attempting to bypass a traffic signal is a federal misdemeanor.",
    L"Mouse cursor is currently in transit. Please pull over first.",
    L"Nice try. The intersection remains under active surveillance.",
    L"Error 404: Exit button not found.",
    L"Movement permit denied by Regional Traffic Authority.",
    L"Road work ahead. Yeah, I sure hope it does."
};
static const int TROLL_POPUP_COUNT = 12;

static const wchar_t* CHAOS_MSGS[] = {
    L"Traffic authority lost control.",
    L"All lanes open in all directions.",
    L"Your cursor has been detained.",
    L"Movement permit revoked.",
    L"Gravity compromised.",
    L"Speed limit: -45 px/sec.",
    L"Opposite Day on Interstate 95.",
    L"Roundabout detected: spin indefinitely.",
    L"Pothole density critical.",
    L"Nobody asked for this."
};
static const int CHAOS_MSG_COUNT = 10;

// Forward declaration
static void SetState(LightState st, const wchar_t* line1, const wchar_t* line2, COLORREF col);

// ---------------------------------------------------------------------------
// Speeding Penalty: Unclosable Media Foundation Video Player
// ---------------------------------------------------------------------------
static IMFPMediaPlayer* g_pPlayer = nullptr;

static LRESULT CALLBACK VideoWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        (void)hdc;
        if (g_pPlayer) {
            g_pPlayer->UpdateVideo();
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        // Unclosable until video finishes!
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE || (wp & 0xFFF0) == SC_MINIMIZE)
            return 0;
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            g_running.store(false);
            if (g_hwnd && IsWindow(g_hwnd)) {
                PostMessageW(g_hwnd, WM_CLOSE, 0, 0xDEAD);
            }
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

class MediaPlayerCallback : public IMFPMediaPlayerCallback
{
    long m_cRef;
public:
    std::atomic<bool> m_ended{false};
    IMFPMediaPlayer* m_player = nullptr;

    MediaPlayerCallback() : m_cRef(1) {}
    virtual ~MediaPlayerCallback() = default;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&m_cRef);
        if (count == 0) delete this;
        return count;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* pEventHeader) override {
        if (pEventHeader->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET) {
            if (m_player) {
                m_player->Play();
            }
        }
        if (pEventHeader->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED ||
            pEventHeader->eEventType == MFP_EVENT_TYPE_ERROR) {
            m_ended.store(true);
        }
    }
};

static bool ExtractEmbeddedVideo(wchar_t* outPath, size_t maxLen)
{
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PENALTY_VIDEO), RT_RCDATA);
    if (!hRes) return false;

    DWORD resSize = SizeofResource(nullptr, hRes);
    if (resSize == 0) return false;

    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) return false;

    const void* pData = LockResource(hData);
    if (!pData) return false;

    wchar_t tempPath[MAX_PATH] = {0};
    DWORD tpLen = GetTempPathW(MAX_PATH, tempPath);
    if (tpLen == 0 || tpLen >= MAX_PATH) return false;

    wchar_t targetPath[MAX_PATH] = {0};
    swprintf_s(targetPath, MAX_PATH, L"%straffic_light_penalty.mp4", tempPath);

    // If file already exists and size matches, reuse it
    HANDLE hExisting = CreateFileW(targetPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hExisting != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER fSize;
        if (GetFileSizeEx(hExisting, &fSize) && (DWORD)fSize.QuadPart == resSize) {
            CloseHandle(hExisting);
            wcscpy_s(outPath, maxLen, targetPath);
            return true;
        }
        CloseHandle(hExisting);
    }

    HANDLE hFile = CreateFileW(targetPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(hFile, pData, resSize, &bytesWritten, nullptr);
    CloseHandle(hFile);

    if (ok && bytesWritten == resSize) {
        wcscpy_s(outPath, maxLen, targetPath);
        return true;
    }
    return false;
}

static void PlayPenaltyVideoThread()
{
    if (g_video_playing.exchange(true)) return;
    g_speeding_penalized.store(true);

    LightState prevState = g_state.load();
    SetState(LightState::RED, L"SPEEDING VIOLATION!", L"MANDATORY TRAFFIC SCHOOL", RGB(255, 59, 48));

    wchar_t videoPath[MAX_PATH] = {0};
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        *lastSlash = L'\0';
        swprintf_s(videoPath, MAX_PATH, L"%s\\video.mp4", exePath);
    } else {
        wcscpy_s(videoPath, MAX_PATH, L"video.mp4");
    }

    DWORD attr = GetFileAttributesW(videoPath);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (GetFileAttributesW(L"video.mp4") != INVALID_FILE_ATTRIBUTES) {
            wcscpy_s(videoPath, MAX_PATH, L"video.mp4");
            attr = 0;
        } else if (GetFileAttributesW(L"windows\\video.mp4") != INVALID_FILE_ATTRIBUTES) {
            wcscpy_s(videoPath, MAX_PATH, L"windows\\video.mp4");
            attr = 0;
        }
    }

    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (ExtractEmbeddedVideo(videoPath, MAX_PATH)) {
            attr = 0;
        }
    }

    if (attr == INVALID_FILE_ATTRIBUTES) {
        // Fallback if video.mp4 is missing
        MessageBoxW(g_hwnd,
            L"SPEEDING VIOLATION DETECTED!\n\nvideo.mp4 was not found in directory.\nSentence commuted to 5 seconds detention.",
            L"Traffic Enforcement Citation",
            MB_ICONWARNING | MB_SYSTEMMODAL);
        for (int i = 0; i < 50 && g_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        g_speed_accum_dist = 0.0;
        g_speed_window_start = std::chrono::steady_clock::now();
        g_speeding_penalized.store(false);
        g_video_playing.store(false);
        SetState(prevState, L"PROBATION", L"OBEY SPEED LIMIT", RGB(52, 199, 89));
        return;
    }

    CoInitialize(nullptr);
    MFStartup(MF_VERSION);

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW vwc = {};
        vwc.cbSize        = sizeof(vwc);
        vwc.style         = CS_NOCLOSE | CS_HREDRAW | CS_VREDRAW;
        vwc.lpfnWndProc   = VideoWndProc;
        vwc.hInstance     = GetModuleHandle(nullptr);
        vwc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        vwc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        vwc.lpszClassName = L"TrafficPenaltyVideo";
        RegisterClassExW(&vwc);
        classRegistered = true;
    }

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int vw = 854, vh = 480;
    int vx = (sw - vw) / 2;
    int vy = (sh - vh) / 2;

    HWND hVidWnd = CreateWindowExW(
        WS_EX_TOPMOST,
        L"TrafficPenaltyVideo",
        L"MANDATORY TRAFFIC SAFETY SCHOOL — SPEEDING PENALTY",
        WS_POPUP | WS_CAPTION | WS_VISIBLE,
        vx, vy, vw, vh,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr
    );

    g_vid_hwnd = hVidWnd;
    ShowWindow(hVidWnd, SW_SHOW);
    UpdateWindow(hVidWnd);
    SetForegroundWindow(hVidWnd);

    wchar_t fullVideoPath[MAX_PATH] = {0};
    GetFullPathNameW(videoPath, MAX_PATH, fullVideoPath, nullptr);

    MediaPlayerCallback* cb = new MediaPlayerCallback();
    IMFPMediaPlayer* pPlayer = nullptr;

    HRESULT hr = MFPCreateMediaPlayer(fullVideoPath, FALSE, 0, cb, hVidWnd, &pPlayer);
    g_pPlayer = pPlayer;
    cb->m_player = pPlayer;

    if (SUCCEEDED(hr)) {
        pPlayer->CreateMediaItemFromURL(fullVideoPath, FALSE, 0, nullptr);

        // Modal wait loop until video completes or ESC terminates
        while (g_running.load() && !cb->m_ended.load()) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        pPlayer->Stop();
        pPlayer->Release();
    } else {
        MessageBoxW(g_hwnd,
            L"SPEEDING VIOLATION DETECTED!\n\nUnable to play video.mp4.\nSentence commuted to 5 seconds detention.",
            L"Traffic Enforcement Citation",
            MB_ICONWARNING | MB_SYSTEMMODAL);
        for (int i = 0; i < 50 && g_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    g_pPlayer = nullptr;
    cb->Release();

    if (IsWindow(hVidWnd)) {
        DestroyWindow(hVidWnd);
    }
    g_vid_hwnd = nullptr;

    MFShutdown();
    CoUninitialize();

    g_speed_accum_dist = 0.0;
    g_speed_window_start = std::chrono::steady_clock::now();
    g_speeding_penalized.store(false);
    g_video_playing.store(false);
    SetState(prevState, L"PROBATION", L"OBEY SPEED LIMIT", RGB(52, 199, 89));
}

static void TriggerSpeedingPenalty()
{
    if (g_video_playing.load() || g_speeding_penalized.load()) return;
    std::thread(PlayPenaltyVideoThread).detach();
}

static void StartReverseGear(const wchar_t* title = L"REVERSE GEAR!", const wchar_t* subtitle = L"STEERING INVERTED")
{
    if (g_reverse_controls.exchange(true)) return;
    SetState(LightState::GREEN, title, subtitle, RGB(COL_MAGENTA.r, COL_MAGENTA.g, COL_MAGENTA.b));
    std::thread([]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        timeBeginPeriod(1);
        POINT last_pos;
        GetCursorPos(&last_pos);
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);

        while (g_reverse_controls.load() && g_running.load()) {
            POINT cur;
            GetCursorPos(&cur);
            int dx = cur.x - last_pos.x;
            int dy = cur.y - last_pos.y;

            if (dx != 0 || dy != 0) {
                int x = last_pos.x - dx;
                int y = last_pos.y - dy;
                if (x < 0) x = 0;
                if (x >= sw) x = sw - 1;
                if (y < 0) y = 0;
                if (y >= sh) y = sh - 1;

                SetCursorPos(x, y);
                last_pos.x = x;
                last_pos.y = y;
            }
            Sleep(1);
        }
        timeEndPeriod(1);
    }).detach();
}

static void StopReverseGear()
{
    if (!g_reverse_controls.exchange(false)) return;
    SetState(LightState::GREEN, L"GREEN", L"CURSOR: GO", RGB(52, 199, 89));
}

// ---------------------------------------------------------------------------
// Low-Level Keyboard Hook (Global ESC to Exit + S/R hotkeys)
// ---------------------------------------------------------------------------
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        if (kb->vkCode == VK_ESCAPE) {
            g_running.store(false);
            if (g_hwnd && IsWindow(g_hwnd)) {
                PostMessageW(g_hwnd, WM_CLOSE, 0, 0xDEAD);
            }
            return 1;
        }
        // Press 'S' to force-trigger speeding penalty video
        if (kb->vkCode == 'S' || kb->vkCode == 's') {
            TriggerSpeedingPenalty();
            return 1;
        }
        // Press 'R' to toggle Reverse Gear
        if (kb->vkCode == 'R' || kb->vkCode == 'r') {
            if (g_reverse_controls.load()) {
                StopReverseGear();
            } else {
                StartReverseGear(L"REVERSE GEAR!", L"STEERING INVERTED (KEY R)");
            }
            return 1;
        }
    }
    return CallNextHookEx(g_kbd_hook, nCode, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Low-Level Mouse Hook
// ---------------------------------------------------------------------------
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode < 0 || !g_enabled.load())
        return CallNextHookEx(g_mouse_hook, nCode, wParam, lParam);

    if (wParam == WM_MOUSEMOVE) {
        MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lParam;

        // Re-entrant event from our own SetCursorPos
        if (g_suppress_next.exchange(false)) {
            g_our_x = ms->pt.x;
            g_our_y = ms->pt.y;
            g_last_raw_x = ms->pt.x;
            g_last_raw_y = ms->pt.y;
            return CallNextHookEx(g_mouse_hook, nCode, wParam, lParam);
        }

        // Lazy-initialise
        if (g_our_x == -1) {
            g_our_x = ms->pt.x;
            g_our_y = ms->pt.y;
            g_last_raw_x = ms->pt.x;
            g_last_raw_y = ms->pt.y;
            g_speed_accum_dist = 0.0;
            g_speed_window_start = std::chrono::steady_clock::now();
        }

        // 1. Immobilized during speeding penalty
        if (g_speeding_penalized.load()) {
            return 1;
        }

        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);

        // 2. REVERSE GEAR: Handled smoothly by dedicated thread without hook echo!
        if (g_reverse_controls.load()) {
            g_speed_accum_dist = 0.0;
            g_speed_window_start = std::chrono::steady_clock::now();
            g_last_raw_x = ms->pt.x;
            g_last_raw_y = ms->pt.y;
            g_our_x = ms->pt.x;
            g_our_y = ms->pt.y;
            return CallNextHookEx(g_mouse_hook, nCode, wParam, lParam);
        }

        LightState st = g_state.load();

        // 3. RED light suppression -> cursor frozen
        if (st == LightState::RED) {
            g_speed_accum_dist = 0.0;
            g_speed_window_start = std::chrono::steady_clock::now();
            g_last_raw_x = ms->pt.x;
            g_last_raw_y = ms->pt.y;
            return 1;
        }

        // 4. SPEEDING VIOLATION DETECTION (Green and Yellow)
        if (g_last_raw_x != -1) {
            long raw_dx = ms->pt.x - g_last_raw_x;
            long raw_dy = ms->pt.y - g_last_raw_y;
            double step_dist = std::sqrt((double)(raw_dx * raw_dx + raw_dy * raw_dy));
            g_speed_accum_dist += step_dist;

            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_speed_window_start).count();

            if (elapsed_ms >= 100) {
                double speed_px_s = (g_speed_accum_dist * 1000.0) / (double)elapsed_ms;
                g_speed_accum_dist = 0.0;
                g_speed_window_start = now;

                double limit = (st == LightState::YELLOW) ? 700.0 : 1100.0;
                if (speed_px_s > limit) {
                    TriggerSpeedingPenalty();
                    return 1;
                }
            } else if (g_speed_accum_dist > 120.0) {
                // Sudden fast swipe
                g_speed_accum_dist = 0.0;
                g_speed_window_start = now;
                TriggerSpeedingPenalty();
                return 1;
            }
        }
        g_last_raw_x = ms->pt.x;
        g_last_raw_y = ms->pt.y;

        // 5. YELLOW light handling
        if (st == LightState::YELLOW) {
            double dx = (double)(ms->pt.x - g_our_x);
            double dy = (double)(ms->pt.y - g_our_y);

            if (dx == 0.0 && dy == 0.0)
                return 1;

            double mult = g_chaos.load() ? (0.02 + (rand() % 10) * 0.01) : 0.15;
            g_accum_x += dx * mult;
            g_accum_y += dy * mult;

            int move_x = (int)g_accum_x;
            int move_y = (int)g_accum_y;
            g_accum_x -= move_x;
            g_accum_y -= move_y;

            if (g_chaos.load() && (rand() % 6 == 0)) {
                move_x += (rand() % 5 - 2);
                move_y += (rand() % 5 - 2);
            }

            if (move_x != 0 || move_y != 0) {
                int tx = std::max(0, std::min(sw - 1, (int)(g_our_x + move_x)));
                int ty = std::max(0, std::min(sh - 1, (int)(g_our_y + move_y)));

                g_suppress_next.store(true);
                SetCursorPos(tx, ty);
            }
            return 1;
        }

        // 6. GREEN light handling
        if (g_chaos.load() && (rand() % 8 == 0)) {
            int jx = (rand() % 9 - 4);
            int jy = (rand() % 9 - 4);
            int tx = std::max(0, std::min(sw - 1, (int)(ms->pt.x + jx)));
            int ty = std::max(0, std::min(sh - 1, (int)(ms->pt.y + jy)));
            g_suppress_next.store(true);
            SetCursorPos(tx, ty);
            return 1;
        }

        // Normal Green pass-through
        g_our_x   = ms->pt.x;
        g_our_y   = ms->pt.y;
        g_accum_x = 0.0;
        g_accum_y = 0.0;
    }

    return CallNextHookEx(g_mouse_hook, nCode, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Traffic Cycle Thread
// ---------------------------------------------------------------------------
/*
 * Posts WM_TRAFFICSTATE to the main window with WPARAM = (int)LightState.
 * The main thread updates UI + cursor from there (thread-safe).
 */
static void SetState(LightState st, const wchar_t* line1, const wchar_t* line2, COLORREF col)
{
    g_state.store(st);
    g_accum_x = g_accum_y = 0.0;
    // Pack string pointers as LPARAM — they are string literals (static storage)
    // We use WM_TRAFFICSTATE with extra data via a simple struct posted on heap
    struct StateMsg { LightState st; std::wstring l1, l2; COLORREF col; };
    StateMsg* msg = new StateMsg{st, line1, line2, col};
    PostMessage(g_hwnd, WM_TRAFFICSTATE, 0, (LPARAM)msg);
}

static void CycleThread()
{
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> state_dist(0, 2);
    std::uniform_real_distribution<double> chaos_dur_dist(0.15, 0.85);
    std::uniform_int_distribution<int> msg_dist(0, CHAOS_MSG_COUNT - 1);
    std::uniform_int_distribution<int> rev_chance_dist(1, 100);

    int cycle_count = 0;

    while (g_running.load()) {
        if (!g_enabled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        // HYPER-CHAOS MODE
        if (g_chaos.load()) {
            LightState st = (LightState)state_dist(rng);
            const wchar_t* chaos_line = CHAOS_MSGS[msg_dist(rng)];
            const wchar_t* state_name = L"RED";
            COLORREF col = RGB(255, 59, 48);

            // 40% chance to toggle movement reversal in chaos
            if (rev_chance_dist(rng) <= 40) {
                if (g_reverse_controls.load()) {
                    StopReverseGear();
                } else {
                    StartReverseGear(L"REVERSED", chaos_line);
                }
            }

            if (!g_reverse_controls.load()) {
                if (st == LightState::YELLOW) {
                    state_name = L"YELLOW";
                    col = RGB(255, 204, 0);
                } else if (st == LightState::GREEN) {
                    state_name = L"GREEN";
                    col = RGB(52, 199, 89);
                }
                SetState(st, state_name, chaos_line, col);
            }

            double dur = chaos_dur_dist(rng);
            auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(dur);
            while (std::chrono::steady_clock::now() < end && g_running.load() && g_chaos.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            continue;
        }

        // If Reverse Gear is active (e.g. toggled by 'R'), pause cycle and wait!
        while (g_reverse_controls.load() && g_running.load() && g_enabled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // NORMAL CYCLE
        cycle_count++;

        // REVERSE GEAR: Every other cycle, activate Reverse Gear for 6 seconds!
        // Red light NEVER comes during reverse gear!
        if (cycle_count % 2 == 0) {
            StartReverseGear(L"REVERSE GEAR!", L"STEERING INVERTED (DRIVE IN REVERSE)");
            for (int i = 0; i < 60 && g_running.load() && g_enabled.load() && !g_chaos.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 6 seconds
            }
            StopReverseGear();
            if (!g_running.load() || !g_enabled.load() || g_chaos.load()) continue;
        }

        // RED LIGHT (only when NOT in reverse gear!)
        SetState(LightState::RED, L"RED", L"CURSOR: STOPPED", RGB(255, 59, 48));
        for (int i = 0; i < 60 && g_running.load() && g_enabled.load() && !g_chaos.load() && !g_reverse_controls.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 3 s

        if (!g_running.load() || !g_enabled.load() || g_chaos.load() || g_reverse_controls.load()) continue;

        // GREEN LIGHT
        SetState(LightState::GREEN, L"GREEN", L"CURSOR: GO", RGB(52, 199, 89));
        for (int i = 0; i < 80 && g_running.load() && g_enabled.load() && !g_chaos.load() && !g_reverse_controls.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 4 s

        if (!g_running.load() || !g_enabled.load() || g_chaos.load() || g_reverse_controls.load()) continue;

        // YELLOW LIGHT
        SetState(LightState::YELLOW, L"YELLOW", L"CURSOR: SLOW", RGB(255, 204, 0));
        for (int i = 0; i < 40 && g_running.load() && g_enabled.load() && !g_chaos.load() && !g_reverse_controls.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 2 s
    }
}

// ---------------------------------------------------------------------------
// GDI helpers
// ---------------------------------------------------------------------------
static void DrawFilledCircle(HDC hdc, int cx, int cy, int r, RGB3 col)
{
    HBRUSH hBr  = CreateSolidBrush(RGB(col.r, col.g, col.b));
    HPEN   hPen = CreatePen(PS_SOLID, 1, RGB(17,17,17));
    HBRUSH ob   = (HBRUSH)SelectObject(hdc, hBr);
    HPEN   op   = (HPEN)SelectObject(hdc, hPen);
    Ellipse(hdc, cx-r, cy-r, cx+r, cy+r);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(hBr);
    DeleteObject(hPen);
}

// ---------------------------------------------------------------------------
// Chaos auto-end timer callback
// ---------------------------------------------------------------------------
static VOID CALLBACK ChaosTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    g_chaos.store(false);
    g_reverse_controls.store(false);
    PostMessageW(g_hwnd, WM_CHAOSBTN, 0, 0);
    KillTimer(g_hwnd, 1);
}

// ---------------------------------------------------------------------------
// Window Procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE:
    {
        // Buttons
        g_btn_chaos  = CreateWindowExW(0, L"BUTTON", L"CHAOS MODE",
            WS_CHILD|WS_VISIBLE|BS_FLAT,
            15, 265, 170, 28, hwnd, (HMENU)ID_BTN_CHAOS,
            GetModuleHandle(nullptr), nullptr);
        g_btn_toggle = CreateWindowExW(0, L"BUTTON", L"DISABLE",
            WS_CHILD|WS_VISIBLE|BS_FLAT,
            15, 300, 170, 28, hwnd, (HMENU)ID_BTN_TOGGLE,
            GetModuleHandle(nullptr), nullptr);

        HFONT hFont = CreateFontW(14,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH, L"Arial");
        SendMessageW(g_btn_chaos,  WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(g_btn_toggle, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }

    case WM_TRAFFICSTATE:
    {
        struct StateMsg { LightState st; std::wstring l1, l2; COLORREF col; };
        StateMsg* sm = (StateMsg*)lp;
        if (!sm) break;

        g_status_line1 = sm->l1;
        g_status_line2 = sm->l2;
        g_status_color = sm->col;

        // Update lamp colors
        g_lamp_r = COL_DIM_R; g_lamp_y = COL_DIM_Y; g_lamp_g = COL_DIM_G;
        switch (sm->st) {
            case LightState::RED:    g_lamp_r = COL_RED;    break;
            case LightState::YELLOW: g_lamp_y = COL_YELLOW; break;
            case LightState::GREEN:  g_lamp_g = COL_GREEN;  break;
        }

        if (g_reverse_controls.load()) {
            g_lamp_r = COL_DIM_R;   // Red lamp is completely DARK
            g_lamp_y = COL_DIM_Y;   // Yellow lamp is completely DARK
            g_lamp_g = COL_MAGENTA; // Only bottom lamp is illuminated
            ApplyOsCursor(LightState::GREEN);
        } else {
            ApplyOsCursor(sm->st);
        }

        delete sm;
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_CHAOSBTN:
    {
        if (!g_chaos.load()) {
            SetWindowTextW(g_btn_chaos, L"CHAOS MODE");
        }
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wp);

        // FAKE DISABLE BUTTON
        if (id == ID_BTN_TOGGLE) {
            int idx = rand() % TROLL_POPUP_COUNT;
            const wchar_t* trollMsg = TROLL_POPUPS[idx];

            const wchar_t* fakeLabels[] = { L"DENIED", L"NICE TRY", L"IMPOSSIBLE", L"NO WAY", L"HA HA", L"NEVER" };
            SetWindowTextW(g_btn_toggle, fakeLabels[rand() % 6]);

            MessageBoxW(hwnd, trollMsg, L"Traffic Authority Alert", MB_ICONWARNING | MB_SYSTEMMODAL);

            SetWindowTextW(g_btn_toggle, L"DISABLE");
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        // CHAOS MODE BUTTON
        if (id == ID_BTN_CHAOS) {
            bool now = !g_chaos.load();
            g_chaos.store(now);
            if (now) {
                SetWindowTextW(g_btn_chaos, L"EXIT CHAOS");
                SetTimer(hwnd, 1, 15000, ChaosTimerProc); // auto-end after 15 s
            } else {
                g_chaos.store(false);
                g_reverse_controls.store(false);
                SetWindowTextW(g_btn_chaos, L"CHAOS MODE");
                KillTimer(hwnd, 1);
            }
        }
        return 0;
    }

    case WM_SYSCOMMAND:
        // Intercept Alt+F4 or system menu close
        if ((wp & 0xFFF0) == SC_CLOSE) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_CLOSE:
    {
        // Real close only allowed if triggered by ESC (lp == 0xDEAD) or app terminating
        if (lp == 0xDEAD || !g_running.load()) {
            DestroyWindow(hwnd);
            return 0;
        }

        // FAKE CLOSE BUTTON TROLL POPUP
        int idx = rand() % TROLL_POPUP_COUNT;
        MessageBoxW(hwnd, TROLL_POPUPS[idx], L"Error 418: I am a coconut", MB_ICONHAND | MB_SYSTEMMODAL);
        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // Background
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bgBr = CreateSolidBrush(RGB(18,18,18));
        FillRect(hdc, &rc, bgBr);
        DeleteObject(bgBr);

        // Outer panel rect
        RECT panel = {8, 8, rc.right-8, rc.bottom-8};
        HBRUSH panelBr = CreateSolidBrush(RGB(30,30,30));
        FillRect(hdc, &panel, panelBr);
        DeleteObject(panelBr);
        HPEN panelPen = CreatePen(PS_SOLID, 1, RGB(60,60,60));
        HPEN oldP = (HPEN)SelectObject(hdc, panelPen);
        HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH oldB = (HBRUSH)SelectObject(hdc, nb);
        Rectangle(hdc, panel.left, panel.top, panel.right, panel.bottom);
        SelectObject(hdc, oldP); SelectObject(hdc, oldB);
        DeleteObject(panelPen);

        // Light box background
        RECT lightBox = {panel.left+8, panel.top+8, panel.right-8, panel.top+210};
        HBRUSH lbBr = CreateSolidBrush(RGB(10,10,10));
        FillRect(hdc, &lightBox, lbBr);
        DeleteObject(lbBr);

        // Draw lamps
        int cx = (rc.right / 2);
        DrawFilledCircle(hdc, cx, 55,  22, g_lamp_r);
        DrawFilledCircle(hdc, cx, 115, 22, g_lamp_y);
        DrawFilledCircle(hdc, cx, 175, 22, g_lamp_g);

        // Status text
        SetBkMode(hdc, TRANSPARENT);
        HFONT hFont = CreateFontW(13,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,
            DEFAULT_PITCH|FF_MODERN, L"Consolas");
        HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
        SetTextColor(hdc, g_status_color);

        RECT txtRc = {panel.left+5, panel.top+215, panel.right-5, panel.top+260};
        std::wstring fullStatus = g_status_line1;
        if (!g_status_line2.empty()) fullStatus += L"\n" + g_status_line2;
        DrawTextW(hdc, fullStatus.c_str(), -1, &txtRc,
            DT_CENTER | DT_WORDBREAK | DT_VCENTER);

        SelectObject(hdc, oldFont);
        DeleteObject(hFont);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORBTN:
    {
        HWND hBtn = (HWND)lp;
        HDC  hdcBtn = (HDC)wp;
        SetBkMode(hdcBtn, OPAQUE);
        SetTextColor(hdcBtn, RGB(255,255,255));
        if (hBtn == g_btn_toggle) {
            SetBkColor(hdcBtn, RGB(235,87,87));
            static HBRUSH hBrToggle = CreateSolidBrush(RGB(235,87,87));
            return (LRESULT)hBrToggle;
        }
        if (hBtn == g_btn_chaos) {
            bool ch = g_chaos.load();
            SetBkColor(hdcBtn, ch ? RGB(255,0,128) : RGB(51,51,51));
            static HBRUSH hBrChaos_on  = CreateSolidBrush(RGB(255,0,128));
            static HBRUSH hBrChaos_off = CreateSolidBrush(RGB(51,51,51));
            return (LRESULT)(ch ? hBrChaos_on : hBrChaos_off);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_DESTROY:
        g_running.store(false);
        if (g_mouse_hook) { UnhookWindowsHookEx(g_mouse_hook); g_mouse_hook = nullptr; }
        if (g_kbd_hook)   { UnhookWindowsHookEx(g_kbd_hook);   g_kbd_hook   = nullptr; }
        RestoreOsCursor();
        if (g_cursor_red)    { DestroyCursor(g_cursor_red);    g_cursor_red    = nullptr; }
        if (g_cursor_yellow) { DestroyCursor(g_cursor_yellow); g_cursor_yellow = nullptr; }
        if (g_cursor_green)  { DestroyCursor(g_cursor_green);  g_cursor_green  = nullptr; }
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);

    srand((unsigned int)time(nullptr));

    // Build colored cursors
    g_cursor_red    = MakeColorCursor(COL_RED.r,    COL_RED.g,    COL_RED.b);
    g_cursor_yellow = MakeColorCursor(COL_YELLOW.r, COL_YELLOW.g, COL_YELLOW.b);
    g_cursor_green  = MakeColorCursor(COL_GREEN.r,  COL_GREEN.g,  COL_GREEN.b);

    // Register window class
    WNDCLASSEXW wc  = {};
    wc.cbSize       = sizeof(wc);
    wc.lpfnWndProc  = WndProc;
    wc.hInstance    = hInst;
    wc.hCursor      = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground= CreateSolidBrush(RGB(18,18,18));
    wc.lpszClassName= L"TrafficLightCursor";
    RegisterClassExW(&wc);

    // Create main window
    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"TrafficLightCursor",
        L"Traffic Light Cursor",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 216, 370,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    // Install low-level mouse and keyboard hooks
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
    g_kbd_hook   = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);

    // Start cycle thread
    std::thread cycle(CycleThread);
    cycle.detach();

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}

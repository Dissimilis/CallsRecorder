#include "DebugWindow.h"
#include "Recorder.h"
#include <cstdio>

static HWND g_dbgWnd;
static HWND g_dbgLog;
static HFONT g_uiFont, g_monoFont;
static Recorder* g_rec;
static unsigned long long g_logCursor;
static int g_dpi = 96;

static const int kWaveTop = 40;
static const int kWaveHeight = 90;
static const UINT kDbgTimer = 1;

static int Scale(int v) { return MulDiv(v, g_dpi, 96); }

static void CreateFonts() {
    if (g_uiFont) DeleteObject(g_uiFont);
    if (g_monoFont) DeleteObject(g_monoFont);
    g_uiFont = CreateFontW(-Scale(16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                           0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_monoFont = CreateFontW(-Scale(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
}

static void LayoutChildren(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    MoveWindow(g_dbgLog, Scale(8), Scale(kWaveTop + kWaveHeight + 8), rc.right - Scale(16),
               rc.bottom - Scale(kWaveTop + kWaveHeight + 16), TRUE);
}

static void AppendLogLines() {
    auto lines = LogTail(g_logCursor);
    if (lines.empty()) return;
    std::wstring text;
    for (auto& l : lines) text += l + L"\r\n";

    int len = GetWindowTextLengthW(g_dbgLog);
    if (len > 48000) { // keep the box from growing forever
        SendMessageW(g_dbgLog, EM_SETSEL, 0, len / 2);
        SendMessageW(g_dbgLog, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(g_dbgLog);
    }
    SendMessageW(g_dbgLog, EM_SETSEL, len, len);
    SendMessageW(g_dbgLog, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    SendMessageW(g_dbgLog, EM_SCROLLCARET, 0, 0);
}

static void DrawStatusAndWave(HDC dc, const RECT& rc) {
    int w = rc.right - rc.left;

    // Status line
    RECT statusRc = {Scale(8), Scale(8), w - Scale(8), Scale(kWaveTop)};
    FillRect(dc, &statusRc, (HBRUSH)(COLOR_WINDOW + 1));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_uiFont);
    wchar_t status[256];
    bool rec = g_rec->recording();
    if (rec) {
        ULONGLONG s = g_rec->elapsedMs() / 1000;
        std::wstring app = g_rec->currentApp();
        swprintf(status, 256, L"RECORDING   %02llu:%02llu:%02llu%ls%ls",
                 s / 3600, (s / 60) % 60, s % 60,
                 app.empty() ? L"" : L"   call app: ", app.c_str());
        SetTextColor(dc, RGB(200, 30, 30));
    } else {
        wcscpy(status, L"Idle");
        SetTextColor(dc, RGB(60, 60, 60));
    }
    DrawTextW(dc, status, -1, &statusRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Waveform strip (double-buffered)
    RECT waveRc = {Scale(8), Scale(kWaveTop), w - Scale(8), Scale(kWaveTop + kWaveHeight)};
    int ww = waveRc.right - waveRc.left, wh = waveRc.bottom - waveRc.top;
    if (ww <= 0) return;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, ww, wh);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(RGB(24, 24, 28));
    RECT full = {0, 0, ww, wh};
    FillRect(mem, &full, bg);
    DeleteObject(bg);

    float levels[Recorder::kLevelCount];
    size_t n = g_rec->recording() ? g_rec->levelHistory(levels, Recorder::kLevelCount) : 0;
    int mid = wh / 2;

    HPEN axis = CreatePen(PS_SOLID, 1, RGB(70, 70, 80));
    HGDIOBJ oldPen = SelectObject(mem, axis);
    MoveToEx(mem, 0, mid, nullptr);
    LineTo(mem, ww, mid);

    if (n) {
        HPEN wave = CreatePen(PS_SOLID, Scale(2), RGB(80, 210, 120));
        SelectObject(mem, wave);
        int step = Scale(3); // px per 50 ms block
        for (size_t i = 0; i < n; i++) {
            int x = (int)(ww - step - (n - 1 - i) * step);
            if (x < 0) continue;
            int h = (int)(levels[i] * (mid - Scale(3)));
            if (h < 1) h = 1;
            MoveToEx(mem, x, mid - h, nullptr);
            LineTo(mem, x, mid + h);
        }
        SelectObject(mem, oldPen);
        DeleteObject(wave);
    }
    SelectObject(mem, oldPen);
    DeleteObject(axis);

    BitBlt(dc, waveRc.left, waveRc.top, ww, wh, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK DbgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_dpi = (int)GetDpiForWindow(hwnd);
            CreateFonts();
            g_dbgLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                           ES_READONLY | ES_AUTOVSCROLL,
                                       0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(g_dbgLog, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
            g_logCursor = 0;
            AppendLogLines();
            SetTimer(hwnd, kDbgTimer, 200, nullptr);
            return 0;
        }
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            CreateFonts();
            SendMessageW(g_dbgLog, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_SIZE:
            LayoutChildren(hwnd);
            return 0;
        case WM_TIMER: {
            AppendLogLines();
            RECT rc;
            GetClientRect(hwnd, &rc);
            rc.bottom = Scale(kWaveTop + kWaveHeight);
            InvalidateRect(hwnd, &rc, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            DrawStatusAndWave(dc, rc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kDbgTimer);
            DeleteObject(g_uiFont);
            DeleteObject(g_monoFont);
            g_uiFont = g_monoFont = nullptr;
            g_dbgWnd = nullptr;
            g_dbgLog = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void DebugWindow_Show(HINSTANCE inst, Recorder* rec) {
    g_rec = rec;
    if (g_dbgWnd) {
        ShowWindow(g_dbgWnd, SW_SHOW);
        SetForegroundWindow(g_dbgWnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DbgProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"CallsRecorderDebugWnd";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        if (!RegisterClassW(&wc)) {
            LogLine(L"DebugWindow: RegisterClass failed err=%lu", GetLastError());
            return;
        }
        registered = true;
    }
    g_dbgWnd = CreateWindowExW(0, L"CallsRecorderDebugWnd", L"CallsRecorder debug",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               560, 460, nullptr, nullptr, inst, nullptr);
    if (!g_dbgWnd) {
        LogLine(L"DebugWindow: CreateWindowEx failed err=%lu", GetLastError());
        return;
    }
    // Size for the monitor's DPI (initial size above is 96-dpi units).
    int dpi = (int)GetDpiForWindow(g_dbgWnd);
    SetWindowPos(g_dbgWnd, nullptr, 0, 0, MulDiv(560, dpi, 96), MulDiv(460, dpi, 96),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(g_dbgWnd, SW_SHOW);
}

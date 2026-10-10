#define _WIN32_WINNT 0x0602
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>

namespace {

constexpr wchar_t kClassName[] = L"NetMeterWindow";
constexpr wchar_t kMutexName[] = L"Global\\NetMeter_AntiAlias_v2";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTimerId = 1;
constexpr UINT kRefreshMs = 500;
constexpr UINT kExitMenuId = 100;
constexpr WORD kAppIconResourceId = 101;
constexpr int kMeterWidth = 180;
constexpr int kMeterHeight = 40;
// Match the Python version's camouflage colors; per-pixel alpha leaves only
// the text visible over the taskbar.
constexpr COLORREF kLightBackground = RGB(230, 230, 230);
constexpr COLORREF kDarkBackground = RGB(18, 18, 18);

struct AppState {
    HWND controller = nullptr; // Independent of Explorer; survives taskbar destruction.
    HWND meter = nullptr;
    HWND taskbar = nullptr;
    HWND trayArea = nullptr;
    HANDLE mutex = nullptr;
    HICON trayIcon = nullptr;
    UINT taskbarCreated = 0;
    bool trayAdded = false;
    bool darkTheme = true;
    bool fallbackMode = false;
    bool haveSample = false;
    uint64_t lastReceived = 0;
    uint64_t lastSent = 0;
    ULONGLONG lastSampleTick = 0;
    std::wstring label = L"Init...";
} g;

struct NetworkCounters {
    uint64_t received = 0;
    uint64_t sent = 0;
};

NetworkCounters ReadNetworkCounters() {
    // Ask Windows which interface it would route public IPv4 traffic through.
    // Summing every adapter double-counts packets when VPNs and virtual NICs
    // mirror traffic from the physical interface.
    SOCKADDR_IN destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = 0x01010101; // 1.1.1.1, same byte order on either endian.
    NET_IFINDEX interfaceIndex = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&destination), &interfaceIndex) != NO_ERROR)
        return {};

    MIB_IF_ROW2 row{};
    row.InterfaceIndex = interfaceIndex;
    if (GetIfEntry2(&row) != NO_ERROR) return {};
    return { row.InOctets, row.OutOctets };
}

std::wstring FormatSpeed(double bytesPerSecond) {
    const double bits = std::max(0.0, bytesPerSecond) * 8.0;
    wchar_t buffer[64]{};
    if (bits < 1000.0) {
        swprintf_s(buffer, L"%.0f bps", bits);
    } else if (bits < 1000000.0) {
        swprintf_s(buffer, L"%.1f Kbps", bits / 1000.0);
    } else {
        swprintf_s(buffer, L"%.1f Mbps", bits / 1000000.0);
    }
    return buffer;
}

void UpdateTheme() {
    HKEY key = nullptr;
    DWORD value = 0, size = sizeof(value);
    const LONG status = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &key);
    if (status == ERROR_SUCCESS) {
        const LONG query = RegQueryValueExW(key, L"SystemUsesLightTheme", nullptr,
            nullptr, reinterpret_cast<BYTE*>(&value), &size);
        RegCloseKey(key);
        g.darkTheme = (query != ERROR_SUCCESS || value == 0);
    } else {
        g.darkTheme = true;
    }
}

COLORREF CurrentBackground() { return g.darkTheme ? kDarkBackground : kLightBackground; }

void ApplyWindowStyles(HWND hwnd) {
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (GetParent(hwnd) != nullptr) exStyle &= ~static_cast<LONG_PTR>(WS_EX_TOPMOST);
    exStyle |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
}

void CreateMeter();
void PositionMeter();

bool AttachToTaskbar(HWND taskbar) {
    if (!g.meter || !IsWindow(g.meter) || !taskbar || !IsWindow(taskbar)) return false;

    // Match the Python implementation: change the style before reparenting.
    LONG_PTR style = GetWindowLongPtrW(g.meter, GWL_STYLE);
    style = (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_CHILD;
    SetWindowLongPtrW(g.meter, GWL_STYLE, style);

    // SetParent can fail across integrity boundaries or on unsupported shell layouts.
    SetLastError(ERROR_SUCCESS);
    HWND previous = SetParent(g.meter, taskbar);
    if (!previous && GetLastError() != ERROR_SUCCESS) return false;

    SetWindowLongPtrW(g.meter, GWL_STYLE, style | WS_VISIBLE);
    ApplyWindowStyles(g.meter);
    g.taskbar = taskbar;
    g.trayArea = FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    g.fallbackMode = false;
    // Put the meter above the taskbar's other child windows once, when it is
    // attached. Routine refreshes only move it when its position changes.
    SetWindowPos(g.meter, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    PositionMeter();
    return true;
}

void FloatBesideTray() {
    if (!g.meter || !IsWindow(g.meter)) return;
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND tray = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    if (!tray) return;

    RECT trayRect{};
    if (!GetWindowRect(tray, &trayRect)) return;
    const int x = trayRect.left - kMeterWidth - 1;
    const int y = trayRect.top + ((trayRect.bottom - trayRect.top) - kMeterHeight) / 2;

    // If the taskbar is gone or embedding failed, keep a top-level meter alive.
    LONG_PTR style = GetWindowLongPtrW(g.meter, GWL_STYLE);
    style = (style & ~static_cast<LONG_PTR>(WS_CHILD)) | WS_POPUP;
    SetWindowLongPtrW(g.meter, GWL_STYLE, style);
    if (GetParent(g.meter) != nullptr) SetParent(g.meter, nullptr);
    g.taskbar = taskbar;
    g.trayArea = tray;
    g.fallbackMode = true;
    ApplyWindowStyles(g.meter);
    RECT current{};
    if (!GetWindowRect(g.meter, &current) || current.left != x || current.top != y ||
        current.right - current.left != kMeterWidth || current.bottom - current.top != kMeterHeight) {
        SetWindowPos(g.meter, HWND_TOPMOST, x, y, kMeterWidth, kMeterHeight,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void PositionMeter() {
    if (!g.meter || !IsWindow(g.meter)) return;
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) return;
    HWND tray = FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    if (!tray) return;
    RECT rect{};
    if (!GetWindowRect(tray, &rect)) return;

    if (GetParent(g.meter) == taskbar) {
        POINT point{ rect.left - kMeterWidth - 1, rect.top + ((rect.bottom - rect.top) - kMeterHeight) / 2 };
        MapWindowPoints(nullptr, taskbar, &point, 1);
        // Windows 11's taskbar composition surface can cover child windows
        // after repainting. Keep the child at the top of the taskbar z-order,
        // as the Python implementation does.
        SetWindowPos(g.meter, HWND_TOP, point.x, point.y, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        FloatBesideTray();
    }
}

void EnsureMeterAndTaskbar() {
    // Keep the meter independent from Explorer, then reposition it beside the
    // tray when Explorer recreates the taskbar.
    if (!g.meter || !IsWindow(g.meter)) {
        g.meter = nullptr;
        CreateMeter();
    }

    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) return;

    HWND tray = FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    if (!tray) return;
    g.trayArea = tray;
    if (GetParent(g.meter) != taskbar) {
        if (!AttachToTaskbar(taskbar)) {
            FloatBesideTray();
            return;
        }
    }
    PositionMeter();
}

void UpdateNetwork() {
    const NetworkCounters counters = ReadNetworkCounters();
    const uint64_t received = counters.received;
    const uint64_t sent = counters.sent;
    const ULONGLONG now = GetTickCount64();
    if (!g.haveSample) {
        g.haveSample = true;
        g.lastReceived = received;
        g.lastSent = sent;
        g.lastSampleTick = now;
        g.label = L"\x2193 0 bps\n\x2191 0 bps";
    } else {
        const ULONGLONG elapsed = now - g.lastSampleTick;
        if (elapsed == 0) return;
        const double seconds = elapsed / 1000.0;
        const double down = received >= g.lastReceived ? (received - g.lastReceived) / seconds : 0.0;
        const double up = sent >= g.lastSent ? (sent - g.lastSent) / seconds : 0.0;
        g.label = L"\x2193 " + FormatSpeed(down) + L"\n\x2191 " + FormatSpeed(up);
        g.lastReceived = received;
        g.lastSent = sent;
        g.lastSampleTick = now;
    }
    if (g.meter && IsWindow(g.meter)) InvalidateRect(g.meter, nullptr, TRUE);
}

void AddTrayIcon() {
    if (!g.trayIcon) {
        // Load the same bundled network glyph used as the executable icon.
        g.trayIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    }
    if (!g.trayIcon) g.trayIcon = LoadIconW(nullptr, IDI_APPLICATION);

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g.controller;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = g.trayIcon;
    wcscpy_s(data.szTip, L"Network Meter");
    g.trayAdded = Shell_NotifyIconW(NIM_ADD, &data) == TRUE;
    if (g.trayAdded) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kExitMenuId, L"Exit Network Meter");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(g.controller);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y, 0, g.controller, nullptr);
    PostMessageW(g.controller, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void CreateMeter() {
    if (g.meter && IsWindow(g.meter)) return;
    g.meter = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName, L"Network Meter", WS_POPUP, -32000, -32000,
        kMeterWidth, kMeterHeight, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g.meter) return;
    ApplyWindowStyles(g.meter);
    ShowWindow(g.meter, SW_SHOWNOACTIVATE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g.taskbarCreated && g.taskbarCreated != 0) {
        g.taskbar = nullptr;
        g.trayArea = nullptr;
        // Recreate the tray icon too; Explorer owns the notification area.
        AddTrayIcon();
        EnsureMeterAndTaskbar();
        return 0;
    }

    if (hwnd == g.controller) {
        switch (message) {
        case WM_CREATE:
            SetTimer(hwnd, kTimerId, kRefreshMs, nullptr);
            return 0;
        case WM_TIMER:
        {
            const bool wasDark = g.darkTheme;
            UpdateTheme();
            if (wasDark != g.darkTheme && g.meter && IsWindow(g.meter)) {
                ApplyWindowStyles(g.meter);
                InvalidateRect(g.meter, nullptr, TRUE);
            }
            UpdateNetwork();
            EnsureMeterAndTaskbar();
            return 0;
        }
        case kTrayMessage:
        {
            // With NOTIFYICON_VERSION_4, the event code is in LOWORD(lParam)
            // and the icon ID is in HIWORD(lParam). Older shell versions send
            // the event code directly as lParam.
            const UINT event = LOWORD(lParam);
            if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU ||
                static_cast<UINT>(lParam) == WM_RBUTTONUP ||
                static_cast<UINT>(lParam) == WM_CONTEXTMENU ||
                event == WM_LBUTTONUP) ShowTrayMenu();
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == kExitMenuId) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            KillTimer(hwnd, kTimerId);
            if (g.trayAdded) {
                NOTIFYICONDATAW data{};
                data.cbSize = sizeof(data); data.hWnd = hwnd; data.uID = 1;
                Shell_NotifyIconW(NIM_DELETE, &data);
            }
            if (g.meter && IsWindow(g.meter)) DestroyWindow(g.meter);
            if (g.trayIcon) {
                DestroyIcon(g.trayIcon);
                g.trayIcon = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        }
        }
    } else if (hwnd == g.meter) {
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);

            // Render a complete layered frame so the text composites above
            // the Windows 11 taskbar's XAML surface.
            HDC frame = CreateCompatibleDC(nullptr);
            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = client.right;
            bitmapInfo.bmiHeader.biHeight = -client.bottom;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            HBITMAP bitmap = frame ? CreateDIBSection(frame, &bitmapInfo, DIB_RGB_COLORS,
                &pixels, nullptr, 0) : nullptr;
            if (frame && bitmap && pixels) {
                HGDIOBJ oldBitmap = SelectObject(frame, bitmap);
                RECT frameRect{ 0, 0, client.right, client.bottom };
                HBRUSH brush = CreateSolidBrush(CurrentBackground());
                FillRect(frame, &frameRect, brush);
                DeleteObject(brush);
                SetBkMode(frame, TRANSPARENT);
                SetTextColor(frame, g.darkTheme ? RGB(255, 255, 255) : RGB(0, 0, 0));
                // Use the taskbar's Segoe UI family at clock-sized text height.
                // Keep grayscale antialiasing for clean alpha compositing.
                HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                HGDIOBJ oldFont = SelectObject(frame, font);
                const size_t newline = g.label.find(L'\n');
                if (newline != std::wstring::npos) {
                    RECT first{ 0, 2, client.right - 35, client.bottom / 2 };
                    RECT second{ 0, client.bottom / 2 - 1, client.right - 35, client.bottom - 1 };
                    DrawTextW(frame, g.label.substr(0, newline).c_str(), -1, &first,
                        DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    DrawTextW(frame, g.label.substr(newline + 1).c_str(), -1, &second,
                        DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                } else {
                    RECT line = frameRect;
                    DrawTextW(frame, g.label.c_str(), -1, &line,
                        DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
                SelectObject(frame, oldFont);

                // GDI doesn't populate the alpha byte of a DIB. Convert the
                // grayscale glyph coverage to premultiplied alpha so the
                // color-key camouflage cannot leave a colored taskbar patch.
                const int background = g.darkTheme ? 18 : 230;
                const int foreground = g.darkTheme ? 255 : 0;
                const int span = std::abs(foreground - background);
                auto* bgra = static_cast<uint32_t*>(pixels);
                const size_t pixelCount = static_cast<size_t>(client.right) * client.bottom;
                for (size_t i = 0; i < pixelCount; ++i) {
                    const int red = static_cast<int>((bgra[i] >> 16) & 0xFF);
                    const int coverage = std::clamp(
                        std::abs(red - background) * 255 / span, 0, 255);
                    const uint32_t color = foreground ? static_cast<uint32_t>(coverage) : 0;
                    bgra[i] = (static_cast<uint32_t>(coverage) << 24) |
                        (color << 16) | (color << 8) | color;
                }

                RECT windowRect{};
                GetWindowRect(hwnd, &windowRect);
                POINT destination{ windowRect.left, windowRect.top };
                HWND parent = GetParent(hwnd);
                if (parent) MapWindowPoints(nullptr, parent, &destination, 1);
                POINT source{ 0, 0 };
                SIZE size{ client.right, client.bottom };
                BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                HDC screen = GetDC(nullptr);
                UpdateLayeredWindow(hwnd, screen, &destination, &size, frame,
                    &source, 0, &blend, ULW_ALPHA);
                ReleaseDC(nullptr, screen);
                DeleteObject(font);
                SelectObject(frame, oldBitmap);
                DeleteObject(bitmap);
            }
            if (frame) DeleteDC(frame);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_DESTROY:
            if (g.meter == hwnd) g.meter = nullptr;
            return 0;
        }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc) != 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g.mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!g.mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDPIAware();
    if (!RegisterWindowClass(instance)) return 1;
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // A hidden top-level controller remains independent when Explorer destroys
    // the taskbar and all of its child windows.
    g.controller = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"NetMeterController",
        WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g.controller) return 1;
    ShowWindow(g.controller, SW_HIDE);
    SetTimer(g.controller, kTimerId, kRefreshMs, nullptr);

    AddTrayIcon();
    CreateMeter();
    UpdateTheme();
    EnsureMeterAndTaskbar();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g.mutex) CloseHandle(g.mutex);
    return static_cast<int>(msg.wParam);
}

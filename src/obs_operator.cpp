// MapleMultiMouse OBS operator HUD — deliberately does not take focus.
// SPDX-License-Identifier: MIT
#include "obs_operator.h"
#include "openmouse.h"

#include <winhttp.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace om {
namespace {

constexpr wchar_t kPanelClass[] = L"MapleMultiMouse.OBSOperator";
constexpr UINT kRefreshMsg = WM_APP + 25;
constexpr int kWidth = 248, kHeight = 244;
constexpr int kButtonX = 18, kButtonW = 212, kButtonH = 40;
constexpr int kButtonY[] = { 64, 112, 160 };
constexpr char const* kActions[] = { "scene_a", "scene_b", "toggle_mic" };

struct OperatorState {
    HWND window = nullptr;
    RECT screenRect{};
    unsigned short port = 4456;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::thread worker;
    std::mutex lock;
    std::condition_variable wake;
    std::deque<std::string> actions;
};
OperatorState g_operator;

struct HttpHandle {
    HINTERNET handle = nullptr;
    ~HttpHandle() { if (handle) WinHttpCloseHandle(handle); }
};

bool BridgeHttp(const wchar_t* method, const wchar_t* path,
                const std::string& body, std::string& response) {
    HttpHandle session{WinHttpOpen(L"MapleMultiMouse/0.1",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.handle) return false;
    WinHttpSetTimeouts(session.handle, 800, 800, 800, 1000);

    HttpHandle connection{WinHttpConnect(session.handle, L"127.0.0.1",
                                           (INTERNET_PORT)g_operator.port, 0)};
    if (!connection.handle) return false;
    HttpHandle request{WinHttpOpenRequest(connection.handle, method, path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)};
    if (!request.handle) return false;

    const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                           : L"Content-Type: application/json\r\n";
    if (!WinHttpSendRequest(request.handle, headers, (DWORD)-1,
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                            (DWORD)body.size(), (DWORD)body.size(), 0)) return false;
    if (!WinHttpReceiveResponse(request.handle, nullptr)) return false;

    DWORD code = 0, codeSize = sizeof(code);
    if (!WinHttpQueryHeaders(request.handle,
          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize, WINHTTP_NO_HEADER_INDEX))
        return false;

    response.clear();
    for (;;) {
        char buffer[1024];
        DWORD bytes = 0;
        if (!WinHttpReadData(request.handle, buffer, sizeof(buffer), &bytes)) return false;
        if (!bytes) break;
        response.append(buffer, bytes);
        if (response.size() > 4096) return false;
    }
    return code == 200;
}

void WorkLoop() {
    while (g_operator.running.load()) {
        std::string action;
        {
            std::unique_lock<std::mutex> lock(g_operator.lock);
            g_operator.wake.wait_for(lock, std::chrono::seconds(2), [] {
                return !g_operator.running.load() || !g_operator.actions.empty();
            });
            if (!g_operator.running.load()) break;
            if (!g_operator.actions.empty()) {
                action = g_operator.actions.front();
                g_operator.actions.pop_front();
            }
        }
        std::string response;
        if (!action.empty()) {
            const std::string body = "{\"action\":\"" + action + "\"}";
            if (!BridgeHttp(L"POST", L"/action", body, response))
                LogLine(L"[obs-operator] action failed (check bridge / OBS scene names)");
        }
        const bool ok = BridgeHttp(L"GET", L"/health", "", response) &&
                        response.find("\"connected\":true") != std::string::npos;
        if (g_operator.connected.exchange(ok) != ok && g_operator.window)
            PostMessageW(g_operator.window, kRefreshMsg, 0, 0);
    }
}

struct MonitorChoice { RECT work{}; bool secondary = false; };
BOOL CALLBACK PickMonitor(HMONITOR mon, HDC, LPRECT, LPARAM ctx) {
    auto* chosen = reinterpret_cast<MonitorChoice*>(ctx);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        const bool secondary = (mi.dwFlags & MONITORINFOF_PRIMARY) == 0;
        if (secondary && !chosen->secondary) {
            chosen->work = mi.rcWork;
            chosen->secondary = true;
        } else if (!chosen->secondary) {
            chosen->work = mi.rcWork;
        }
    }
    return TRUE;
}

void DrawButton(HDC dc, int index, const wchar_t* label) {
    RECT rc{kButtonX, kButtonY[index], kButtonX + kButtonW,
            kButtonY[index] + kButtonH};
    HBRUSH brush = CreateSolidBrush(RGB(42, 71, 106));
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
    SetTextColor(dc, RGB(244, 247, 252));
    DrawTextW(dc, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK PanelProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case kRefreshMsg: InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT rc{};
        GetClientRect(window, &rc);
        HBRUSH background = CreateSolidBrush(RGB(23, 27, 34));
        FillRect(dc, &rc, background);
        DeleteObject(background);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(236, 240, 246));
        RECT title{16, 16, kWidth - 16, 48};
        DrawTextW(dc, L"OBS OPERATOR", -1, &title,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawButton(dc, 0, L"Scene A");
        DrawButton(dc, 1, L"Scene B");
        DrawButton(dc, 2, L"Toggle microphone");
        RECT status{16, 209, kWidth - 16, 237};
        SetTextColor(dc, g_operator.connected.load()
                         ? RGB(131, 235, 158) : RGB(255, 179, 105));
        DrawTextW(dc, g_operator.connected.load() ? L"OBS connected"
                                                  : L"OBS / bridge offline",
                  -1, &status, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(window, &paint);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(window, message, wp, lp);
}

void EnqueueAction(const char* action) {
    {
        std::lock_guard<std::mutex> guard(g_operator.lock);
        // Never block raw input: keep a short bounded action queue.
        if (g_operator.actions.size() >= 8) return;
        g_operator.actions.emplace_back(action);
    }
    g_operator.wake.notify_one();
}

} // namespace

bool ObsOperatorStart(HINSTANCE instance, unsigned short bridgePort) {
    if (g_operator.running.load()) return false;
    g_operator.port = bridgePort;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = instance;
    wc.lpszClassName = kPanelClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    MonitorChoice choice{};
    EnumDisplayMonitors(nullptr, nullptr, PickMonitor, (LPARAM)&choice);
    if (choice.work.right <= choice.work.left)
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &choice.work, 0);
    const int left = choice.work.right - kWidth - 22;
    const int top = choice.work.top + 22;
    g_operator.screenRect = {left, top, left + kWidth, top + kHeight};

    // Layered + transparent means seat 0 can still click the real app under
    // the panel. Only the raw-input seat's virtual pointer activates buttons.
    g_operator.window = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED |
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kPanelClass, L"OBS Operator", WS_POPUP,
        left, top, kWidth, kHeight, nullptr, nullptr, instance, nullptr);
    if (!g_operator.window) return false;

    SetLayeredWindowAttributes(g_operator.window, 0, 238, LWA_ALPHA);
    ShowWindow(g_operator.window, SW_SHOWNOACTIVATE);
    g_operator.connected = false;
    g_operator.running = true;
    g_operator.worker = std::thread(WorkLoop);
    LogLine(L"[obs-operator] focus-safe HUD at %d,%d, localhost port %u",
            left, top, (unsigned)bridgePort);
    return true;
}

void ObsOperatorMouse(POINT screenPoint, USHORT flags) {
    if (!(flags & RI_MOUSE_LEFT_BUTTON_DOWN) || !g_operator.running.load()) return;
    if (!PtInRect(&g_operator.screenRect, screenPoint)) return;
    const int x = screenPoint.x - g_operator.screenRect.left;
    const int y = screenPoint.y - g_operator.screenRect.top;
    if (x < kButtonX || x >= kButtonX + kButtonW) return;
    for (int index = 0; index < 3; ++index) {
        if (y >= kButtonY[index] && y < kButtonY[index] + kButtonH) {
            EnqueueAction(kActions[index]);
            return;
        }
    }
}

void ObsOperatorStop() {
    if (!g_operator.running.exchange(false)) return;
    g_operator.wake.notify_all();
    if (g_operator.worker.joinable()) g_operator.worker.join();
    if (g_operator.window) {
        DestroyWindow(g_operator.window);
        g_operator.window = nullptr;
    }
}

} // namespace om

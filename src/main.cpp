#include <windows.h>
#include <shellscalingapi.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <climits>

// --- Data structures ---

struct MonitorInfo {
    HMONITOR handle;
    RECT     rc;
    bool     primary;
    WCHAR    device[CCHDEVICENAME];
};

enum class Edge { None, Left, Right, Top, Bottom };

// --- Global state (main thread only, no locking needed) ---

static std::vector<MonitorInfo> g_monitors;
static HMONITOR  g_lastMonitor = nullptr;
static POINT     g_lastPos     = {LONG_MIN, LONG_MIN};
static bool      g_suppressing = false;
static DWORD     g_mainThreadId = 0;
static HWND      g_hwnd = nullptr;
static HHOOK     g_hook = nullptr;

static constexpr UINT_PTR TIMER_TOPO_CHECK = 1;
static constexpr UINT     TOPO_INTERVAL_MS = 30000;

// --- Topology signature for change detection ---

static std::string BuildTopoSignature(std::vector<MonitorInfo>& mons) {
    std::sort(mons.begin(), mons.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        int cmp = wmemcmp(a.device, b.device, CCHDEVICENAME);
        if (cmp != 0) return cmp < 0;
        if (a.rc.left != b.rc.left) return a.rc.left < b.rc.left;
        return a.rc.top < b.rc.top;
    });
    std::string sig;
    for (auto& m : mons) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%ld,%ld,%ld,%ld,%d,",
                 m.rc.left, m.rc.top, m.rc.right, m.rc.bottom, m.primary);
        sig += buf;
        // Append device name (narrow)
        for (int i = 0; i < CCHDEVICENAME && m.device[i]; ++i)
            sig += static_cast<char>(m.device[i]);
        sig += ';';
    }
    return sig;
}

static std::string g_topoSignature;

// --- Monitor enumeration ---

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(lParam);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        MonitorInfo info{};
        info.handle  = hMon;
        info.rc      = mi.rcMonitor;
        info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        wmemcpy(info.device, mi.szDevice, CCHDEVICENAME);
        out->push_back(info);
    }
    return TRUE;
}

static void RefreshMonitors() {
    std::vector<MonitorInfo> fresh;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc,
                        reinterpret_cast<LPARAM>(&fresh));
    auto sig = BuildTopoSignature(fresh);
    if (sig == g_topoSignature) return; // no change

    g_monitors      = std::move(fresh);
    g_topoSignature = std::move(sig);
    g_lastMonitor   = nullptr;
    g_lastPos        = {LONG_MIN, LONG_MIN};
    printf("Monitors refreshed (%zu detected)\n", g_monitors.size());
}

// --- Edge detection via line-segment / rect intersection ---
// RECT is half-open [left, right) / [top, bottom) for pixel containment,
// but intersection tests use closed intervals to capture corner exits.

struct HitResult {
    Edge   edge;
    double t;       // parameter along segment [0,1]
    double coord;   // intersection coordinate along the edge
};

static HitResult FindExitEdge(POINT p0, POINT p1, const RECT& rc) {
    double dx = static_cast<double>(p1.x) - p0.x;
    double dy = static_cast<double>(p1.y) - p0.y;

    HitResult best{Edge::None, 2.0, 0.0};

    auto tryEdge = [&](Edge e, double t, double along) {
        if (t < -1e-9 || t > 1.0) return;
        // t≈0: p0 is on the edge, only accept if moving outward
        if (t < 1e-9) {
            bool outward = (e == Edge::Left && dx < 0) ||
                           (e == Edge::Right && dx > 0) ||
                           (e == Edge::Top && dy < 0) ||
                           (e == Edge::Bottom && dy > 0);
            if (!outward) return;
        }
        if (t < best.t - 1e-9) {
            best = {e, t, along};
        } else if (std::abs(t - best.t) < 1e-9) {
            bool horiz = (e == Edge::Left || e == Edge::Right);
            if (horiz && std::abs(dx) >= std::abs(dy)) best = {e, t, along};
            if (!horiz && std::abs(dy) > std::abs(dx)) best = {e, t, along};
        }
    };

    // Right edge: x = rc.right
    if (dx != 0.0) {
        double t = (rc.right - p0.x) / dx;
        double y = p0.y + t * dy;
        if (y >= rc.top && y <= rc.bottom)
            tryEdge(Edge::Right, t, y);
    }
    // Left edge: x = rc.left
    if (dx != 0.0) {
        double t = (rc.left - p0.x) / dx;
        double y = p0.y + t * dy;
        if (y >= rc.top && y <= rc.bottom)
            tryEdge(Edge::Left, t, y);
    }
    // Bottom edge: y = rc.bottom
    if (dy != 0.0) {
        double t = (rc.bottom - p0.y) / dy;
        double x = p0.x + t * dx;
        if (x >= rc.left && x <= rc.right)
            tryEdge(Edge::Bottom, t, x);
    }
    // Top edge: y = rc.top
    if (dy != 0.0) {
        double t = (rc.top - p0.y) / dy;
        double x = p0.x + t * dx;
        if (x >= rc.left && x <= rc.right)
            tryEdge(Edge::Top, t, x);
    }

    return best;
}

// --- Percentage mapping with shared-edge overlap ---

static const MonitorInfo* FindMonitor(HMONITOR h) {
    for (auto& m : g_monitors)
        if (m.handle == h) return &m;
    return nullptr;
}

static bool RemapCursor(const RECT& src, const RECT& dst, Edge edge, double hitCoord, POINT& out) {
    // Overlap: only used to verify monitors are adjacent
    // Percentage is based on source full edge, mapped to destination full edge
    long ovStart, ovEnd, srcStart, srcEnd, dstStart, dstEnd;

    if (edge == Edge::Left || edge == Edge::Right) {
        ovStart  = std::max(src.top, dst.top);
        ovEnd    = std::min(src.bottom, dst.bottom);
        srcStart = src.top;  srcEnd = src.bottom;
        dstStart = dst.top;  dstEnd = dst.bottom;
    } else {
        ovStart  = std::max(src.left, dst.left);
        ovEnd    = std::min(src.right, dst.right);
        srcStart = src.left;  srcEnd = src.right;
        dstStart = dst.left;  dstEnd = dst.right;
    }

    long srcLen = srcEnd - srcStart;
    long dstLen = dstEnd - dstStart;
    if (ovEnd - ovStart <= 0 || srcLen <= 0 || dstLen <= 0) return false;

    // Percentage along source full edge
    double pct = (hitCoord - srcStart) / static_cast<double>(srcLen);
    pct = std::clamp(pct, 0.0, 1.0);

    // Map to destination full edge, then round, then inset 1px
    long mapped = dstStart + static_cast<long>(std::lround(pct * dstLen));
    mapped = std::clamp(mapped, dstStart + 1, dstEnd - 2);

    // Build output point
    switch (edge) {
    case Edge::Right:  out = {dst.left + 1, mapped};     break;
    case Edge::Left:   out = {dst.right - 2, mapped};    break;
    case Edge::Bottom: out = {mapped, dst.top + 1};      break;
    case Edge::Top:    out = {mapped, dst.bottom - 2};   break;
    default: return false;
    }
    return true;
}

// --- Low-level mouse hook ---

static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_MOUSEMOVE) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        // Skip injected events (primary anti-recursion guard)
        if (ms->flags & LLMHF_INJECTED) {
            printf("[DBG] skip injected pt=(%ld,%ld) flags=0x%lx\n", ms->pt.x, ms->pt.y, ms->flags);
            return CallNextHookEx(g_hook, nCode, wParam, lParam);
        }
        // Skip if we just called SetCursorPos (secondary guard)
        if (g_suppressing) {
            return CallNextHookEx(g_hook, nCode, wParam, lParam);
        }

        POINT pt = ms->pt;
        HMONITOR curMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
        if (!curMon) { printf("[DBG] MonitorFromPoint returned NULL for (%ld,%ld)\n", pt.x, pt.y); goto done; }

        if (g_lastMonitor && curMon != g_lastMonitor &&
            g_lastPos.x != LONG_MIN)
        {
            printf("[DBG] CROSS detected: lastMon=%p curMon=%p lastPos=(%ld,%ld) pt=(%ld,%ld)\n",
                   (void*)g_lastMonitor, (void*)curMon, g_lastPos.x, g_lastPos.y, pt.x, pt.y);
            const MonitorInfo* src = FindMonitor(g_lastMonitor);
            const MonitorInfo* dst = FindMonitor(curMon);

            if (!src || !dst) {
                printf("[DBG] FindMonitor failed: src=%p dst=%p\n", (const void*)src, (const void*)dst);
            } else {
                printf("[DBG] src rc=(%ld,%ld,%ld,%ld) dst rc=(%ld,%ld,%ld,%ld)\n",
                       src->rc.left, src->rc.top, src->rc.right, src->rc.bottom,
                       dst->rc.left, dst->rc.top, dst->rc.right, dst->rc.bottom);
                HitResult hit = FindExitEdge(g_lastPos, pt, src->rc);
                printf("[DBG] FindExitEdge: edge=%d t=%.6f coord=%.1f\n",
                       (int)hit.edge, hit.t, hit.coord);
                if (hit.edge != Edge::None) {
                    POINT mapped;
                    if (RemapCursor(src->rc, dst->rc, hit.edge,
                                    hit.coord, mapped))
                    {
                        printf("[DBG] RemapCursor: mapped=(%ld,%ld) cur=(%ld,%ld)\n",
                               mapped.x, mapped.y, pt.x, pt.y);
                        if (mapped.x != pt.x || mapped.y != pt.y) {
                            g_suppressing = true;
                            BOOL ok = SetCursorPos(mapped.x, mapped.y);
                            g_suppressing = false;
                            printf("[DBG] SetCursorPos(%ld,%ld) => %d\n", mapped.x, mapped.y, ok);
                            if (ok) {
                                g_lastMonitor = MonitorFromPoint(
                                    mapped, MONITOR_DEFAULTTONULL);
                                g_lastPos = mapped;
                                return 1; // suppress original event
                            }
                        }
                    }
                }
            }
        }
    done:
        if (wParam == WM_MOUSEMOVE) {
            auto* ms2 = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            HMONITOR cur = MonitorFromPoint(ms2->pt, MONITOR_DEFAULTTONULL);
            if (cur) {
                g_lastMonitor = cur;
                g_lastPos = ms2->pt;
            }
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// --- Console Ctrl handler (runs on a separate thread) ---

static BOOL WINAPI ConsoleCtrlHandler(DWORD) {
    if (!PostThreadMessage(g_mainThreadId, WM_QUIT, 0, 0)) {
        if (g_hwnd) PostMessage(g_hwnd, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

// --- Window procedure ---

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        RefreshMonitors();
        return 0;
    case WM_TIMER:
        if (wParam == TIMER_TOPO_CHECK) RefreshMonitors();
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// --- Entry point ---

int main() {
    // DPI awareness (non-fatal fallback for manifest)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_mainThreadId = GetCurrentThreadId();
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    RefreshMonitors();
    if (g_monitors.empty()) {
        printf("No monitors detected.\n");
        return 1;
    }

    // Hidden top-level window for WM_DISPLAYCHANGE / WM_SETTINGCHANGE
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = GetModuleHandle(nullptr);
    wc.lpszClassName  = L"CursorMapperHidden";
    if (!RegisterClassW(&wc)) {
        printf("Failed to register window class: %lu\n", GetLastError());
        return 1;
    }

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, nullptr,
                             WS_POPUP, 0, 0, 0, 0,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        printf("Failed to create hidden window: %lu\n", GetLastError());
        return 1;
    }

    if (!SetTimer(g_hwnd, TIMER_TOPO_CHECK, TOPO_INTERVAL_MS, nullptr)) {
        printf("Failed to create topology check timer: %lu\n", GetLastError());
        DestroyWindow(g_hwnd);
        return 1;
    }

    // Install low-level mouse hook
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc,
                               GetModuleHandle(nullptr), 0);
    if (!g_hook) {
        printf("Failed to install mouse hook: %lu\n", GetLastError());
        DestroyWindow(g_hwnd);
        return 1;
    }

    printf("cursor_mapper running. Press Ctrl+C to exit.\n");

    // Message loop (required for WH_MOUSE_LL dispatch)
    MSG msg;
    BOOL ret;
    while ((ret = GetMessage(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) {
            printf("GetMessage error: %lu\n", GetLastError());
            break;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(g_hook);
    KillTimer(g_hwnd, TIMER_TOPO_CHECK);
    DestroyWindow(g_hwnd);
    printf("cursor_mapper stopped.\n");
    return 0;
}

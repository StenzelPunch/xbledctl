#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "gui_theme.h"

#include <d3d11.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dbt.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "shlwapi.lib")

extern "C" {
#include "xbox_led.h"
}

static ID3D11Device           *g_pd3dDevice          = nullptr;
static ID3D11DeviceContext    *g_pd3dDeviceContext    = nullptr;
static IDXGISwapChain         *g_pSwapChain          = nullptr;
static bool                    g_SwapChainOccluded   = false;
static UINT                    g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;

static bool CreateDeviceD3D(HWND hWnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static char g_config_path[MAX_PATH] = {};
static const int MAX_CONTROLLERS = 8;
static const int MAX_PROFILES = 32;

struct ControllerProfile {
    uint64_t device_id;
    int brightness;
    int mode_idx;
};

static ControllerProfile g_profiles[MAX_PROFILES] = {};
static int g_profile_count = 0;
static uint64_t g_config_selected_device_id = 0;
static int g_default_brightness = LED_BRIGHTNESS_DEFAULT;
static int g_default_mode_idx = 1;

static int ClampBrightness(int value)
{
    if (value < LED_BRIGHTNESS_MIN) return LED_BRIGHTNESS_MIN;
    if (value > LED_BRIGHTNESS_MAX) return LED_BRIGHTNESS_MAX;
    return value;
}

static int ClampModeIdx(int value)
{
    if (value < 0 || value > 7) return 1;
    return value;
}

static int FindProfileIndex(uint64_t device_id)
{
    for (int i = 0; i < g_profile_count; i++) {
        if (g_profiles[i].device_id == device_id)
            return i;
    }
    return -1;
}

static void SetProfileValue(uint64_t device_id, int brightness, int mode_idx)
{
    if (device_id == 0)
        return;

    int idx = FindProfileIndex(device_id);
    if (idx < 0) {
        if (g_profile_count < MAX_PROFILES) {
            idx = g_profile_count++;
        } else {
            idx = MAX_PROFILES - 1;
        }
        g_profiles[idx].device_id = device_id;
    }

    g_profiles[idx].brightness = ClampBrightness(brightness);
    g_profiles[idx].mode_idx = ClampModeIdx(mode_idx);
}

static bool GetProfileValue(uint64_t device_id, int *brightness, int *mode_idx)
{
    int idx = FindProfileIndex(device_id);
    if (idx < 0)
        return false;

    if (brightness)
        *brightness = g_profiles[idx].brightness;
    if (mode_idx)
        *mode_idx = g_profiles[idx].mode_idx;
    return true;
}

static void InitConfigPath()
{
    GetModuleFileNameA(nullptr, g_config_path, MAX_PATH);
    PathRemoveFileSpecA(g_config_path);
    strcat_s(g_config_path, "\\xbledctl.ini");
}

static void SaveConfig(int brightness, int mode_idx, bool start_with_windows, bool minimize_to_tray, uint64_t selected_device_id)
{
    FILE *f = nullptr;
    fopen_s(&f, g_config_path, "w");
    if (!f)
        return;

    fprintf(f, "[xbledctl]\n");
    fprintf(f, "brightness=%d\n", ClampBrightness(brightness));
    fprintf(f, "mode=%d\n", ClampModeIdx(mode_idx));
    fprintf(f, "start_with_windows=%d\n", start_with_windows ? 1 : 0);
    fprintf(f, "minimize_to_tray=%d\n", minimize_to_tray ? 1 : 0);
    fprintf(f, "selected_controller_id=%016llX\n", (unsigned long long)selected_device_id);

    for (int i = 0; i < g_profile_count; i++) {
        fprintf(f, "profile=%016llX,%d,%d\n",
                (unsigned long long)g_profiles[i].device_id,
                ClampBrightness(g_profiles[i].brightness),
                ClampModeIdx(g_profiles[i].mode_idx));
    }

    fclose(f);
}

static void LoadConfig(int *brightness, int *mode_idx, bool *start_with_windows, bool *minimize_to_tray, uint64_t *selected_device_id)
{
    *brightness = LED_BRIGHTNESS_DEFAULT;
    *mode_idx = 1;
    *start_with_windows = true;
    *minimize_to_tray = true;
    if (selected_device_id)
        *selected_device_id = 0;
    g_profile_count = 0;

    FILE *f = nullptr;
    fopen_s(&f, g_config_path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int val;
        unsigned long long id = 0;
        int p_brightness = 0;
        int p_mode_idx = 0;
        if (sscanf_s(line, "brightness=%d", &val) == 1)
            *brightness = ClampBrightness(val);
        else if (sscanf_s(line, "mode=%d", &val) == 1)
            *mode_idx = ClampModeIdx(val);
        else if (sscanf_s(line, "start_with_windows=%d", &val) == 1)
            *start_with_windows = (val != 0);
        else if (sscanf_s(line, "minimize_to_tray=%d", &val) == 1)
            *minimize_to_tray = (val != 0);
        else if (sscanf_s(line, "selected_controller_id=%llx", &id) == 1) {
            if (selected_device_id)
                *selected_device_id = (uint64_t)id;
        } else if (sscanf_s(line, "profile=%llx,%d,%d", &id, &p_brightness, &p_mode_idx) == 3) {
            SetProfileValue((uint64_t)id, p_brightness, p_mode_idx);
        }
    }
    fclose(f);
}

static const char *AUTOSTART_KEY = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char *AUTOSTART_VAL = "xbledctl";

static void SetAutoStart(bool enable)
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, AUTOSTART_KEY, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;

    if (enable) {
        char exe_path[MAX_PATH];
        GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        char cmd[MAX_PATH + 32];
        snprintf(cmd, sizeof(cmd), "\"%s\" --minimized", exe_path);
        RegSetValueExA(hKey, AUTOSTART_VAL, 0, REG_SZ, (BYTE *)cmd, (DWORD)strlen(cmd) + 1);
    } else {
        RegDeleteValueA(hKey, AUTOSTART_VAL);
    }
    RegCloseKey(hKey);
}

static bool IsAutoStartEnabled()
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, AUTOSTART_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD type, size = 0;
    bool exists = RegQueryValueExA(hKey, AUTOSTART_VAL, nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
    RegCloseKey(hKey);
    return exists;
}

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_SHOW 1001
#define ID_TRAY_QUIT 1002

static NOTIFYICONDATAW g_nid = {};
static HWND g_hwnd = nullptr;
static bool g_minimized_to_tray = false;
static int  g_redraw_frames = 3;

static void AddTrayIcon(HWND hwnd)
{
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Xbox LED Control");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void MinimizeToTray(HWND hwnd)
{
    ShowWindow(hwnd, SW_HIDE);
    g_minimized_to_tray = true;
}

static void RestoreFromTray(HWND hwnd)
{
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    g_minimized_to_tray = false;
    g_redraw_frames = 3;
}

static XboxController g_ctrl;
static uint64_t       g_device_ids[MAX_CONTROLLERS] = {};
static int            g_device_count = 0;
static int            g_selected_device_idx = 0;
static uint64_t       g_selected_device_id = 0;
static int            g_brightness = LED_BRIGHTNESS_DEFAULT;
static int            g_mode_idx   = 1;
static char           g_status[128] = "Plug in your controller with a USB cable";
static ImVec4         g_status_color;
static bool           g_start_with_windows = true;
static bool           g_minimize_to_tray = true;
static bool           g_device_change_pending = false;
static DWORD          g_device_change_tick = 0;
static bool           g_device_removed = false;
static bool           g_controller_present = false;

enum WorkerCmd { CMD_NONE, CMD_REFRESH, CMD_REFRESH_APPLY, CMD_APPLY };
static HANDLE         g_worker_thread = nullptr;
static HANDLE         g_worker_event = nullptr;
static volatile WorkerCmd g_worker_cmd = CMD_NONE;
static volatile bool  g_worker_busy = false;
static volatile int   g_worker_brightness = 0;
static volatile int   g_worker_mode = 0;
static volatile unsigned long long g_worker_device_id = 0;

static const ImVec4 COL_WARN    = ImVec4(0.902f, 0.706f, 0.157f, 1.0f);

struct ModeEntry {
    const char *label;
    uint8_t     value;
};

static const ModeEntry MODES[] = {
    { "Off",        LED_MODE_OFF           },
    { "Steady",     LED_MODE_ON            },
    { "Fast Blink", LED_MODE_BLINK_FAST    },
    { "Slow Blink", LED_MODE_BLINK_SLOW    },
    { "Charging",   LED_MODE_BLINK_CHARGE  },
    { "Fade Slow",  LED_MODE_FADE_SLOW     },
    { "Fade Fast",  LED_MODE_FADE_FAST     },
    { "Fade In",    LED_MODE_RAMP_TO_LEVEL },
};
static const int MODE_COUNT = sizeof(MODES) / sizeof(MODES[0]);

static const ImVec4 COL_SUCCESS  = ImVec4(0.157f, 0.784f, 0.314f, 1.0f);
static const ImVec4 COL_ERROR    = ImVec4(0.863f, 0.235f, 0.235f, 1.0f);
static const ImVec4 COL_DIM      = ImVec4(0.549f, 0.549f, 0.588f, 1.0f);
static const ImVec4 COL_TEXT     = ImVec4(0.902f, 0.902f, 0.922f, 1.0f);
static const ImVec4 COL_ACCENT   = ImVec4(0.063f, 0.486f, 0.063f, 1.0f);
static const ImVec4 COL_ACCENT_H = ImVec4(0.078f, 0.627f, 0.078f, 1.0f);
static const ImVec4 COL_ACCENT_A = ImVec4(0.047f, 0.392f, 0.047f, 1.0f);

static int FindDeviceIndex(uint64_t device_id)
{
    for (int i = 0; i < g_device_count; i++) {
        if (g_device_ids[i] == device_id)
            return i;
    }
    return -1;
}

static void LoadSelectedProfile()
{
    int brightness = g_default_brightness;
    int mode_idx = g_default_mode_idx;

    if (g_selected_device_id != 0)
        GetProfileValue(g_selected_device_id, &brightness, &mode_idx);

    g_brightness = ClampBrightness(brightness);
    g_mode_idx = ClampModeIdx(mode_idx);
}

static void SetDiscoveredDevices(const uint64_t *device_ids, int count)
{
    if (count < 0)
        count = 0;
    if (count > MAX_CONTROLLERS)
        count = MAX_CONTROLLERS;

    g_device_count = count;
    for (int i = 0; i < count; i++)
        g_device_ids[i] = device_ids[i];
    for (int i = count; i < MAX_CONTROLLERS; i++)
        g_device_ids[i] = 0;

    if (count == 0) {
        g_selected_device_idx = 0;
        g_selected_device_id = 0;
        g_controller_present = false;
        return;
    }

    int idx = -1;
    if (g_selected_device_id != 0)
        idx = FindDeviceIndex(g_selected_device_id);
    if (idx < 0 && g_config_selected_device_id != 0)
        idx = FindDeviceIndex(g_config_selected_device_id);
    if (idx < 0)
        idx = 0;

    g_selected_device_idx = idx;
    g_selected_device_id = g_device_ids[idx];
    g_config_selected_device_id = g_selected_device_id;
    g_controller_present = true;
    LoadSelectedProfile();
}

static bool RefreshControllerList()
{
    uint64_t ids[MAX_CONTROLLERS] = {};
    int count = 0;
    if (!xbox_enumerate_devices(ids, MAX_CONTROLLERS, &count) || count <= 0) {
        SetDiscoveredDevices(ids, 0);
        return false;
    }

    SetDiscoveredDevices(ids, count);
    return true;
}

static void SetStatus(const char *msg, const ImVec4 &col)
{
    snprintf(g_status, sizeof(g_status), "%s", msg);
    g_status_color = col;
    g_redraw_frames = 3;
}

static void PostWorkerCmd(WorkerCmd cmd)
{
    if (g_worker_busy)
        return;
    if (cmd == CMD_APPLY) {
        g_worker_brightness = ClampBrightness(g_brightness);
        g_worker_mode = ClampModeIdx(g_mode_idx);
        g_worker_device_id = (unsigned long long)g_selected_device_id;
    }
    g_worker_cmd = cmd;
    g_worker_busy = true;
    SetEvent(g_worker_event);
}

static DWORD WINAPI WorkerThread(LPVOID /*unused*/)
{
    while (WaitForSingleObject(g_worker_event, INFINITE) == WAIT_OBJECT_0) {
        WorkerCmd cmd = g_worker_cmd;
        g_worker_cmd = CMD_NONE;

        if (cmd == CMD_REFRESH || cmd == CMD_REFRESH_APPLY) {
            if (RefreshControllerList()) {
                SetStatus("Ready - drag the slider or pick a mode", COL_SUCCESS);
                if (cmd == CMD_REFRESH_APPLY) {
                    int applied = 0;
                    int failed = 0;

                    for (int i = 0; i < g_device_count; i++) {
                        uint64_t device_id = g_device_ids[i];
                        int mode_idx = g_default_mode_idx;
                        int bright = g_default_brightness;
                        GetProfileValue(device_id, &bright, &mode_idx);

                        mode_idx = ClampModeIdx(mode_idx);
                        bright = ClampBrightness(bright);
                        if (mode_idx == 0)
                            bright = 0;

                        if (!xbox_open_device(&g_ctrl, device_id)) {
                            failed++;
                            continue;
                        }

                        bool ok = xbox_set_led(&g_ctrl, (uint8_t)MODES[mode_idx].value, (uint8_t)bright);
                        xbox_close(&g_ctrl);

                        if (ok) {
                            applied++;
                        } else {
                            failed++;
                        }
                    }

                    if (applied > 0) {
                        if (failed == 0)
                            SetStatus("Controllers detected - settings applied", COL_SUCCESS);
                        else
                            SetStatus("Some controllers applied - try Refresh", COL_WARN);
                    } else {
                        SetStatus("Command failed - try Refresh to reconnect", COL_ERROR);
                    }
                }
            } else {
                SetStatus("Plug in your controller with a USB cable", COL_DIM);
            }
        } else if (cmd == CMD_APPLY) {
            int mode_idx = ClampModeIdx(g_worker_mode);
            int mode_val = MODES[mode_idx].value;
            int bright = ClampBrightness(g_worker_brightness);
            uint64_t device_id = (uint64_t)g_worker_device_id;
            if (mode_idx == 0) bright = 0;

            if (device_id == 0) {
                SetStatus("No controller selected - click Refresh", COL_ERROR);
            } else if (!xbox_open_device(&g_ctrl, device_id)) {
                RefreshControllerList();
                if (g_controller_present)
                    SetStatus("Selected controller unavailable - choose another", COL_WARN);
                else
                    SetStatus("Cannot open controller - try Refresh", COL_ERROR);
            } else {
                bool ok = xbox_set_led(&g_ctrl, (uint8_t)mode_val, (uint8_t)bright);
                xbox_close(&g_ctrl);
                if (ok) {
                    SetProfileValue(device_id, bright, mode_idx);
                    g_selected_device_id = device_id;
                    g_config_selected_device_id = device_id;
                    if (bright == 0 || mode_idx == 0) {
                        SetStatus("LED turned off", COL_SUCCESS);
                    } else {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "LED: %s at brightness %d/%d",
                                 MODES[mode_idx].label, bright, LED_BRIGHTNESS_MAX);
                        SetStatus(buf, COL_SUCCESS);
                    }
                    SaveConfig(g_brightness, g_mode_idx, g_start_with_windows, g_minimize_to_tray, g_selected_device_id);
                } else {
                    SetStatus("Command failed - try Refresh to reconnect", COL_ERROR);
                }
            }
        }
        g_worker_busy = false;
    }
    return 0;
}

static void ApplyLed()
{
    SetStatus("Sending command...", COL_DIM);
    PostWorkerCmd(CMD_APPLY);
}

static void RefreshController()
{
    SetStatus("Searching for controller...", COL_DIM);
    PostWorkerCmd(CMD_REFRESH);
}

static void TryAutoApply()
{
    SetStatus("Controller detected - applying settings...", COL_DIM);
    PostWorkerCmd(CMD_REFRESH_APPLY);
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
    case WM_PAINT:
        g_redraw_frames = 3;
        break;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            if (g_minimize_to_tray) {
                MinimizeToTray(hWnd);
                return 0;
            }
        }
        g_ResizeWidth  = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        if ((wParam & 0xfff0) == SC_CLOSE && g_minimize_to_tray) {
            MinimizeToTray(hWnd);
            return 0;
        }
        break;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK) {
            RestoreFromTray(hWnd);
        } else if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"Show");
            AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");
            SetForegroundWindow(hWnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(menu);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_SHOW) RestoreFromTray(hWnd);
        if (LOWORD(wParam) == ID_TRAY_QUIT) {
            RemoveTrayIcon();
            DestroyWindow(hWnd);
        }
        return 0;

    case WM_DEVICECHANGE: {
        if (wParam == DBT_DEVICEARRIVAL) {
            g_device_change_pending = true;
            g_device_change_tick = GetTickCount();
        }
        if (wParam == DBT_DEVNODES_CHANGED) {
            if (!g_device_change_pending) {
                g_device_change_pending = true;
                g_device_change_tick = GetTickCount();
            }
        }
        if (wParam == DBT_DEVICEREMOVECOMPLETE) {
            g_device_removed = true;
        }
        return 0;
    }

    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void RenderGui(ImFont *fontTitle, ImFont *fontSub, ImFont *fontBig)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::PushFont(fontTitle);
    ImGui::Text("Xbox LED Control");
    ImGui::PopFont();
    ImGui::Spacing();

    ImGui::BeginChild("##ctrl_card", ImVec2(-1, 55), ImGuiChildFlags_Borders);
    {
        ImGui::PushFont(fontSub);
        ImGui::TextColored(COL_DIM, "CONTROLLER");
        ImGui::SameLine(0, 10);
        if (g_controller_present)
            ImGui::TextColored(COL_SUCCESS, "  CONNECTED");
        else
            ImGui::TextColored(COL_ERROR, "  DISCONNECTED");
        ImGui::PopFont();

        ImGui::Spacing();
        if (!g_controller_present) {
            ImGui::TextColored(COL_DIM, "Connect an Xbox controller via USB");
        }
    }
    ImGui::EndChild();
    ImGui::Spacing();

    ImGui::BeginChild("##controller_select_card", ImVec2(-1, 70), ImGuiChildFlags_Borders);
    {
        ImGui::PushFont(fontSub);
        ImGui::TextColored(COL_DIM, "SELECT CONTROLLER");
        ImGui::PopFont();

        if (g_device_count > 0) {
            const char *items[MAX_CONTROLLERS] = {};
            char labels[MAX_CONTROLLERS][96] = {};
            for (int i = 0; i < g_device_count; i++) {
                unsigned long long device_id = (unsigned long long)g_device_ids[i];
                snprintf(labels[i], sizeof(labels[i]), "Controller %d (%016llX)", i + 1, device_id);
                items[i] = labels[i];
            }

            int selected = g_selected_device_idx;
            if (selected < 0 || selected >= g_device_count)
                selected = 0;
            ImGui::SetNextItemWidth(-1);
            ImGui::BeginDisabled(g_worker_busy || g_device_count <= 1);
            if (ImGui::Combo("##controller_select", &selected, items, g_device_count)) {
                if (selected >= 0 && selected < g_device_count) {
                    g_selected_device_idx = selected;
                    g_selected_device_id = g_device_ids[selected];
                    g_config_selected_device_id = g_selected_device_id;
                    LoadSelectedProfile();
                    SaveConfig(g_brightness, g_mode_idx, g_start_with_windows, g_minimize_to_tray, g_selected_device_id);
                    SetStatus("Controller selected", COL_DIM);
                }
            }
            ImGui::EndDisabled();
        } else {
            ImGui::TextColored(COL_DIM, "No controllers discovered");
        }
    }
    ImGui::EndChild();
    ImGui::Spacing();

    ImGui::BeginChild("##bright_card", ImVec2(-1, 120), ImGuiChildFlags_Borders);
    {
        ImGui::PushFont(fontSub);
        ImGui::TextColored(COL_DIM, "BRIGHTNESS");
        ImGui::PopFont();

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
        float pct = (float)g_brightness / LED_BRIGHTNESS_MAX;
        ImVec4 numCol = ImVec4(
            0.063f + 0.094f * pct,
            0.486f + (0.863f - 0.486f) * pct,
            0.063f + 0.094f * pct,
            1.0f
        );
        ImGui::PushFont(fontBig);
        ImGui::TextColored(numCol, "%d", g_brightness);
        ImGui::PopFont();

        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##brightness", &g_brightness, 0, LED_BRIGHTNESS_MAX, "", ImGuiSliderFlags_None)) {
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ApplyLed();
        }

        ImGui::TextColored(COL_DIM, "0");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
        ImGui::TextColored(COL_DIM, "%d", LED_BRIGHTNESS_MAX);
    }
    ImGui::EndChild();
    ImGui::Spacing();

    ImGui::BeginChild("##mode_card", ImVec2(-1, 80), ImGuiChildFlags_Borders);
    {
        ImGui::PushFont(fontSub);
        ImGui::TextColored(COL_DIM, "LED MODE");
        ImGui::PopFont();
        ImGui::Spacing();

        for (int i = 0; i < MODE_COUNT; i++) {
            if (i > 0) ImGui::SameLine();

            bool is_active = (i == g_mode_idx);
            if (is_active) {
                ImGui::PushStyleColor(ImGuiCol_Button,        COL_ACCENT);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  COL_ACCENT_H);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   COL_ACCENT_A);
                ImGui::PushStyleColor(ImGuiCol_Text,           ImVec4(1,1,1,1));
            }

            if (ImGui::Button(MODES[i].label)) {
                g_mode_idx = i;
                ApplyLed();
            }

            if (is_active)
                ImGui::PopStyleColor(4);
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();

    bool busy = g_worker_busy;
    ImGui::BeginDisabled(busy);

    ImGui::PushStyleColor(ImGuiCol_Button,       COL_ACCENT);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_ACCENT_H);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  COL_ACCENT_A);
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1,1,1,1));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(24, 12));
    if (ImGui::Button(busy ? "Applying..." : "Apply"))
        ApplyLed();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);

    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button,       ImVec4(0.157f, 0.157f, 0.216f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.216f, 0.216f, 0.275f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.255f, 0.255f, 0.314f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 12));
    if (ImGui::Button("Refresh"))
        RefreshController();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextColored(g_status_color, "%s", g_status);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Checkbox("Start with Windows", &g_start_with_windows)) {
        SetAutoStart(g_start_with_windows);
        SaveConfig(g_brightness, g_mode_idx, g_start_with_windows, g_minimize_to_tray, g_selected_device_id);
    }
    ImGui::SameLine(0, 20);
    if (ImGui::Checkbox("Minimize to tray", &g_minimize_to_tray)) {
        SaveConfig(g_brightness, g_mode_idx, g_start_with_windows, g_minimize_to_tray, g_selected_device_id);
    }

    ImGui::End();
}

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext)  { g_pd3dDeviceContext->Release();  g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)         { g_pd3dDevice->Release();         g_pd3dDevice = nullptr; }
}

static void CreateRenderTarget()
{
    ID3D11Texture2D *pBack;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBack));
    g_pd3dDevice->CreateRenderTargetView(pBack, nullptr, &g_mainRenderTargetView);
    pBack->Release();
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\xbledctl_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"xbledctl", nullptr);
        if (existing) {
            PostMessageW(existing, WM_COMMAND, ID_TRAY_SHOW, 0);
        }
        return 0;
    }

    bool start_minimized = (strstr(lpCmdLine, "--minimized") != nullptr);

    xbox_init(&g_ctrl);
    g_status_color = COL_DIM;
    g_worker_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    g_worker_thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);

    InitConfigPath();
    LoadConfig(&g_brightness, &g_mode_idx, &g_start_with_windows, &g_minimize_to_tray, &g_config_selected_device_id);
    g_default_brightness = g_brightness;
    g_default_mode_idx = g_mode_idx;

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
        nullptr, nullptr, nullptr, nullptr, L"xbledctl", nullptr };
    RegisterClassExW(&wc);

    RECT wr = { 0, 0, 520, 500 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&wr, style, FALSE);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Xbox LED Control",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    DEV_BROADCAST_DEVICEINTERFACE dbdi = {};
    dbdi.dbcc_size = sizeof(dbdi);
    dbdi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    RegisterDeviceNotification(g_hwnd, &dbdi, DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);

    AddTrayIcon(g_hwnd);

    if (start_minimized) {
        MinimizeToTray(g_hwnd);
    } else {
        ShowWindow(g_hwnd, SW_SHOWDEFAULT);
        UpdateWindow(g_hwnd);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ApplyXboxTheme();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ImFont *fontDefault = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    ImFont *fontTitle   = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 28.0f);
    ImFont *fontSub     = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 14.0f);
    ImFont *fontBig     = fontTitle;
    if (!fontDefault) fontDefault = io.Fonts->AddFontDefault();
    if (!fontTitle)   fontTitle   = fontDefault;
    if (!fontSub)     fontSub     = fontDefault;
    fontBig = fontTitle;

    TryAutoApply();

    g_start_with_windows = IsAutoStartEnabled();

    const float clear[4] = { 0.071f, 0.071f, 0.094f, 1.0f };

    bool done = false;
    while (!done) {
        if (g_minimized_to_tray && !g_device_change_pending && !g_device_removed) {
            WaitMessage();
        } else if (g_redraw_frames <= 0 && !g_device_change_pending && !g_device_removed) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }

        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        if (g_device_removed) {
            g_device_removed = false;
            g_device_change_pending = true;
            g_device_change_tick = GetTickCount();
        }

        if (g_device_change_pending
            && (GetTickCount() - g_device_change_tick) >= 1000
            && !g_worker_busy) {
            g_device_change_pending = false;
            TryAutoApply();
        }

        if (g_minimized_to_tray)
            continue;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(100);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
            g_redraw_frames = 3;
        }

        if (g_redraw_frames <= 0)
            continue;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::PushFont(fontDefault);
        RenderGui(fontTitle, fontSub, fontBig);
        ImGui::PopFont();

        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
        g_redraw_frames--;
    }

    TerminateThread(g_worker_thread, 0);
    CloseHandle(g_worker_thread);
    CloseHandle(g_worker_event);
    xbox_cleanup(&g_ctrl);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);

    return 0;
}

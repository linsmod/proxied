#include "pch.h"
#include "proxied.h"
#include "Resource.h"
#include <fstream>
#include <shellapi.h>
#include <tchar.h>
#include <stdio.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#define REG_PATH _T("Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings")
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    //// 初始化通用控件
    //INITCOMMONCONTROLSEX icex;
    //icex.dwSize = sizeof(icex);
    //icex.dwICC = ICC_WIN95_CLASSES;
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
    InitCommonControls();
    Proxied app;
    app.Run();
    return 0;
}
Proxied::Proxied() :
    proxyEnabled_(false),
    autoStart_(false),
    hWnd_(NULL),
    hPopupMenu_(NULL),
    hEvent_(NULL) {
    memset(&nid_, 0, sizeof(nid_));
}

Proxied::~Proxied() {
    if (hPopupMenu_) {
        DestroyMenu(hPopupMenu_);
    }
    if (hWnd_) {
        DestroyWindow(hWnd_);
    }
    if (hEvent_) {
        CloseHandle(hEvent_);
    }
}

void Proxied::Run() {
    // 启用DPI感知
    SetProcessDPIAware();

    // 初始化窗口类
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = _T("ProxiedTrayClass");
    RegisterClass(&wc);

    // 创建隐藏窗口
    hWnd_ = CreateWindow(wc.lpszClassName, _T("Proxied"), 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!hWnd_) {
        return;
    }

    // 设置this指针到窗口附加数据
    SetWindowLongPtr(hWnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // 初始化托盘图标
   
    CheckAutoStart();

    // 获取当前代理设置
    GetProxySettings();
    InitTrayIcon();
    // 创建一个手动重置的事件对象
    hEvent_ = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!hEvent_) {
        return;
    }

    // 创建消息循环线程
    HANDLE hThread = CreateThread(NULL, 0,
        &Proxied::ThreadProc,
        this, 0, NULL);

    if (!hThread) {
        CloseHandle(hEvent_);
        return;
    }

    // 主消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CloseHandle(hThread);
}

void Proxied::InitTrayIcon() {
    // 创建托盘菜单
    hPopupMenu_ = CreatePopupMenu();
    AppendMenu(hPopupMenu_, MF_STRING, 0, _T("Proxied v1.0"));
    AppendMenu(hPopupMenu_, MF_STRING | (proxyEnabled_ ? MF_CHECKED : 0), IDM_ENABLE, _T("启用代理"));
    AppendMenu(hPopupMenu_, MF_STRING | (proxyEnabled_ ? 0 : MF_CHECKED), IDM_DISABLE, _T("禁用代理"));
    AppendMenu(hPopupMenu_, MF_SEPARATOR, 0, NULL);
    //AppendMenu(hPopupMenu_, MF_STRING, IDM_CONFIG, _T("配置代理..."));
    //AppendMenu(hPopupMenu_, MF_SEPARATOR, 0, NULL);
    AppendMenu(hPopupMenu_, MF_STRING | (autoStart_ ? MF_CHECKED : 0), IDM_AUTOSTART, _T("开机自启"));
    AppendMenu(hPopupMenu_, MF_SEPARATOR, 0, NULL);
    
    AppendMenu(hPopupMenu_, MF_STRING, IDM_EXIT, _T("退出"));

    // 初始化托盘图标
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hWnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    nid_.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_SMALL));
    _tcscpy_s(nid_.szTip, sizeof(nid_.szTip) / sizeof(TCHAR),
        proxyEnabled_ ? _T("Proxied - 代理已启用") : _T("Proxied - 代理已禁用"));

    Shell_NotifyIcon(NIM_ADD, &nid_);
}

void Proxied::CheckAutoStart() {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER,
        _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {

        wchar_t path[MAX_PATH];
        DWORD size = sizeof(path);
        autoStart_ = (RegQueryValueEx(hKey, _T("Proxied"), NULL, NULL,
            reinterpret_cast<LPBYTE>(path), &size) == ERROR_SUCCESS);

        RegCloseKey(hKey);
    }
}

void Proxied::SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER,
        _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {

        if (enable) {
            wchar_t path[MAX_PATH];
            GetModuleFileName(NULL, path, MAX_PATH);
            RegSetValueEx(hKey, _T("Proxied"), 0, REG_SZ,
                reinterpret_cast<const BYTE*>(path),
                (wcslen(path) + 1) * sizeof(wchar_t));
        }
        else {
            RegDeleteValue(hKey, _T("Proxied"));
        }

        RegCloseKey(hKey);
        autoStart_ = enable;
    }
}

void Proxied::GetProxySettings() {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type, size = sizeof(DWORD);
        RegQueryValueEx(hKey, _T("ProxyEnable"), NULL, &type,
            reinterpret_cast<LPBYTE>(&proxyEnabled_), &size);

        wchar_t server[256];
        size = sizeof(server);
        if (RegQueryValueEx(hKey, _T("ProxyServer"), NULL, &type,
            reinterpret_cast<LPBYTE>(server), &size) == ERROR_SUCCESS) {
            proxyServer_ = server;
        }

        wchar_t nonProxy[1024];
        size = sizeof(nonProxy);
        if (RegQueryValueEx(hKey, _T("ProxyOverride"), NULL, &type,
            reinterpret_cast<LPBYTE>(nonProxy), &size) == ERROR_SUCCESS) {
            nonProxyHosts_ = nonProxy;
        }

        RegCloseKey(hKey);
    }
}
#include <windows.h>

std::wstring Proxied::GetGradleConfigPath() {
    wchar_t gradleHome[MAX_PATH];
    DWORD size = GetEnvironmentVariable(_T("GRADLE_USER_HOME"), gradleHome, MAX_PATH);
    if (size == 0) {
        // 使用默认路径 ~/.gradle
        wchar_t userProfile[MAX_PATH];
        GetEnvironmentVariable(_T("USERPROFILE"), userProfile, MAX_PATH);
        return std::wstring(userProfile) + _T("\\.gradle\\gradle.properties");
    }
    return std::wstring(gradleHome) + _T("\\gradle.properties");
}

bool Proxied::UpdateGradleConfig(bool enable) {
    std::wstring configPath = GetGradleConfigPath();
    std::wifstream inFile(configPath);
    std::vector<std::wstring> lines;
    bool changed = false;

    // 读取现有配置并过滤所有代理相关设置
    if (inFile) {
        std::wstring line;
        while (std::getline(inFile, line)) {
            if (line.find(_T("systemProp.http.proxyHost=")) != std::wstring::npos ||
                line.find(_T("systemProp.http.proxyPort=")) != std::wstring::npos ||
                line.find(_T("systemProp.https.proxyHost=")) != std::wstring::npos ||
                line.find(_T("systemProp.https.proxyPort=")) != std::wstring::npos ||
                line.find(_T("systemProp.http.nonProxyHosts=")) != std::wstring::npos ||
                line.find(_T("systemProp.https.nonProxyHosts=")) != std::wstring::npos) {
                changed = true;
                continue; // 跳过所有代理相关设置
            }
            lines.push_back(line);
        }
        inFile.close();
    }

    // 添加新配置
    if (enable) {
        size_t colonPos = proxyServer_.find(_T(':'));
        if (colonPos != std::wstring::npos) {
            std::wstring host = proxyServer_.substr(0, colonPos);
            std::wstring port = proxyServer_.substr(colonPos + 1);

            lines.push_back(_T("systemProp.http.proxyHost=") + host);
            lines.push_back(_T("systemProp.http.proxyPort=") + port);
            lines.push_back(_T("systemProp.https.proxyHost=") + host);
            lines.push_back(_T("systemProp.https.proxyPort=") + port);

            if (!nonProxyHosts_.empty()) {
                lines.push_back(_T("systemProp.http.nonProxyHosts=") + nonProxyHosts_);
                lines.push_back(_T("systemProp.https.nonProxyHosts=") + nonProxyHosts_);
            }
            changed = true;
        }
    }

    // 写入文件
    if (changed) {
        std::wofstream outFile(configPath);
        if (outFile) {
            for (const auto& line : lines) {
                outFile << line << std::endl;
            }
            outFile.close();
            return true;
        }
    }
    return false;
}

void Proxied::UpdateUserEnvironmentVariable(const std::wstring& name, const std::wstring* value) {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, _T("Environment"), 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (value) {
            RegSetValueEx(hKey, name.c_str(), 0, REG_SZ, (const BYTE*)value->c_str(), (value->size() + 1) * sizeof(wchar_t));
        }
        else {
            RegDeleteValue(hKey, name.c_str());
        }
        RegCloseKey(hKey);

        // 广播环境变量更改
        SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)_T("Environment"), SMTO_ABORTIFHUNG, 5000, NULL);
    }
    else {
        _tprintf(_T("Failed to open registry key for user environment variables.\n"));
    }
}
DWORD WINAPI Proxied::ThreadProc(LPVOID lpParam) {
    Proxied* pThis = reinterpret_cast<Proxied*>(lpParam);
    pThis->HandleRegistryChanges(pThis->hEvent_);
    return 0;
}

std::wstring Proxied::EnsureProxyPrefix(const std::wstring& proxy) {
    if (proxy.find(L"://") == std::wstring::npos) {
        return L"http://" + proxy;
    }
    return proxy;
}

void Proxied::HandleRegistryChanges(HANDLE hEvent) {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, KEY_NOTIFY, &hKey) != ERROR_SUCCESS) {
        return;
    }

    // 初始读取代理设置
    GetProxySettings();

    if (proxyEnabled_) {
        std::wstring proxyWithPrefix = EnsureProxyPrefix(proxyServer_);
        UpdateUserEnvironmentVariable(_T("http_proxy"), &proxyWithPrefix);
        UpdateUserEnvironmentVariable(_T("https_proxy"), &proxyWithPrefix);
        UpdateGradleConfig(true);
    }
    else {
        UpdateUserEnvironmentVariable(_T("http_proxy"), nullptr);
        UpdateUserEnvironmentVariable(_T("https_proxy"), nullptr);
        UpdateGradleConfig(false);
    }
    while (RegNotifyChangeKeyValue(hKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, hEvent, TRUE) != ERROR_SUCCESS) {
        Sleep(1000);
    }
    while (true) {
        // 等待事件被触发
        if (WaitForSingleObject(hEvent, INFINITE) == WAIT_OBJECT_0) {
            // 重新读取代理设置
            GetProxySettings();

            if (proxyEnabled_) {
                std::wstring proxyWithPrefix = EnsureProxyPrefix(proxyServer_);
                UpdateUserEnvironmentVariable(_T("http_proxy"), &proxyWithPrefix);
                UpdateUserEnvironmentVariable(_T("https_proxy"), &proxyWithPrefix);
                UpdateGradleConfig(true);
            }
            else {
                UpdateUserEnvironmentVariable(_T("http_proxy"), nullptr);
                UpdateUserEnvironmentVariable(_T("https_proxy"), nullptr);
                UpdateGradleConfig(false);
            }

            // 更新托盘提示
            _tcscpy_s(nid_.szTip, sizeof(nid_.szTip) / sizeof(TCHAR),
                proxyEnabled_ ? _T("Proxied - 代理已启用") : _T("Proxied - 代理已禁用"));
            Shell_NotifyIcon(NIM_MODIFY, &nid_);

            // 更新菜单状态
            CheckMenuItem(hPopupMenu_, IDM_ENABLE,
                proxyEnabled_ ? MF_CHECKED : MF_UNCHECKED);
            CheckMenuItem(hPopupMenu_, IDM_DISABLE,
                proxyEnabled_ ? MF_UNCHECKED : MF_CHECKED);

            // 重置事件以继续监听
            if (RegNotifyChangeKeyValue(hKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, hEvent, TRUE) != ERROR_SUCCESS) {
                break;
            }
        }
    }

    RegCloseKey(hKey);
}
#include <windows.h>
#include <tchar.h>

void UpdateUserEnvironmentVariable(const std::wstring& name, const std::wstring* value) {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, _T("Environment"), 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        LONG result;
        if (value) {
            // 设置新的环境变量值
            result = RegSetValueEx(hKey, name.c_str(), 0, REG_SZ, 
                                   reinterpret_cast<const BYTE*>(value->c_str()), 
                                   static_cast<DWORD>((value->size() + 1) * sizeof(wchar_t)));
        } else {
            // 删除指定的环境变量
            result = RegDeleteValue(hKey, name.c_str());
        }
        RegCloseKey(hKey);

        if (result != ERROR_SUCCESS) {
            _tprintf(_T("Failed to update user environment variable %s\n"), name.c_str());
        } else {
            _tprintf(_T("Updated user environment variable %s=%s\n"),
                     name.c_str(), value ? value->c_str() : _T("(cleared)"));
        }
    } else {
        _tprintf(_T("Failed to open registry key for user environment variables.\n"));
    }
}
LRESULT CALLBACK Proxied::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    Proxied* pThis = reinterpret_cast<Proxied*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch (message) {
    case WM_TRAYICON: {
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hWnd);
            TrackPopupMenu(pThis->hPopupMenu_, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
            PostMessage(hWnd, WM_NULL, 0, 0);
        }
        break;
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDM_EXIT:
            Shell_NotifyIcon(NIM_DELETE, &pThis->nid_);
            PostQuitMessage(0);
            break;
        case IDM_ENABLE:
            if (!pThis->proxyEnabled_) {
                HKEY hKey;
                if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
                    DWORD value = 1;
                    RegSetValueEx(hKey, _T("ProxyEnable"), 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&value), sizeof(value));
                    RegSetValueEx(hKey, _T("ProxyServer"), 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(pThis->proxyServer_.c_str()),
                        (pThis->proxyServer_.length() + 1) * sizeof(wchar_t));
                    RegCloseKey(hKey);
                    pThis->proxyEnabled_ = true;
                    std::wstring proxyWithPrefix = pThis->EnsureProxyPrefix(pThis->proxyServer_);
                    pThis->UpdateUserEnvironmentVariable(_T("http_proxy"), &proxyWithPrefix);
                    pThis->UpdateUserEnvironmentVariable(_T("https_proxy"), &proxyWithPrefix);
                    pThis->UpdateGradleConfig(true);
                    _tcscpy_s(pThis->nid_.szTip, sizeof(pThis->nid_.szTip) / sizeof(TCHAR),
                        _T("Proxied - 代理已启用"));
                    Shell_NotifyIcon(NIM_MODIFY, &pThis->nid_);
                    CheckMenuItem(pThis->hPopupMenu_, IDM_ENABLE, MF_CHECKED);
                    CheckMenuItem(pThis->hPopupMenu_, IDM_DISABLE, MF_UNCHECKED);
                }
            }
            break;
        case IDM_DISABLE:
            if (pThis->proxyEnabled_) {
                HKEY hKey;
                if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
                    DWORD value = 0;
                    RegSetValueEx(hKey, _T("ProxyEnable"), 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&value), sizeof(value));
                    RegCloseKey(hKey);
                    pThis->proxyEnabled_ = false;
                    pThis->UpdateUserEnvironmentVariable(_T("http_proxy"), nullptr);
                    pThis->UpdateUserEnvironmentVariable(_T("https_proxy"), nullptr);
                    pThis->UpdateGradleConfig(false);
                    _tcscpy_s(pThis->nid_.szTip, sizeof(pThis->nid_.szTip) / sizeof(TCHAR),
                        _T("Proxied - 代理已禁用"));
                    Shell_NotifyIcon(NIM_MODIFY, &pThis->nid_);
                    CheckMenuItem(pThis->hPopupMenu_, IDM_ENABLE, MF_UNCHECKED);
                    CheckMenuItem(pThis->hPopupMenu_, IDM_DISABLE, MF_CHECKED);
                }
            }
            break;
        case IDM_AUTOSTART:
            pThis->SetAutoStart(!pThis->autoStart_);
            CheckMenuItem(pThis->hPopupMenu_, IDM_AUTOSTART,
                pThis->autoStart_ ? MF_CHECKED : MF_UNCHECKED);
            break;
        case IDM_CONFIG:
            DialogBoxParam(GetModuleHandle(NULL),
                MAKEINTRESOURCE(IDD_PROXY_CONFIG),
                hWnd, ConfigDialogProc,
                reinterpret_cast<LPARAM>(pThis));
            break;
        }
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

INT_PTR CALLBACK Proxied::ConfigDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    Proxied* pThis = nullptr;
    
    switch (message) {
    case WM_INITDIALOG: {
        // 设置标准Windows样式
        SetWindowLongPtr(hDlg, GWL_EXSTYLE, 
            GetWindowLongPtr(hDlg, GWL_EXSTYLE) | WS_EX_CLIENTEDGE);
        
        SetWindowLongPtr(hDlg, GWLP_USERDATA, lParam);
        pThis = reinterpret_cast<Proxied*>(lParam);

        // 初始化对话框控件
        SetDlgItemText(hDlg, IDC_PROXY_SERVER, pThis->proxyServer_.c_str());

        // 加载分组配置
        pThis->LoadProxyGroups();

        // 初始化分组下拉框
        HWND hCombo = GetDlgItem(hDlg, IDC_PROXY_GROUP);
        for (const auto& group : pThis->proxyGroups_) {
            SendMessage(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(group.name.c_str()));
        }

        // 设置当前选中分组
        if (!pThis->currentGroup_.empty()) {
            int index = SendMessage(hCombo, CB_FINDSTRINGEXACT, -1,
                reinterpret_cast<LPARAM>(pThis->currentGroup_.c_str()));
            if (index != CB_ERR) {
                SendMessage(hCombo, CB_SETCURSEL, index, 0);
            }
        }
        return TRUE;
    }

    case WM_COMMAND:
        pThis = reinterpret_cast<Proxied*>(GetWindowLongPtr(hDlg, GWLP_USERDATA));
        if (!pThis) return FALSE;
        
        switch (LOWORD(wParam)) {
        case IDOK: {
            // 保存配置
            wchar_t server[256];
            GetDlgItemText(hDlg, IDC_PROXY_SERVER, server, 256);
            pThis->proxyServer_ = server;

            // 保存当前选中分组
            HWND hCombo = GetDlgItem(hDlg, IDC_PROXY_GROUP);
            int sel = SendMessage(hCombo, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR) {
                wchar_t name[64];
                SendMessage(hCombo, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(name));
                pThis->currentGroup_ = name;
            }

            pThis->SaveProxyGroups();
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDC_ADD_GROUP: {
            // 添加新分组
            wchar_t name[64] = { 0 };
            if (DialogBoxParam(GetModuleHandle(NULL),
                MAKEINTRESOURCE(IDD_ADD_GROUP),
                hDlg, AddGroupDialogProc,
                reinterpret_cast<LPARAM>(name)) == IDOK) {

                // 添加到分组列表
                if (pThis->proxyGroups_.size() < static_cast<size_t>(10)) {
                    Proxied::ProxyGroup group;
                    group.name = name;
                    group.server = pThis->proxyServer_;
                    pThis->proxyGroups_.push_back(group);

                    // 更新下拉框
                    HWND hCombo = GetDlgItem(hDlg, IDC_PROXY_GROUP);
                    SendMessage(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
                    SendMessage(hCombo, CB_SETCURSEL, pThis->proxyGroups_.size() - 1, 0);
                }
            }
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

INT_PTR CALLBACK Proxied::AddGroupDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        // 设置标准Windows样式
        SetWindowLongPtr(hDlg, GWL_EXSTYLE, 
            GetWindowLongPtr(hDlg, GWL_EXSTYLE) | WS_EX_CLIENTEDGE);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            wchar_t* name = reinterpret_cast<wchar_t*>(lParam);
            GetDlgItemText(hDlg, IDC_GROUP_NAME, name, 64);
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void Proxied::LoadProxyGroups() {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER,
        _T("Software\\Proxied\\ProxyGroups"),
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {

        // 读取分组数量
        DWORD count = 0;
        DWORD size = sizeof(DWORD);
        if (RegQueryValueEx(hKey, _T("Count"), NULL, NULL,
            reinterpret_cast<LPBYTE>(&count), &size) == ERROR_SUCCESS) {

            proxyGroups_.resize(min(static_cast<size_t>(count), static_cast<size_t>(10)));

            // 读取每个分组
            for (size_t i = 0; i < proxyGroups_.size(); i++) {
                wchar_t valueName[32];
                _stprintf_s(valueName, 32, _T("Group%zu"), i);

                wchar_t valueData[320]; // name + server
                size = sizeof(valueData);
                if (RegQueryValueEx(hKey, valueName, NULL, NULL,
                    reinterpret_cast<LPBYTE>(valueData), &size) == ERROR_SUCCESS) {

                    // 格式: "name|server"
                    wchar_t* sep = wcschr(valueData, '|');
                    if (sep) {
                        *sep = '\0';
                        proxyGroups_[i].name = valueData;
                        proxyGroups_[i].server = sep + 1;
                    }
                }
            }
        }

        // 读取当前分组
        wchar_t group[64];
        size = sizeof(group);
        if (RegQueryValueEx(hKey, _T("CurrentGroup"), NULL, NULL,
            reinterpret_cast<LPBYTE>(group), &size) == ERROR_SUCCESS) {
            currentGroup_ = group;
        }

        RegCloseKey(hKey);
    }
}

void Proxied::SaveProxyGroups() {
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER,
        _T("Software\\Proxied\\ProxyGroups"),
        0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {

        // 保存分组数量
        DWORD count = static_cast<DWORD>(proxyGroups_.size());
        RegSetValueEx(hKey, _T("Count"), 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&count), sizeof(count));

        // 保存每个分组
        for (size_t i = 0; i < proxyGroups_.size(); i++) {
            wchar_t valueName[32];
            _stprintf_s(valueName, 32, _T("Group%zu"), i);

            std::wstring valueData = proxyGroups_[i].name + _T("|") + proxyGroups_[i].server;

            RegSetValueEx(hKey, valueName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(valueData.c_str()),
                (valueData.length() + 1) * sizeof(wchar_t));
        }

        // 保存当前分组
        if (!currentGroup_.empty()) {
            RegSetValueEx(hKey, _T("CurrentGroup"), 0, REG_SZ,
                reinterpret_cast<const BYTE*>(currentGroup_.c_str()),
                (currentGroup_.length() + 1) * sizeof(wchar_t));
        }

        RegCloseKey(hKey);
    }
}

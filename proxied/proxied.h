#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <shellapi.h>
class Proxied {
public:
    Proxied();
    ~Proxied();

    static DWORD WINAPI ThreadProc(LPVOID lpParam);

    std::wstring EnsureProxyPrefix(const std::wstring& proxy);

    void SyncSettings();

    void Run();

private:
    struct ProxyGroup {
        std::wstring name;
        std::wstring server;
    };

    // 常量定义
    static const UINT WM_TRAYICON = WM_USER + 1;
    static const UINT WM_WSL_RESULT = WM_USER + 2;
    static const int IDM_GITHUB = 109;
    static const int IDM_EXIT = 100;
    static const int IDM_ENABLE = 101;
    static const int IDM_DISABLE = 102;
    static const int IDM_CONFIG = 103;
    static const int IDM_AUTOSTART = 104;
    static const int IDM_GIT_PROXY = 110;
    static const int IDM_GRADLE_PROXY = 111;
    static const int IDM_WSL_GIT_PROXY = 112;
    static const int IDC_PROXY_SERVER = 105;
    static const int IDC_PROXY_GROUP = 106;
    static const int IDC_ADD_GROUP = 107;
    static const int IDC_GROUP_NAME = 108;

    // 成员变量
    std::vector<ProxyGroup> proxyGroups_;
    std::wstring currentGroup_;
    NOTIFYICONDATA nid_;
    HMENU hPopupMenu_;
    bool isUpdating;
    std::wstring proxyServer_;
    std::wstring nonProxyHosts_;
    std::wstring gradleConfigPath_;
    bool autoStart_;
    bool gitProxyEnabled_;
    bool gradleProxyEnabled_;
    bool wslGitProxyEnabled_;
    bool wslAvailable_;
    bool wslBusy_;

    std::wstring GetGradleConfigPath();
    bool UpdateGradleConfig(bool enable);
    bool UpdateGitConfig(bool enable);
    bool UpdateWslGitConfig(bool enable);
    HWND hWnd_;
    HANDLE hEvent_;

    // 私有方法
    void InitTrayIcon();
    void CheckAutoStart();
    void SetAutoStart(bool enable);
    bool GetProxySettings();
    void UpdateUserEnvironmentVariable(const std::wstring& name, const std::wstring* value);
    void HandleRegistryChanges(HANDLE hEvent);
    void LoadProxyGroups();
    void SaveProxyGroups();
    void CheckGitProxySetting();
    void SetGitProxySetting(bool enable);
    void CheckGradleProxySetting();
    void SetGradleProxySetting(bool enable);
    void CheckWslGitProxySetting();
    void SetWslGitProxySetting(bool enable);
    void CheckWslAvailability();
    void LogMenuAction(const std::wstring& action, DWORD elapsedMs);
    static std::wstring MenuName(int id);
    static DWORD WINAPI WslToggleThread(LPVOID lpParam);

    // 窗口过程
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    static INT_PTR CALLBACK ConfigDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
    static INT_PTR CALLBACK AddGroupDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
};

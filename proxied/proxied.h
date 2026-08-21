#pragma once

#include <windows.h>
#include <winnls.h>
#include <string>
#include <vector>
#include <shellapi.h>
#include <mutex>
class Proxied {
public:
    Proxied();
    ~Proxied();

    static DWORD WINAPI ThreadProc(LPVOID lpParam);

    std::wstring EnsureProxyPrefix(const std::wstring& proxy);

    void ApplyChanges();

    void Run();

    // 单实例互斥锁句柄由 wWinMain 传入（Shift+Exit 重启时需先释放）
    void SetSingleInstanceHandle(HANDLE h) { hSingleInstance_ = h; }

private:
    // 常量定义
    static const UINT WM_TRAYICON = WM_USER + 1;
    static const UINT WM_PROXY_OP_DONE = WM_USER + 3;
    static const UINT_PTR BUSY_TIMER_ID = 1;
    static const int IDM_GITHUB = 109;
    static const int IDM_EXIT = 100;
    static const int IDM_ENABLE = 101;
    static const int IDM_DISABLE = 102;
    static const int IDM_AUTOSTART = 104;
    static const int IDM_GIT_PROXY = 110;
    static const int IDM_GRADLE_PROXY = 111;
    static const int IDM_WSL_GIT_PROXY = 112;
    static const int IDM_SYNC = 113;

    // 成员变量
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
    bool opBusy_;
    bool proxyEnabled_;
    int busyFrame_;
    std::mutex applyMutex_;
    std::mutex logMutex_;
    HINSTANCE hResourceInstance_;
    HANDLE hSingleInstance_;

    std::wstring GetGradleConfigPath();
    bool UpdateGradleConfig(bool enable);
    bool UpdateGitConfig(bool enable);
    bool UpdateWslGitConfig(bool enable);
    HWND hWnd_;
    HANDLE hEvent_;

    // 私有方法
    void InitTrayIcon();
    void SetTrayIcon(HICON hIcon);
    HICON MakeBusyIcon(int frame);
    void CheckAutoStart();
    void SetAutoStart(bool enable);
    bool GetProxySettings();
    void UpdateUserEnvironmentVariable(const std::wstring& name, const std::wstring* value);
    void HandleRegistryChanges(HANDLE hEvent);
    void CheckGitProxySetting();
    void SetGitProxySetting(bool enable);
    void CheckGradleProxySetting();
    void SetGradleProxySetting(bool enable);
    void CheckWslGitProxySetting();
    void SetWslGitProxySetting(bool enable);
    void CheckWslAvailability();
    void ResetMenuLog();
    void LogProgramCall(const std::wstring& name, bool ok,
        const std::wstring& input = L"", const std::string& output = std::string());
    bool RunHiddenCommand(const std::wstring& commandLine, std::string& output, DWORD& exitCode);
    static DWORD WINAPI OpThread(LPVOID lpParam);
    void DetectLanguage();
    std::wstring LoadLocalizedString(UINT id);
    std::wstring LoadStringByLang(UINT id, LANGID lang);
    HBITMAP CreateSectionIconBitmap();

    // 窗口过程
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
};

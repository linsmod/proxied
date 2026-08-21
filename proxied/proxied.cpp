#include "pch.h"
#include "proxied.h"
#include "Resource.h"
#include "version.h"
#include <fstream>
#include <shellapi.h>
#include <cmath>
#include <ole2.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <tchar.h>
#include <algorithm> // for std::replace
#include <stdio.h>
#include <commctrl.h>
#include <shlobj.h>
#pragma comment(lib, "comctl32.lib")
#define REG_PATH _T("Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings")
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	//// 初始化通用控件
	//INITCOMMONCONTROLSEX icex;
	//icex.dwSize = sizeof(icex);
	//icex.dwICC = ICC_WIN95_CLASSES;
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
	InitCommonControls();

	// 单实例：若已有一个实例在运行则直接退出，避免重复托盘图标
	HANDLE hSingleInstance = CreateMutex(NULL, FALSE,
		_T("Proxied_SingleInstance_{9F2C1A3B-7E4D-4B8A-9C1E-2D5F6A8B0C3E}"));
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		if (hSingleInstance) {
			CloseHandle(hSingleInstance);
		}
		return 0;
	}

	Proxied app;
	app.Run();

	if (hSingleInstance) {
		CloseHandle(hSingleInstance);
	}
	return 0;
}
Proxied::Proxied() :
	isUpdating(false),
	autoStart_(false),
  gitProxyEnabled_(true),
  gradleProxyEnabled_(true),
  wslGitProxyEnabled_(false),
  wslAvailable_(false),
  opBusy_(false),
  proxyEnabled_(false),
  busyFrame_(0),
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

struct OpCtx {
    Proxied* self;
    int opId;
    DWORD startTick;
};

static const int BUSY_FRAMES = 12;
static const int BUSY_INTERVAL = 80;

void Proxied::Run() {
	// 启用DPI感知
	SetProcessDPIAware();

	// 初始化 GDI+（用于绘制带 alpha 的旋转 busy 图标）
	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	ULONG_PTR gdiplusToken = 0;
	Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

	// 每次启动重置日志（不追加历史记录）
	ResetMenuLog();

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
	CheckGitProxySetting();
	CheckGradleProxySetting();
	CheckWslGitProxySetting();
	CheckWslAvailability();

	InitTrayIcon();

	// 启动的时候也走后台线程同步，期间显示 busy 旋转图标（尤其 WSL 冷启动较慢）
	opBusy_ = true;
	SetTimer(hWnd_, BUSY_TIMER_ID, BUSY_INTERVAL, NULL);
	busyFrame_ = 0;
	_tcscpy_s(nid_.szTip, ARRAYSIZE(nid_.szTip), _T("Proxied - 正在处理..."));
	SetTrayIcon(MakeBusyIcon(0));
	{
		OpCtx* ctx = new OpCtx{ this, IDM_SYNC, GetTickCount() };
		HANDLE hThread = CreateThread(NULL, 0, &Proxied::OpThread, ctx, 0, NULL);
		if (hThread) {
			CloseHandle(hThread);
		}
		else {
			opBusy_ = false;
			KillTimer(hWnd_, BUSY_TIMER_ID);
			SetTrayIcon(LoadIcon(GetModuleHandle(NULL),
				MAKEINTRESOURCE(proxyEnabled_ ? IDI_SMALL2 : IDI_SMALL)));
		}
	}

	// 创建一个手动重置的事件对象
	hEvent_ = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!hEvent_) {
		return;
	}

	// 创建消息循环线程监听变化来自动同步
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

	Gdiplus::GdiplusShutdown(gdiplusToken);
}

void Proxied::InitTrayIcon() {
	// 创建托盘菜单
	hPopupMenu_ = CreatePopupMenu();
	AppendMenu(hPopupMenu_, MF_STRING | 0, IDM_ENABLE, _T("启用代理"));
	AppendMenu(hPopupMenu_, MF_STRING | 0, IDM_DISABLE, _T("禁用代理"));
	AppendMenu(hPopupMenu_, MF_SEPARATOR, 0, NULL);
	//AppendMenu(hPopupMenu_, MF_STRING, IDM_CONFIG, _T("配置代理..."));
	//AppendMenu(hPopupMenu_, MF_SEPARATOR, 0, NULL);
	AppendMenu(hPopupMenu_, MF_STRING | (autoStart_ ? MF_CHECKED : 0), IDM_AUTOSTART, _T("开机自启"));
	AppendMenu(hPopupMenu_, MF_STRING | (gitProxyEnabled_ ? MF_CHECKED : 0), IDM_GIT_PROXY, _T("当启用或禁用时也修改Git的全局代理设置"));
	AppendMenu(hPopupMenu_, MF_STRING | (gradleProxyEnabled_ ? MF_CHECKED : 0), IDM_GRADLE_PROXY, _T("当启用或禁用时也修改Gradle的代理设置"));
	AppendMenu(hPopupMenu_, MF_STRING | (wslGitProxyEnabled_ ? MF_CHECKED : 0), IDM_WSL_GIT_PROXY, _T("当启用或禁用时也修改WSL内Git的代理设置"));
	AppendMenu(hPopupMenu_, MF_SEPARATOR, 0, NULL);
	AppendMenu(hPopupMenu_, MF_STRING, IDM_GITHUB,
		(std::wstring(_T("关于 ")) + PROXIED_VERSION).c_str());
	AppendMenu(hPopupMenu_, MF_STRING, IDM_EXIT, _T("退出"));

	// 初始化托盘图标
	nid_.cbSize = sizeof(nid_);
	nid_.hWnd = hWnd_;
	nid_.uID = 1;
	nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid_.uCallbackMessage = WM_TRAYICON;
	nid_.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_SMALL));
	_tcscpy_s(nid_.szTip, sizeof(nid_.szTip) / sizeof(TCHAR),
		_T("Proxied - Loading..."));

	Shell_NotifyIcon(NIM_ADD, &nid_);
}

void Proxied::SetTrayIcon(HICON hIcon) {
    if (nid_.hIcon) {
        DestroyIcon(nid_.hIcon);
    }
    nid_.hIcon = hIcon;
    Shell_NotifyIcon(NIM_MODIFY, &nid_);
}

// 在基础图标上叠加旋转的系统等待光标（纯转圈，IDC_WAIT），生成 busy 图标
HICON Proxied::MakeBusyIcon(int frame) {
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size <= 0) size = 16;

    HICON hBase = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(proxyEnabled_ ? IDI_SMALL2 : IDI_SMALL));
    Gdiplus::Bitmap* bmpBase = Gdiplus::Bitmap::FromHICON(hBase);
    DestroyIcon(hBase);
    if (!bmpBase) {
        return LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(proxyEnabled_ ? IDI_SMALL2 : IDI_SMALL));
    }

    // 系统等待光标（纯转圈不带箭头）：GDI+ 的 FromHICON 对光标无效，
    // 改为画到 32bpp DIB 再用其像素构造 GDI+ Bitmap（保留 alpha）
    Gdiplus::Bitmap* bmpWait = NULL;
    HBITMAP hBmpWait = NULL;
    HCURSOR hWait = LoadCursor(NULL, IDC_WAIT);
    if (hWait) {
        BITMAPINFO bmi = { 0 };
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = size;
        bmi.bmiHeader.biHeight = -size;  // 自上而下
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* waitBits = NULL;
        hBmpWait = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &waitBits, NULL, 0);
        if (hBmpWait) {
            HDC hdcW = CreateCompatibleDC(NULL);
            HGDIOBJ oldW = SelectObject(hdcW, hBmpWait);
            memset(waitBits, 0, static_cast<size_t>(size) * size * 4);
            DrawIconEx(hdcW, 0, 0, hWait, size, size, 0, NULL, DI_NORMAL);
            SelectObject(hdcW, oldW);
            DeleteDC(hdcW);
            bmpWait = new Gdiplus::Bitmap(size, size, size * 4,
                PixelFormat32bppARGB, static_cast<BYTE*>(waitBits));
        }
    }

    Gdiplus::Bitmap bmpOut(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bmpOut);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(bmpBase, 0, 0, size, size);
        if (bmpWait) {
            double angle = frame * (360.0 / BUSY_FRAMES);
            g.TranslateTransform(static_cast<Gdiplus::REAL>(size) / 2, static_cast<Gdiplus::REAL>(size) / 2);
            g.RotateTransform(static_cast<Gdiplus::REAL>(angle));
            g.TranslateTransform(-static_cast<Gdiplus::REAL>(size) / 2, -static_cast<Gdiplus::REAL>(size) / 2);
            // 放大绘制（超出图标画布的部分会被裁掉），让转圈更醒目
            int ringSize = size + size / 2;
            int off = (ringSize - size) / 2;
            g.DrawImage(bmpWait, static_cast<Gdiplus::REAL>(-off),
                static_cast<Gdiplus::REAL>(-off),
                static_cast<Gdiplus::REAL>(ringSize), static_cast<Gdiplus::REAL>(ringSize));
        }
    }

    HICON hIcon = NULL;
    Gdiplus::Status st = bmpOut.GetHICON(&hIcon);

    delete bmpBase;
    if (bmpWait) delete bmpWait;
    if (hBmpWait) DeleteObject(hBmpWait);

    if (st != Gdiplus::Ok || !hIcon) {
        hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(proxyEnabled_ ? IDI_SMALL2 : IDI_SMALL));
    }
    return hIcon;
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
				static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
		}
		else {
			RegDeleteValue(hKey, _T("Proxied"));
		}

		RegCloseKey(hKey);
		autoStart_ = enable;
	}
}
bool Proxied::GetProxySettings() {
	DWORD proxyEnabled_ = 0; // 使用 DWORD 而不是 bool
	HKEY hKey = nullptr;

	if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		DWORD type, size;

		// 读取 ProxyEnable
		size = sizeof(DWORD);
		if (RegQueryValueEx(hKey, _T("ProxyEnable"), NULL, &type,
			reinterpret_cast<LPBYTE>(&proxyEnabled_), &size) == ERROR_SUCCESS &&
			type == REG_DWORD) {
			// 成功读取 ProxyEnable
		}

		// 读取 ProxyServer
		wchar_t server[256];
		size = sizeof(server);
		if (RegQueryValueEx(hKey, _T("ProxyServer"), NULL, &type,
			reinterpret_cast<LPBYTE>(server), &size) == ERROR_SUCCESS &&
			type == REG_SZ) {
			proxyServer_ = server;
		}

		// 读取 ProxyOverride
		wchar_t nonProxy[1024];
		size = sizeof(nonProxy);
		if (RegQueryValueEx(hKey, _T("ProxyOverride"), NULL, &type,
			reinterpret_cast<LPBYTE>(nonProxy), &size) == ERROR_SUCCESS &&
			type == REG_SZ) {
			nonProxyHosts_ = nonProxy;
		}

		RegCloseKey(hKey); // 关闭注册表句柄
	}

	return proxyEnabled_ != 0; // 将 DWORD 转换为 bool
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
	bool fileExists = inFile.good();

	// 读取现有配置并过滤所有代理相关设置
	if (fileExists) {
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
	else if (enable) {
		// 如果文件不存在且需要启用代理，标记为已更改以创建新文件
		changed = true;
		// 确保 lines 向量为空
		lines.clear();
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
				// 转换Windows格式(分号分隔)为Gradle格式(竖线分隔)
				std::wstring gradleNonProxy = nonProxyHosts_;
				std::replace(gradleNonProxy.begin(), gradleNonProxy.end(), L';', L'|');
				lines.push_back(_T("systemProp.http.nonProxyHosts=") + gradleNonProxy);
				lines.push_back(_T("systemProp.https.nonProxyHosts=") + gradleNonProxy);
			}
			changed = true;
		}
	}

	// 写入文件
	bool result = false;
	if (changed) {
		// 确保目录存在
		std::wstring dirPath = configPath.substr(0, configPath.find_last_of(L'\\'));
		SHCreateDirectoryEx(NULL, dirPath.c_str(), NULL);
		
		std::wofstream outFile(configPath);
		if (outFile) {
			for (const auto& line : lines) {
				outFile << line << std::endl;
			}
			outFile.close();
			result = true;
		}
		LogProgramCall(L"Gradle", result);
	}
	return result;
}

void Proxied::UpdateUserEnvironmentVariable(const std::wstring& name, const std::wstring* value) {
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, _T("Environment"), 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
		if (value) {
			RegSetValueEx(hKey, name.c_str(), 0, REG_SZ, (const BYTE*)value->c_str(), static_cast<DWORD>((value->size() + 1) * sizeof(wchar_t)));
		}
		else {
			RegDeleteValue(hKey, name.c_str());
		}
		RegCloseKey(hKey);

		// 广播环境变量更改
		// SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)_T("Environment"), SMTO_ABORTIFHUNG, 5000, NULL);
        
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

// 以隐藏方式运行命令，捕获 stdout/stderr 到 output，返回是否成功创建进程
bool Proxied::RunHiddenCommand(const std::wstring& commandLine,
                               std::string& output, DWORD& exitCode) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    HANDLE hRead = NULL, hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        return false;
    }
    // 读取端不被子进程继承
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = NULL;

    if (!CreateProcess(NULL, (LPWSTR)commandLine.c_str(), NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return false;
    }
    CloseHandle(hWrite);  // 关闭父进程写入端，子进程退出后 ReadFile 才会返回

    char buf[4096];
    DWORD read = 0;
    std::string all;
    while (ReadFile(hRead, buf, sizeof(buf), &read, NULL) && read > 0) {
        all.append(buf, read);
    }
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // 管道输出编码归一化：WSL/git 有时以 UTF-16LE 写入管道，转成 UTF-8 以便日志正确显示
    if (all.size() >= 2 && (all.size() % 2 == 0) && all[1] == '\0') {
        const wchar_t* w = reinterpret_cast<const wchar_t*>(all.data());
        int wlen = static_cast<int>(all.size() / 2);
        int sz = WideCharToMultiByte(CP_UTF8, 0, w, wlen, NULL, 0, NULL, NULL);
        if (sz > 0) {
            std::string u8(sz, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, wlen, &u8[0], sz, NULL, NULL);
            output = std::move(u8);
            return true;
        }
    }
    output = std::move(all);
    return true;
}

bool Proxied::UpdateGitConfig(bool enable) {
    // 构建 git 命令
    std::wstring command;
    if (enable) {
        std::wstring proxyWithPrefix = EnsureProxyPrefix(proxyServer_);
        // 组合命令，使用 & 连接多个命令
        command = L"git config --global http.proxy \"" + proxyWithPrefix + L"\" & git config --global https.proxy \"" + proxyWithPrefix + L"\"";
    } else {
        // 组合命令，使用 & 连接多个命令；不存在的键也视为成功（exit 0）
        command = L"git config --global --unset http.proxy & git config --global --unset https.proxy & exit 0";
    }
    
    // 准备执行命令
    // 使用 cmd.exe 执行命令
    std::wstring fullCommand = L"cmd.exe /c " + command;

    DWORD exitCode = 0;
    std::string output;
    if (!RunHiddenCommand(fullCommand, output, exitCode)) {
        LogProgramCall(L"Git", false, command, std::string());
        return false;
    }

    bool ok = (exitCode == 0);
    LogProgramCall(L"Git", ok, command, output);
    return ok;
}

bool Proxied::UpdateWslGitConfig(bool enable) {
    std::wstring command;
    if (enable) {
        std::wstring proxyWithPrefix = EnsureProxyPrefix(proxyServer_);
        // 在 WSL 里设置 git 的全局代理
        command = L"wsl.exe -e sh -c \"git config --global http.proxy '" + proxyWithPrefix +
            L"' && git config --global https.proxy '" + proxyWithPrefix + L"'\"";
    } else {
        // 关闭时移除代理配置；--unset-all 对不存在的键会报错，2>/dev/null 吞掉即可
        command = L"wsl.exe -e sh -c \"git config --global --unset-all http.proxy 2>/dev/null; "
            L"git config --global --unset-all https.proxy 2>/dev/null; exit 0\"";
    }

    // 直接启动 wsl.exe（不经过 cmd，避免引号转义问题）
    DWORD exitCode = 0;
    std::string output;
    if (!RunHiddenCommand(command, output, exitCode)) {
        LogProgramCall(L"WSL Git", false, command, std::string());
        return false;
    }

    bool ok = (exitCode == 0);
    LogProgramCall(L"WSL Git", ok, command, output);
    return ok;
}

void Proxied::ApplyChanges() {
    // 序列化：避免与后台操作线程或注册表监听线程并发执行
    std::lock_guard<std::mutex> lock(applyMutex_);
    // 初始读取代理设置
    bool proxyEnabled = GetProxySettings();
    proxyEnabled_ = proxyEnabled;

    if (proxyEnabled) {
        std::wstring proxyWithPrefix = EnsureProxyPrefix(proxyServer_);
        UpdateUserEnvironmentVariable(_T("http_proxy"), &proxyWithPrefix);
        UpdateUserEnvironmentVariable(_T("https_proxy"), &proxyWithPrefix);
        if (gradleProxyEnabled_) {
            UpdateGradleConfig(true);
        }
        if (gitProxyEnabled_) {
            UpdateGitConfig(true);
        }
        if (wslAvailable_ && wslGitProxyEnabled_) {
            UpdateWslGitConfig(true);
        }
    } else {
        UpdateUserEnvironmentVariable(_T("http_proxy"), nullptr);
        UpdateUserEnvironmentVariable(_T("https_proxy"), nullptr);
        if (gradleProxyEnabled_) {
            UpdateGradleConfig(false);
        }
        if (gitProxyEnabled_) {
            UpdateGitConfig(false);
        }
        if (wslAvailable_ && wslGitProxyEnabled_) {
            UpdateWslGitConfig(false);
        }
    }
    // 广播环境变量变更到其他应用程序
    //PostMessage(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)_T("Environment"));
	 SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)_T("Environment"), SMTO_ABORTIFHUNG, 5000, NULL);

	// 更新托盘提示与图标（busy 时由旋转图标接管，跳过以免闪烁）
	_tcscpy_s(nid_.szTip, ARRAYSIZE(nid_.szTip),
		proxyEnabled ? _T("Proxied - 代理已启用") : _T("Proxied - 代理已禁用"));
	if (!opBusy_) {
		HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(proxyEnabled ? IDI_SMALL2 : IDI_SMALL));
		SetTrayIcon(hIcon);
	}

	// 更新菜单状态
	CheckMenuItem(hPopupMenu_, IDM_ENABLE, MF_BYCOMMAND | (proxyEnabled ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(hPopupMenu_, IDM_DISABLE, MF_BYCOMMAND | (proxyEnabled ? MF_UNCHECKED : MF_CHECKED));
	CheckMenuItem(hPopupMenu_, IDM_GIT_PROXY, MF_BYCOMMAND | (gitProxyEnabled_ ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(hPopupMenu_, IDM_GRADLE_PROXY, MF_BYCOMMAND | (gradleProxyEnabled_ ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(hPopupMenu_, IDM_WSL_GIT_PROXY, MF_BYCOMMAND | (wslGitProxyEnabled_ ? MF_CHECKED : MF_UNCHECKED));
}

void Proxied::HandleRegistryChanges(HANDLE hEvent) {
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, KEY_NOTIFY, &hKey) != ERROR_SUCCESS) {
		return;
	}
	while (RegNotifyChangeKeyValue(hKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, hEvent, TRUE) != ERROR_SUCCESS) {
		Sleep(1000);
	}
	while (true) {
		// 等待事件被触发
		if (WaitForSingleObject(hEvent, INFINITE) == WAIT_OBJECT_0) {
			// 自己发起的修改（后台操作线程已处理），跳过避免重复执行
			if (!opBusy_) {
				ApplyChanges();
			}
			isUpdating = false;
			// 重置事件以继续监听
			if (RegNotifyChangeKeyValue(hKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, hEvent, TRUE) != ERROR_SUCCESS) {
				break;
			}
		}
	}

	RegCloseKey(hKey);
}

// 统一的后台操作线程：同一时间只允许一个操作在进行中（由 opBusy_ 守护）
DWORD WINAPI Proxied::OpThread(LPVOID lpParam) {
    OpCtx* ctx = reinterpret_cast<OpCtx*>(lpParam);
    Proxied* self = ctx->self;
    bool ok = true;

    switch (ctx->opId) {
    case IDM_ENABLE:
    case IDM_DISABLE: {
        bool enable = (ctx->opId == IDM_ENABLE);
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            DWORD value = enable ? 1 : 0;
            RegSetValueEx(hKey, _T("ProxyEnable"), 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&value), sizeof(value));
            RegCloseKey(hKey);
        }
        self->ApplyChanges();
        break;
    }
    case IDM_SYNC:
        // 启动时的初始同步（无状态变更），同样在后台线程执行以显示 busy 图标
        self->ApplyChanges();
        break;
    case IDM_GIT_PROXY:
        self->SetGitProxySetting(!self->gitProxyEnabled_);
        self->ApplyChanges();
        break;
    case IDM_GRADLE_PROXY:
        self->SetGradleProxySetting(!self->gradleProxyEnabled_);
        self->ApplyChanges();
        break;
    case IDM_WSL_GIT_PROXY: {
        bool enable = !self->wslGitProxyEnabled_;
        ok = self->UpdateWslGitConfig(enable);
        if (ok) {
            // 在 opBusy_ 仍为 true 时落盘，避免注册表监听线程重复触发 ApplyChanges
            self->SetWslGitProxySetting(enable);
        }
        break;
    }
    }

    PostMessage(self->hWnd_, WM_PROXY_OP_DONE, ctx->opId, ok ? 1 : 0);
    delete ctx;
    return 0;
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
		case IDM_GITHUB:
			if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
				// 按住 SHIFT：打开日志文件
				wchar_t temp[MAX_PATH];
				GetEnvironmentVariable(_T("TEMP"), temp, MAX_PATH);
				std::wstring logPath = std::wstring(temp) + L"\\proxied_menu.log";
				ShellExecute(NULL, _T("open"), _T("notepad.exe"),
					logPath.c_str(), NULL, SW_SHOWNORMAL);
			}
			else {
				ShellExecute(NULL, _T("open"), _T("https://github.com/linsmod/proxied/releases"), NULL, NULL, SW_SHOWNORMAL);
			}
			break;
		case IDM_EXIT:
			Shell_NotifyIcon(NIM_DELETE, &pThis->nid_);
			PostQuitMessage(0);
			break;
		case IDM_AUTOSTART:
			pThis->SetAutoStart(!pThis->autoStart_);
			CheckMenuItem(pThis->hPopupMenu_, IDM_AUTOSTART,
				pThis->autoStart_ ? MF_CHECKED : MF_UNCHECKED);
			break;
		case IDM_ENABLE:
		case IDM_DISABLE:
		case IDM_GIT_PROXY:
		case IDM_GRADLE_PROXY:
		case IDM_WSL_GIT_PROXY:
			// 统一交给后台线程执行，保证同一时间只有一个操作在进行
			if (pThis->opBusy_) {
				break;
			}
			pThis->opBusy_ = true;
			// 启动旋转图标计时器，立即显示第一帧
			SetTimer(pThis->hWnd_, BUSY_TIMER_ID, BUSY_INTERVAL, NULL);
			pThis->busyFrame_ = 0;
			_tcscpy_s(pThis->nid_.szTip, ARRAYSIZE(pThis->nid_.szTip), _T("Proxied - 正在处理..."));
			pThis->SetTrayIcon(pThis->MakeBusyIcon(0));
			{
				OpCtx* ctx = new OpCtx{ pThis, LOWORD(wParam), GetTickCount() };
				HANDLE hThread = CreateThread(NULL, 0,
					&Proxied::OpThread, ctx, 0, NULL);
				if (hThread) {
					CloseHandle(hThread);
				}
				else {
					pThis->opBusy_ = false;
					KillTimer(pThis->hWnd_, BUSY_TIMER_ID);
					pThis->SetTrayIcon(LoadIcon(GetModuleHandle(NULL),
						MAKEINTRESOURCE(pThis->proxyEnabled_ ? IDI_SMALL2 : IDI_SMALL)));
					MessageBox(hWnd, _T("无法启动后台任务。"),
						_T("Proxied"), MB_OK | MB_ICONWARNING);
				}
			}
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
	case WM_PROXY_OP_DONE: {
		int opId = static_cast<int>(wParam);
		bool ok = (lParam != 0);
		pThis->opBusy_ = false;
		// 停止旋转并恢复普通图标
		KillTimer(hWnd, BUSY_TIMER_ID);
		pThis->SetTrayIcon(LoadIcon(GetModuleHandle(NULL),
			MAKEINTRESOURCE(pThis->proxyEnabled_ ? IDI_SMALL2 : IDI_SMALL)));
		if (opId == IDM_WSL_GIT_PROXY) {
			if (ok) {
				CheckMenuItem(pThis->hPopupMenu_, IDM_WSL_GIT_PROXY,
					pThis->wslGitProxyEnabled_ ? MF_CHECKED : MF_UNCHECKED);
			}
			else {
				MessageBox(hWnd, _T("无法修改 WSL 内 Git 的代理设置，请检查 WSL 与 Git 是否已安装。"),
					_T("Proxied"), MB_OK | MB_ICONWARNING);
			}
		}
		break;
	}
	case WM_TIMER: {
		if (wParam == BUSY_TIMER_ID) {
			pThis->busyFrame_ = (pThis->busyFrame_ + 1) % BUSY_FRAMES;
			pThis->SetTrayIcon(pThis->MakeBusyIcon(pThis->busyFrame_));
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
			LRESULT index = SendMessage(hCombo, CB_FINDSTRINGEXACT, -1,
				reinterpret_cast<LPARAM>(pThis->currentGroup_.c_str()));
			if (index != CB_ERR) {
				SendMessage(hCombo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
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
			LRESULT sel = SendMessage(hCombo, CB_GETCURSEL, 0, 0);
			if (sel != CB_ERR) {
				wchar_t name[64];
				SendMessage(hCombo, CB_GETLBTEXT, static_cast<WPARAM>(sel), reinterpret_cast<LPARAM>(name));
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
                static_cast<DWORD>((valueData.length() + 1) * sizeof(wchar_t)));
        }
    
        // 保存当前分组
        if (!currentGroup_.empty()) {
            RegSetValueEx(hKey, _T("CurrentGroup"), 0, REG_SZ,
                reinterpret_cast<const BYTE*>(currentGroup_.c_str()),
                static_cast<DWORD>((currentGroup_.length() + 1) * sizeof(wchar_t)));
        }
    
        RegCloseKey(hKey);
    }
}

void Proxied::CheckGitProxySetting() {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER,
        _T("Software\\Proxied"),
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
    
        DWORD value = 1; // 默认启用
        DWORD size = sizeof(DWORD);
        if (RegQueryValueEx(hKey, _T("GitProxyEnabled"), NULL, NULL,
            reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS) {
            gitProxyEnabled_ = (value != 0);
        }
    
        RegCloseKey(hKey);
    }
}

void Proxied::SetGitProxySetting(bool enable) {
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER,
        _T("Software\\Proxied"),
        0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
    
        DWORD value = enable ? 1 : 0;
        RegSetValueEx(hKey, _T("GitProxyEnabled"), 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value));
    
        RegCloseKey(hKey);
        gitProxyEnabled_ = enable;
    }
}

void Proxied::CheckGradleProxySetting() {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER,
        _T("Software\\Proxied"),
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
    
        DWORD value = 1; // 默认启用
        DWORD size = sizeof(DWORD);
        if (RegQueryValueEx(hKey, _T("GradleProxyEnabled"), NULL, NULL,
            reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS) {
            gradleProxyEnabled_ = (value != 0);
        }
    
        RegCloseKey(hKey);
    }
}

void Proxied::SetGradleProxySetting(bool enable) {
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER,
        _T("Software\\Proxied"),
        0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
    
        DWORD value = enable ? 1 : 0;
        RegSetValueEx(hKey, _T("GradleProxyEnabled"), 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value));
    
        RegCloseKey(hKey);
        gradleProxyEnabled_ = enable;
        }
        }

        void Proxied::CheckWslGitProxySetting() {
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_CURRENT_USER,
            _T("Software\\Proxied"),
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
    
        DWORD value = 0; // 默认不启用（首次使用时）
        DWORD size = sizeof(DWORD);
        if (RegQueryValueEx(hKey, _T("WslGitProxyEnabled"), NULL, NULL,
            reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS) {
            wslGitProxyEnabled_ = (value != 0);
        }
    
            RegCloseKey(hKey);
        }
        }

        void Proxied::SetWslGitProxySetting(bool enable) {
        HKEY hKey;
        if (RegCreateKeyEx(HKEY_CURRENT_USER,
            _T("Software\\Proxied"),
            0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        
            DWORD value = enable ? 1 : 0;
            RegSetValueEx(hKey, _T("WslGitProxyEnabled"), 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&value), sizeof(value));
        
            RegCloseKey(hKey);
            wslGitProxyEnabled_ = enable;
        }
        }

        void Proxied::CheckWslAvailability() {
            wchar_t path[MAX_PATH];
            wslAvailable_ = (SearchPathW(NULL, L"wsl.exe", NULL, MAX_PATH, path, NULL) != 0);
        }

        static std::string WStringToUtf8(const std::wstring& s) {
            if (s.empty()) return std::string();
            int sz = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, NULL, 0, NULL, NULL);
            std::string out(sz - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], sz, NULL, NULL);
            return out;
        }

        void Proxied::ResetMenuLog() {
            std::lock_guard<std::mutex> lock(logMutex_);
            wchar_t temp[MAX_PATH];
            DWORD len = GetEnvironmentVariable(_T("TEMP"), temp, MAX_PATH);
            std::string path = WStringToUtf8((len > 0 && len <= MAX_PATH)
                ? std::wstring(temp) + L"\\proxied_menu.log"
                : L"proxied_menu.log");

            // 每次启动清空旧日志（不追加历史），写 UTF-8 BOM + 启动头
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (out) {
                const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
                out.write(reinterpret_cast<const char*>(bom), 3);

                SYSTEMTIME st;
                GetLocalTime(&st);
                char ts[64];
                sprintf_s(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

                std::string header = std::string(ts) +
                    "  === Proxied 启动 (版本 " + WStringToUtf8(PROXIED_VERSION) + ") ===\n";
                out.write(header.c_str(), static_cast<std::streamsize>(header.size()));
                out.flush();
            }
        }

        void Proxied::LogProgramCall(const std::wstring& name, bool ok,
                const std::wstring& input, const std::string& output) {
            std::lock_guard<std::mutex> lock(logMutex_);
            wchar_t temp[MAX_PATH];
            DWORD len = GetEnvironmentVariable(_T("TEMP"), temp, MAX_PATH);
            std::string path = WStringToUtf8((len > 0 && len <= MAX_PATH)
                ? std::wstring(temp) + L"\\proxied_menu.log"
                : L"proxied_menu.log");

            SYSTEMTIME st;
            GetLocalTime(&st);
            char ts[64];
            sprintf_s(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d  ",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

            std::string line = std::string(ts) + "[程序] " + WStringToUtf8(name) +
                (ok ? " 执行成功\n" : " 执行失败\n");
            if (!input.empty()) {
                line += "  命令: " + WStringToUtf8(input) + "\n";
            }
            if (!output.empty()) {
                line += "  输出:\n";
                line += output;
                if (output.back() != '\n') {
                    line += "\n";
                }
            }

            std::ofstream out(path, std::ios::binary | std::ios::app);
            if (out) {
                out.write(line.c_str(), static_cast<std::streamsize>(line.size()));
                out.flush();
            }
        }


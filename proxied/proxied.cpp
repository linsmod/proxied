#include "pch.h"
#include "proxied.h"
#include "resource.h"
#include "StringResources.h"
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
#include <cwctype>
#include <cstdlib>
#include <iterator>
#pragma comment(lib, "comctl32.lib")
#define REG_PATH _T("Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings")

namespace {
struct ParsedProxyAddress {
	std::wstring normalized;
	std::wstring host;
	std::wstring hostPort;
	std::wstring port;
};

std::wstring TrimCopy(const std::wstring& value) {
	size_t first = 0;
	while (first < value.size() && std::iswspace(value[first])) ++first;
	size_t last = value.size();
	while (last > first && std::iswspace(value[last - 1])) --last;
	return value.substr(first, last - first);
}

std::wstring LowerCopy(std::wstring value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
	return value;
}

// 将用户输入规范化为完整 URI，同时提取 Windows/Gradle 需要的主机与端口。
bool ParseProxyAddress(const std::wstring& input, bool socks5, ParsedProxyAddress& result) {
	std::wstring value = TrimCopy(input);
	if (value.empty()) return false;

	std::wstring scheme = socks5 ? L"socks5" : L"http";
	size_t schemeEnd = value.find(L"://");
	if (schemeEnd != std::wstring::npos) {
		scheme = LowerCopy(value.substr(0, schemeEnd));
		if (socks5) {
			if (scheme != L"socks5") return false;
		}
		else if (scheme != L"http") {
			return false;
		}
		value = value.substr(schemeEnd + 3);
	}

	if (value.empty() || value.find_first_of(L"/\\?#\"';&|<>") != std::wstring::npos ||
		std::any_of(value.begin(), value.end(), [](wchar_t c) { return std::iswspace(c) != 0; })) {
		return false;
	}

	std::wstring host;
	std::wstring port;
	if (value[0] == L'[') {
		size_t close = value.find(L']');
		if (close == std::wstring::npos || close == 1 || close + 1 >= value.size() || value[close + 1] != L':') {
			return false;
		}
		host = value.substr(1, close - 1);
		port = value.substr(close + 2);
	}
	else {
		size_t colon = value.rfind(L':');
		if (colon == std::wstring::npos || colon == 0 || value.find(L':') != colon) return false;
		host = value.substr(0, colon);
		port = value.substr(colon + 1);
	}

	// 主机名只接受 DNS/IPv4 字符；IPv6 只接受十六进制、冒号和点，避免命令行元字符。
	bool ipv6 = value[0] == L'[';
	bool validHost = std::all_of(host.begin(), host.end(), [ipv6](wchar_t c) {
		if (ipv6) {
			return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
				(c >= L'A' && c <= L'F') || c == L':' || c == L'.';
		}
		return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
			(c >= L'A' && c <= L'Z') || c == L'.' || c == L'_' || c == L'-';
	});
	if (!validHost) return false;

	if (port.empty() || !std::all_of(port.begin(), port.end(),
		[](wchar_t c) { return std::iswdigit(c) != 0; })) {
		return false;
	}
	unsigned long portNumber = std::wcstoul(port.c_str(), NULL, 10);
	if (portNumber == 0 || portNumber > 65535) return false;

	result.normalized = scheme + L"://" + value;
	result.host = host;
	result.hostPort = value;
	result.port = port;
	return true;
}

std::wstring BuildWindowsProxyServer(const std::wstring& httpProxy, const std::wstring& socks5Proxy) {
	ParsedProxyAddress http;
	ParsedProxyAddress socks;
	std::wstring value;
	if (ParseProxyAddress(httpProxy, false, http)) {
		value += L"http=" + http.hostPort + L";https=" + http.hostPort;
	}
	if (ParseProxyAddress(socks5Proxy, true, socks)) {
		if (!value.empty()) value += L';';
		value += L"socks=" + socks.hostPort;
	}
	return value;
}

std::wstring BuildNoProxy(const std::wstring& value) {
	std::wstring result;
	auto append = [&result](const std::wstring& item) {
		if (item.empty()) return;
		if (!result.empty()) result += L',';
		result += item;
	};
	size_t start = 0;
	while (start <= value.size()) {
		size_t end = value.find(L';', start);
		std::wstring item = TrimCopy(value.substr(start,
			end == std::wstring::npos ? std::wstring::npos : end - start));
		if (LowerCopy(item) == L"<local>") {
			append(L"localhost");
			append(L"127.0.0.1");
			append(L"::1");
		}
		else {
			append(item);
		}
		if (end == std::wstring::npos) break;
		start = end + 1;
	}
	return result;
}

void ParseWindowsProxyServer(const std::wstring& value,
	std::wstring& httpProxy, std::wstring& socks5Proxy) {
	httpProxy.clear();
	socks5Proxy.clear();
	size_t start = 0;
	while (start <= value.size()) {
		size_t end = value.find(L';', start);
		std::wstring item = TrimCopy(value.substr(start,
			end == std::wstring::npos ? std::wstring::npos : end - start));
		size_t equals = item.find(L'=');
		if (equals == std::wstring::npos) {
			ParsedProxyAddress parsed;
			if (!item.empty() && httpProxy.empty() && ParseProxyAddress(item, false, parsed)) {
				httpProxy = parsed.normalized;
			}
		}
		else {
			std::wstring protocol = LowerCopy(TrimCopy(item.substr(0, equals)));
			std::wstring address = item.substr(equals + 1);
			ParsedProxyAddress parsed;
			if ((protocol == L"http" || protocol == L"https") && httpProxy.empty() &&
				ParseProxyAddress(address, false, parsed)) {
				httpProxy = parsed.normalized;
			}
			else if (protocol == L"socks" && socks5Proxy.empty() &&
				ParseProxyAddress(address, true, parsed)) {
				socks5Proxy = parsed.normalized;
			}
		}
		if (end == std::wstring::npos) break;
		start = end + 1;
	}
}

bool ReadRegistryString(HKEY key, const wchar_t* name, std::wstring& value) {
	DWORD type = 0;
	DWORD size = 0;
	if (RegQueryValueExW(key, name, NULL, &type, NULL, &size) != ERROR_SUCCESS ||
		(type != REG_SZ && type != REG_EXPAND_SZ)) {
		return false;
	}
	std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
	if (RegQueryValueExW(key, name, NULL, &type,
		reinterpret_cast<LPBYTE>(buffer.data()), &size) != ERROR_SUCCESS) {
		return false;
	}
	value.assign(buffer.data());
	return true;
}
}
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	//// 初始化通用控件
	//INITCOMMONCONTROLSEX icex;
	//icex.dwSize = sizeof(icex);
	//icex.dwICC = ICC_WIN95_CLASSES;
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
	InitCommonControls();

	// 单实例：若已有一个实例在运行则直接退出，避免重复托盘图标
	// 互斥锁句柄交由 Proxied 管理（Shift+Exit 重启时需先释放再启动新实例）
	HANDLE hSingleInstance = CreateMutex(NULL, FALSE,
		_T("Proxied_SingleInstance_{9F2C1A3B-7E4D-4B8A-9C1E-2D5F6A8B0C3E}"));
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		if (hSingleInstance) {
			CloseHandle(hSingleInstance);
		}
		return 0;
	}

	Proxied app;
	app.SetSingleInstanceHandle(hSingleInstance);
	app.Run();

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
	settingsDialog_(NULL),
	hPopupMenu_(NULL),
	hEvent_(NULL),
	hResourceInstance_(NULL),
	hSingleInstance_(NULL){
	memset(&nid_, 0, sizeof(nid_));
	DetectLanguage();
}

void Proxied::DetectLanguage() {
	// 字符串/对话框资源以多语言形式编译进主模块（中性英文兜底 + 英文 + 简体中文），
	// LoadString 与 DialogBox 会按系统 UI 语言自动选择最匹配的版本，无需手动切换资源实例。
	hResourceInstance_ = GetModuleHandle(NULL);
}

std::wstring Proxied::LoadStringByLang(UINT id, LANGID lang) {
	// STRINGTABLE 块格式：资源名 = (id >> 4) + 1，块内偏移 = id & 0xF，
	// 每项一个 WORD 长度前缀（字符数）+ UTF-16 内容
	HRSRC hRes = FindResourceExW(hResourceInstance_, RT_STRING,
		MAKEINTRESOURCEW((id >> 4) + 1), lang);
	if (!hRes) {
		return L"";
	}
	HGLOBAL hData = LoadResource(hResourceInstance_, hRes);
	if (!hData) {
		return L"";
	}
	const WORD* p = static_cast<const WORD*>(LockResource(hData));
	if (!p) {
		return L"";
	}
	// 跳过目标字符串之前的项（每项 = 1 个长度前缀 + 内容）
	DWORD idx = id & 0xF;
	for (DWORD i = 0; i < idx; ++i) {
		p += 1 + *p;
	}
	WORD len = *p;
	return std::wstring(reinterpret_cast<const wchar_t*>(p + 1), len);
}

std::wstring Proxied::LoadLocalizedString(UINT id) {
	if (!hResourceInstance_) {
		return L"";
	}
	// LoadString 的语言回退在本机不可靠（中文系统仍返回英文兜底块），
	// 改为按明确优先级用 FindResourceExW 精确定位：
	// 线程 UI 语言 -> 用户 UI 语言 -> 系统 UI 语言 -> 英文 -> 中性
	LANGID langs[] = {
		GetThreadUILanguage(),
		GetUserDefaultUILanguage(),
		GetSystemDefaultUILanguage(),
		MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
	};
	for (LANGID lang : langs) {
		std::wstring s = LoadStringByLang(id, lang);
		if (!s.empty()) {
			return s;
		}
	}
	return L"";
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
	if (hSingleInstance_) {
		CloseHandle(hSingleInstance_);
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
	_tcscpy_s(nid_.szTip, ARRAYSIZE(nid_.szTip), LoadLocalizedString(IDS_PROCESSING).c_str());
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
	AppendMenu(hPopupMenu_, MF_STRING | 0, IDM_ENABLE, LoadLocalizedString(IDS_ENABLE_PROXY).c_str());
	AppendMenu(hPopupMenu_, MF_STRING | 0, IDM_DISABLE, LoadLocalizedString(IDS_DISABLE_PROXY).c_str());
	AppendMenu(hPopupMenu_, MF_STRING | 0, IDM_SETTINGS, LoadLocalizedString(IDS_PROXY_SETTINGS).c_str());
	AppendMenu(hPopupMenu_, MF_SEPARATOR, 0, NULL);
	AppendMenu(hPopupMenu_, MF_STRING | (autoStart_ ? MF_CHECKED : 0), IDM_AUTOSTART, LoadLocalizedString(IDS_AUTOSTART).c_str());
	AppendMenu(hPopupMenu_, MF_SEPARATOR, 0, NULL);
	AppendMenu(hPopupMenu_, MF_STRING | (gitProxyEnabled_ ? MF_CHECKED : 0), IDM_GIT_PROXY, LoadLocalizedString(IDS_GIT_PROXY_SYNC).c_str());
	AppendMenu(hPopupMenu_, MF_STRING | (gradleProxyEnabled_ ? MF_CHECKED : 0), IDM_GRADLE_PROXY, LoadLocalizedString(IDS_GRADLE_PROXY_SYNC).c_str());
	AppendMenu(hPopupMenu_, MF_STRING | (wslGitProxyEnabled_ ? MF_CHECKED : 0), IDM_WSL_GIT_PROXY, LoadLocalizedString(IDS_WSL_GIT_PROXY_SYNC).c_str());
	AppendMenu(hPopupMenu_, MF_SEPARATOR, 0, NULL);
	std::wstring versionText = LoadLocalizedString(IDS_CURRENT_VERSION) + PROXIED_VERSION;
	AppendMenu(hPopupMenu_, MF_STRING, 0, versionText.c_str());
	AppendMenu(hPopupMenu_, MF_SEPARATOR, 0, NULL);
	AppendMenu(hPopupMenu_, MF_STRING, IDM_GITHUB,LoadLocalizedString(IDS_ABOUT).c_str());
	AppendMenu(hPopupMenu_, MF_STRING, IDM_EXIT, LoadLocalizedString(IDS_EXIT).c_str());

	// 初始化托盘图标
	nid_.cbSize = sizeof(nid_);
	nid_.hWnd = hWnd_;
	nid_.uID = 1;
	nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid_.uCallbackMessage = WM_TRAYICON;
	nid_.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_SMALL));
	_tcscpy_s(nid_.szTip, sizeof(nid_.szTip) / sizeof(TCHAR),
		LoadLocalizedString(IDS_LOADING).c_str());

	Shell_NotifyIcon(NIM_ADD, &nid_);
}

HBITMAP Proxied::CreateSectionIconBitmap() {
	// 加载程序图标（16x16）画到白色背景位图：白色 = 菜单透明色，图标彩色显示
	HICON hIcon = static_cast<HICON>(LoadImage(hResourceInstance_,
		MAKEINTRESOURCE(IDI_PROXIED), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
	if (!hIcon) {
		return NULL;
	}
	HDC hdcScreen = GetDC(NULL);
	HDC hdc = CreateCompatibleDC(hdcScreen);
	HBITMAP hBmp = CreateCompatibleBitmap(hdcScreen, 16, 16);
	if (hdc && hBmp) {
		HBITMAP hOld = static_cast<HBITMAP>(SelectObject(hdc, hBmp));
		RECT rc = { 0, 0, 16, 16 };
		FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
		DrawIconEx(hdc, 0, 0, hIcon, 16, 16, 0, NULL, DI_NORMAL);
		SelectObject(hdc, hOld);
	}
	if (hdc) {
		DeleteDC(hdc);
	}
	ReleaseDC(NULL, hdcScreen);
	DestroyIcon(hIcon);
	return hBmp;
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
	DWORD proxyEnabled = 0;
	HKEY hKey = nullptr;
	proxyServer_.clear();
	httpProxy_.clear();
	socks5Proxy_.clear();
	nonProxyHosts_.clear();

	if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		DWORD type = 0;
		DWORD size = sizeof(DWORD);
		RegQueryValueEx(hKey, _T("ProxyEnable"), NULL, &type,
			reinterpret_cast<LPBYTE>(&proxyEnabled), &size);
		if (type != REG_DWORD) proxyEnabled = 0;

		ReadRegistryString(hKey, L"ProxyServer", proxyServer_);
		ReadRegistryString(hKey, L"ProxyOverride", nonProxyHosts_);
		RegCloseKey(hKey);
	}

	ParseWindowsProxyServer(proxyServer_, httpProxy_, socks5Proxy_);
	return proxyEnabled != 0;
}
struct ProxySettingsDialogContext {
	Proxied* self;
	std::wstring* httpProxy;
	std::wstring* socks5Proxy;
};

bool Proxied::ShowProxySettings(std::wstring& httpProxy, std::wstring& socks5Proxy) {
	if (settingsDialog_ && IsWindow(settingsDialog_)) {
		BringWindowToTop(settingsDialog_);
		SetForegroundWindow(settingsDialog_);
		SetActiveWindow(settingsDialog_);
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(applyMutex_);
		GetProxySettings();
		httpProxy = httpProxy_;
		socks5Proxy = socks5Proxy_;
	}
	ProxySettingsDialogContext context{ this, &httpProxy, &socks5Proxy };
	INT_PTR result = DialogBoxParamW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDD_PROXY_SETTINGS),
		hWnd_, &Proxied::ProxySettingsDlgProc, reinterpret_cast<LPARAM>(&context));
	settingsDialog_ = NULL;
	return result == 1;
}

INT_PTR CALLBACK Proxied::ProxySettingsDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
	ProxySettingsDialogContext* context =
		reinterpret_cast<ProxySettingsDialogContext*>(GetWindowLongPtr(hDlg, DWLP_USER));
	switch (message) {
	case WM_INITDIALOG: {
		context = reinterpret_cast<ProxySettingsDialogContext*>(lParam);
		SetWindowLongPtr(hDlg, DWLP_USER, lParam);
		Proxied* self = context->self;
		self->settingsDialog_ = hDlg;
		SetWindowText(hDlg, self->LoadLocalizedString(IDS_PROXY_SETTINGS).c_str());
		SetDlgItemText(hDlg, IDC_HTTP_PROXY_LABEL, self->LoadLocalizedString(IDS_HTTP_PROXY).c_str());
		SetDlgItemText(hDlg, IDC_SOCKS5_PROXY_LABEL, self->LoadLocalizedString(IDS_SOCKS5_PROXY).c_str());
		SetDlgItemText(hDlg, IDOK, self->LoadLocalizedString(IDS_SAVE).c_str());
		SetDlgItemText(hDlg, IDCANCEL, self->LoadLocalizedString(IDS_CANCEL).c_str());
		SetDlgItemText(hDlg, IDC_HTTP_PROXY_EDIT, context->httpProxy->c_str());
		SetDlgItemText(hDlg, IDC_SOCKS5_PROXY_EDIT, context->socks5Proxy->c_str());
		SendDlgItemMessage(hDlg, IDC_HTTP_PROXY_EDIT, EM_LIMITTEXT, 2047, 0);
		SendDlgItemMessage(hDlg, IDC_SOCKS5_PROXY_EDIT, EM_LIMITTEXT, 2047, 0);
		self->UpdateSettingsDialogIcon();

		RECT dialogRect;
		GetWindowRect(hDlg, &dialogRect);
		int width = dialogRect.right - dialogRect.left;
		int height = dialogRect.bottom - dialogRect.top;
		SetWindowPos(hDlg, NULL, (GetSystemMetrics(SM_CXSCREEN) - width) / 2,
			(GetSystemMetrics(SM_CYSCREEN) - height) / 2, 0, 0,
			SWP_NOSIZE | SWP_NOZORDER);
		return TRUE;
	}
	case WM_COMMAND:
		if (LOWORD(wParam) == IDCANCEL) {
			EndDialog(hDlg, 0);
			return TRUE;
		}
		if (LOWORD(wParam) == IDOK) {
			wchar_t httpBuffer[2048] = { 0 };
			wchar_t socksBuffer[2048] = { 0 };
			GetDlgItemText(hDlg, IDC_HTTP_PROXY_EDIT, httpBuffer, ARRAYSIZE(httpBuffer));
			GetDlgItemText(hDlg, IDC_SOCKS5_PROXY_EDIT, socksBuffer, ARRAYSIZE(socksBuffer));
			std::wstring httpInput = TrimCopy(httpBuffer);
			std::wstring socksInput = TrimCopy(socksBuffer);
			ParsedProxyAddress http;
			ParsedProxyAddress socks;
			bool valid = (!httpInput.empty() || !socksInput.empty()) &&
				(httpInput.empty() || ParseProxyAddress(httpInput, false, http)) &&
				(socksInput.empty() || ParseProxyAddress(socksInput, true, socks));
			if (!valid) {
				MessageBox(hDlg, context->self->LoadLocalizedString(IDS_INVALID_PROXY).c_str(),
					context->self->LoadLocalizedString(IDS_APP_TITLE).c_str(), MB_OK | MB_ICONWARNING);
				return TRUE;
			}
			*context->httpProxy = httpInput.empty() ? L"" : http.normalized;
			*context->socks5Proxy = socksInput.empty() ? L"" : socks.normalized;
			EndDialog(hDlg, 1);
			return TRUE;
		}
		break;
	case WM_CLOSE:
		EndDialog(hDlg, 0);
		return TRUE;
	}
	return FALSE;
}

void Proxied::UpdateSettingsDialogIcon() {
	if (!settingsDialog_ || !IsWindow(settingsDialog_)) return;
	HICON icon = LoadIcon(GetModuleHandle(NULL),
		MAKEINTRESOURCE(proxyEnabled_ ? IDI_SMALL2 : IDI_SMALL));
	SendMessage(settingsDialog_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
	SendMessage(settingsDialog_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
}

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
	std::wifstream inFile(configPath.c_str());
	std::vector<std::wstring> lines;
	bool changed = false;
	bool fileExists = inFile.good();

	// 读取现有配置并过滤所有代理相关设置
	if (fileExists) {
		std::wstring line;
		while (std::getline(inFile, line)) {
			const wchar_t* proxyKeys[] = {
				L"systemProp.http.proxyHost=", L"systemProp.http.proxyPort=",
				L"systemProp.https.proxyHost=", L"systemProp.https.proxyPort=",
				L"systemProp.http.nonProxyHosts=", L"systemProp.https.nonProxyHosts=",
				L"systemProp.socksProxyHost=", L"systemProp.socksProxyPort="
			};
			bool isProxySetting = std::any_of(std::begin(proxyKeys), std::end(proxyKeys),
				[&line](const wchar_t* key) { return line.find(key) == 0; });
			if (isProxySetting) {
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

	// 与 Git 保持一致：优先 HTTP，仅未配置 HTTP 时使用 SOCKS，避免双重代理。
	if (enable) {
		ParsedProxyAddress http;
		ParsedProxyAddress socks;
		bool hasHttp = ParseProxyAddress(httpProxy_, false, http);
		bool hasSocks = !hasHttp && ParseProxyAddress(socks5Proxy_, true, socks);
		if (hasHttp) {
			lines.push_back(L"systemProp.http.proxyHost=" + http.host);
			lines.push_back(L"systemProp.http.proxyPort=" + http.port);
			lines.push_back(L"systemProp.https.proxyHost=" + http.host);
			lines.push_back(L"systemProp.https.proxyPort=" + http.port);

			if (!nonProxyHosts_.empty()) {
				// 转换Windows格式(分号分隔)为Gradle格式(竖线分隔)
				std::wstring gradleNonProxy = nonProxyHosts_;
				std::replace(gradleNonProxy.begin(), gradleNonProxy.end(), L';', L'|');
				lines.push_back(_T("systemProp.http.nonProxyHosts=") + gradleNonProxy);
				lines.push_back(_T("systemProp.https.nonProxyHosts=") + gradleNonProxy);
			}
		}
		if (hasSocks) {
			lines.push_back(L"systemProp.socksProxyHost=" + socks.host);
			lines.push_back(L"systemProp.socksProxyPort=" + socks.port);
		}
		changed = changed || hasHttp || hasSocks;
	}

	// 写入文件
	bool result = false;
	if (changed) {
		// 确保目录存在
		std::wstring dirPath = configPath.substr(0, configPath.find_last_of(L'\\'));
		SHCreateDirectoryEx(NULL, dirPath.c_str(), NULL);
		
		std::wofstream outFile(configPath.c_str());
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
    // Git 只有一个通用 HTTP 代理入口：优先 HTTP，未配置时回退到 SOCKS5。
    std::wstring command;
    std::wstring gitProxy = !httpProxy_.empty() ? httpProxy_ : socks5Proxy_;
    if (enable && !gitProxy.empty()) {
        command = L"git config --global http.proxy \"" + gitProxy + L"\" && git config --global https.proxy \"" + gitProxy + L"\"";
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
    std::wstring gitProxy = !httpProxy_.empty() ? httpProxy_ : socks5Proxy_;
    if (enable && !gitProxy.empty()) {
        // 在 WSL 里设置 git 的全局代理
        command = L"wsl.exe -e sh -c \"git config --global http.proxy '" + gitProxy +
            L"' && git config --global https.proxy '" + gitProxy + L"'\"";
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
    // 设置窗口句柄只由 UI 线程访问，通过消息刷新图标。
    PostMessage(hWnd_, WM_SETTINGS_PROXY_STATE, 0, 0);

    if (proxyEnabled) {
        const std::wstring& envProxy = !httpProxy_.empty() ? httpProxy_ : socks5Proxy_;
        std::wstring noProxy = BuildNoProxy(nonProxyHosts_);
        UpdateUserEnvironmentVariable(_T("http_proxy"), envProxy.empty() ? nullptr : &envProxy);
        UpdateUserEnvironmentVariable(_T("https_proxy"), envProxy.empty() ? nullptr : &envProxy);
        UpdateUserEnvironmentVariable(_T("all_proxy"), socks5Proxy_.empty() ? nullptr : &socks5Proxy_);
        UpdateUserEnvironmentVariable(_T("no_proxy"), noProxy.empty() ? nullptr : &noProxy);
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
        UpdateUserEnvironmentVariable(_T("all_proxy"), nullptr);
        UpdateUserEnvironmentVariable(_T("no_proxy"), nullptr);
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
		proxyEnabled ? LoadLocalizedString(IDS_PROXY_ENABLED).c_str() : LoadLocalizedString(IDS_PROXY_DISABLED).c_str());
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
    case IDM_SETTINGS: {
        HKEY hKey;
        ok = false;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            std::wstring server = BuildWindowsProxyServer(
                self->pendingHttpProxy_, self->pendingSocks5Proxy_);
            ok = !server.empty() && RegSetValueExW(hKey, L"ProxyServer", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(server.c_str()),
                static_cast<DWORD>((server.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
            RegCloseKey(hKey);
        }
        if (ok) self->ApplyChanges();
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
			if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
				// 按住 SHIFT：重启程序。先释放单实例锁并移除托盘图标，
				// 再启动新实例，避免新进程因互斥锁未释放而直接退出
				if (pThis->hSingleInstance_) {
					CloseHandle(pThis->hSingleInstance_);
					pThis->hSingleInstance_ = NULL;
				}
				Shell_NotifyIcon(NIM_DELETE, &pThis->nid_);
				wchar_t exePath[MAX_PATH];
				GetModuleFileNameW(NULL, exePath, MAX_PATH);
				ShellExecuteW(NULL, L"open", exePath, NULL, NULL, SW_SHOWNORMAL);
				PostQuitMessage(0);
			}
			else {
				Shell_NotifyIcon(NIM_DELETE, &pThis->nid_);
				PostQuitMessage(0);
			}
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
		case IDM_SETTINGS: {
			// 设置窗口在 UI 线程显示，实际写注册表与同步仍在后台线程执行。
			int operationId = LOWORD(wParam);
			if (pThis->opBusy_) {
				break;
			}
			if (operationId == IDM_SETTINGS &&
				!pThis->ShowProxySettings(pThis->pendingHttpProxy_, pThis->pendingSocks5Proxy_)) {
				break;
			}
			pThis->opBusy_ = true;
			// 启动旋转图标计时器，立即显示第一帧
			SetTimer(pThis->hWnd_, BUSY_TIMER_ID, BUSY_INTERVAL, NULL);
			pThis->busyFrame_ = 0;
			_tcscpy_s(pThis->nid_.szTip, ARRAYSIZE(pThis->nid_.szTip), pThis->LoadLocalizedString(IDS_PROCESSING).c_str());
			pThis->SetTrayIcon(pThis->MakeBusyIcon(0));
			{
				OpCtx* ctx = new OpCtx{ pThis, operationId, GetTickCount() };
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
					MessageBox(hWnd, pThis->LoadLocalizedString(IDS_CANNOT_START_TASK).c_str(),
						pThis->LoadLocalizedString(IDS_APP_TITLE).c_str(), MB_OK | MB_ICONWARNING);
				}
			}
			break;
		}
		}
		break;
	}
	case WM_SETTINGS_PROXY_STATE:
		pThis->UpdateSettingsDialogIcon();
		break;
	case WM_PROXY_OP_DONE: {
		int opId = static_cast<int>(wParam);
		bool ok = (lParam != 0);
		pThis->opBusy_ = false;
		// 停止旋转并恢复普通图标
		KillTimer(hWnd, BUSY_TIMER_ID);
		pThis->SetTrayIcon(LoadIcon(GetModuleHandle(NULL),
			MAKEINTRESOURCE(pThis->proxyEnabled_ ? IDI_SMALL2 : IDI_SMALL)));
		_tcscpy_s(pThis->nid_.szTip, ARRAYSIZE(pThis->nid_.szTip),
			pThis->LoadLocalizedString(pThis->proxyEnabled_ ? IDS_PROXY_ENABLED : IDS_PROXY_DISABLED).c_str());
		Shell_NotifyIcon(NIM_MODIFY, &pThis->nid_);
		if (opId == IDM_SETTINGS && !ok) {
			MessageBox(hWnd, pThis->LoadLocalizedString(IDS_PROXY_SAVE_ERROR).c_str(),
				pThis->LoadLocalizedString(IDS_APP_TITLE).c_str(), MB_OK | MB_ICONERROR);
		}
		if (opId == IDM_WSL_GIT_PROXY) {
			if (ok) {
				CheckMenuItem(pThis->hPopupMenu_, IDM_WSL_GIT_PROXY,
					pThis->wslGitProxyEnabled_ ? MF_CHECKED : MF_UNCHECKED);
			}
			else {
				MessageBox(hWnd, pThis->LoadLocalizedString(IDS_WSL_GIT_ERROR).c_str(),
					pThis->LoadLocalizedString(IDS_APP_TITLE).c_str(), MB_OK | MB_ICONWARNING);
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


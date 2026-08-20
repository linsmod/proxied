# proxied

一个驻留在系统托盘的小程序。打开它，你就可以从任务栏直接控制系统级别的代理开关，它和 Windows 设置是**双向同步**的——无论开关是从 proxied 发出，还是从 Windows 的网络设置页那里。

不止 Windows。它还同步你开发环境里那些"固执地不听系统代理的话"的程序，让你**在一个地方控制所有开发环境的代理**。

![image](https://github.com/user-attachments/assets/20e4a404-3b10-4193-b15e-a44d607673c8)

## 功能

- **系统级代理开关**：启用/禁用 Windows 系统代理，实时同步 Windows 设置页（双向，通过监听注册表实现）
- **代理分组**：内置多组代理配置，托盘菜单一键切换，不用每次去设置页改地址
- **同步 Git 代理**：启用/禁用时同步写入 `git config --global http.proxy/https.proxy`
- **同步 Gradle 代理**：启用/禁用时同步写入 `~/.gradle/gradle.properties`
- **同步 WSL 内 Git 代理**：启用/禁用时通过 `wsl.exe` 同步写入 WSL 内的 git 全局代理配置
- **同步 `http_proxy` 环境变量**：一并写入 `HKCU\Environment`，照顾那些读环境变量的程序
- **开机自启**：可选随 Windows 启动

以上三项"同步 Git / Gradle / WSL Git"都是独立的开关（托盘菜单里勾选），默认全部开启，按需关掉某个就行。

## 原理

Windows 自家程序大多乖乖用系统级代理设置，但有些开发工具不是：

- **Gradle** 固执地使用 `gradle.properties` 文件配置网络代理，而不是自动跟随系统代理。
- **WSL** 是一个独立的 Linux 环境，完全不认识 Windows 的系统代理，里面的 git 只认自己的 `~/.gitconfig`。

我无法忍受它们如此的固执，因此我创造了 proxied。它把 Windows 的代理设置实时同步到：

| 目标 | 位置 |
| --- | --- |
| Windows 系统代理 | `HKCU\...\Internet Settings`（ProxyEnable / ProxyServer） |
| Git（Windows） | `git config --global http.proxy / https.proxy` |
| Gradle | `~/.gradle/gradle.properties`（systemProp.http.* / systemProp.https.*） |
| WSL 内 Git | WSL 内 `git config --global http.proxy / https.proxy` |
| 环境变量 | `HKCU\Environment` 的 `http_proxy` / `https_proxy` |

它同时也写入 `http_proxy` 环境变量——我开始以为 Gradle 会读取它，然而并没有。或许某个其他软件会使用这个变量，就像在 Ubuntu 上面一样？我不知道，但仍然保留了这部分代码和功能。

程序自身开关状态存放在 `HKCU\Software\Proxied`。

## 构建

项目为 Visual Studio 工程（x64），编译命令：

```powershell
MSBuild proxied\proxied.vcxproj /p:Configuration=Release /p:Platform=x64
```

产物在 `proxied\x64\Release\proxied.exe`。

## 依赖

- Windows 10/11（含 WSL2，可选）
- 若要使用 WSL 内 Git 同步，需安装 WSL 发行版并内置 `git`

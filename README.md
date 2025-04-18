打开proxied，你就可以从任务栏直接控制系统级别的代理开关，它和Windows设置是双向同步的。
你同时也掌控gradle的代理配置，无论开关是从proxied发出，还是从Windows的网络设置页那里。

## 原理
gradle固执的使用gradle.properties文件配置网络代理。
而不是和其他软件一样自动使用系统级别的网络代理设置，
我无法忍受它如此的固执，因此我创造了proxied。
它会把windows的代理设置实时同步到gradle.properties文件中，
当然它同时也写入http_proxy环境变量, 我开始以为以为gradle会读取它，然而并没有。
或许某个其他软件会使用这个变量，就像在ubuntu上面一样？我不知道，但仍然保留了这部分代码和功能。

![image](https://github.com/user-attachments/assets/20e4a404-3b10-4193-b15e-a44d607673c8)

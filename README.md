## 原理
gradle默认会读取环境变量中的代理设置。
如果需要经常管理代理，这在windows上面就会有些不自然。
于是我创造了proxied，它会把windows的代理设置实时同步到环境变量。
而且你还可以从任务栏直接控制代理开关。
![image](https://github.com/user-attachments/assets/20e4a404-3b10-4193-b15e-a44d607673c8)

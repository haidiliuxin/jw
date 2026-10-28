# 实验 01：流式套接字聊天程序

本目录包含 Winsock 多线程聊天室源码和 Windows 可执行程序。核心网络代码直接调用 Winsock API，不依赖 CSocket、Boost.Asio 等封装套接字库。

## 文件

- `server.cpp`：服务端源码
- `client.cpp`：客户端源码
- `chat_common.hpp`：自编写的发送完整数据和 LF 帧解析代码
- `server.exe` / `client.exe`：已构建的 Windows x64 可执行程序
- `实验报告.md`：实验说明与测试记录
- `tests/integration_test.py`：真实 TCP 集成测试

## 运行

在一个终端：

```powershell
.\server.exe 54000
```

在另两个终端分别启动客户端并输入不同用户名：

```powershell
.\client.exe 127.0.0.1 54000
```

直接输入文字发送群聊；`/who` 查看在线列表，`/help` 查看帮助，`/quit` 退出。服务端输入 `/quit` 关闭。

## 从源码构建

在安装了 Visual Studio C++ 工具链的开发者 PowerShell 中执行：

```powershell
cl /nologo /EHsc /std:c++17 /utf-8 /W4 server.cpp ws2_32.lib /Fe:server.exe
cl /nologo /EHsc /std:c++17 /utf-8 /W4 client.cpp ws2_32.lib /Fe:client.exe
```

仓库按要求未包含构建脚本。测试需 Python 3：`python tests\integration_test.py`。

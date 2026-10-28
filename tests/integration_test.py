import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "server.exe"
MAX_WAIT = 5


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def recv_line(sock, timeout=MAX_WAIT):
    sock.settimeout(timeout)
    data = bytearray()
    while True:
        chunk = sock.recv(1)
        if not chunk:
            raise AssertionError("服务端提前关闭连接")
        if chunk == b"\n":
            return data.rstrip(b"\r").decode("utf-8")
        data.extend(chunk)


def expect_prefix(sock, prefix):
    line = recv_line(sock)
    assert line.startswith(prefix), f"期望 {prefix!r}，收到 {line!r}"
    return line


def join(port, username):
    sock = socket.create_connection(("127.0.0.1", port), timeout=MAX_WAIT)
    sock.sendall((f"JOIN {username}\n").encode("utf-8"))
    expect_prefix(sock, "OK ")
    return sock


def main():
    if not SERVER.is_file():
        raise SystemExit("找不到 server.exe；请先在 Visual Studio 开发者 PowerShell 中运行 .\\build.ps1")
    port = free_port()
    proc = subprocess.Popen([str(SERVER), str(port)], cwd=ROOT,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                            bufsize=1)
    clients = []
    try:
        deadline = time.time() + MAX_WAIT
        while time.time() < deadline:
            try:
                a = join(port, "甲")
                clients.append(a)
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise AssertionError("服务端未能启动")
        expect_prefix(a, "SYS 甲 加入")
        b = join(port, "乙")
        clients.append(b)
        expect_prefix(a, "SYS 乙 加入")
        expect_prefix(b, "SYS 乙 加入")

        # 分片发送一帧；接收端不得把 recv 边界当作消息边界。
        frame = "MSG 你好，群聊世界！\n".encode("utf-8")
        for byte in frame:
            a.sendall(bytes([byte]))
        expect_prefix(a, "MSG 甲 你好，群聊世界！")
        expect_prefix(b, "MSG 甲 你好，群聊世界！")

        a.sendall(b"WHO\n")
        users = expect_prefix(a, "USERS ")
        assert "甲" in users and "乙" in users, users

        dup = socket.create_connection(("127.0.0.1", port), timeout=MAX_WAIT)
        clients.append(dup)
        dup.sendall("JOIN 甲\n".encode("utf-8"))
        expect_prefix(dup, "ERR ")
        print("PASS: 加入、中文广播、分片组帧、在线列表、重名拒绝")
    finally:
        for client in clients:
            try:
                client.close()
            except OSError:
                pass
        if proc.poll() is None:
            try:
                proc.stdin.write("/quit\n")
                proc.stdin.flush()
                proc.wait(timeout=MAX_WAIT)
                assert proc.returncode == 0, f"服务端非正常退出，退出码 {proc.returncode}"
            except Exception:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    main()


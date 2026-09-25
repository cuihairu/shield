#!/usr/bin/env python3
"""client_demo.py - Shield 最小客户端示例（idlen 封包 + JSON body）。

帧格式：[route_id:2B 大端][length:2B 大端][body]。
连接后发送一条 c2s 请求，打印收到的第一条 s2c 回包。

用法：
    # 默认运行时 echo 服务（config/app.yaml，端口 7900）
    python3 scripts/client_demo.py

    # 模板工程 / hello_world（端口 8001）
    python3 scripts/client_demo.py --port 8001 --message '{"player_id":"p1"}'

仅用标准库，可直接拷进你自己的客户端工程当协议参考。
"""

import argparse
import json
import socket
import struct
import sys


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Send one idlen-framed JSON request and print the reply.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7900)
    parser.add_argument("--route", type=int, default=1,
                        help="c2s route id (echo=1, hello_world login=1)")
    parser.add_argument("--message", default='{"hello": "shield"}',
                        help="JSON body to send")
    parser.add_argument("--timeout", type=float, default=5.0)
    args = parser.parse_args()

    body = json.loads(args.message)
    payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
    frame = struct.pack(">HH", args.route, len(payload)) + payload

    try:
        with socket.create_connection((args.host, args.port),
                                      timeout=args.timeout) as sock:
            sock.sendall(frame)
            reply = read_frame(sock, args.timeout)
    except (OSError, TimeoutError) as exc:
        print(f"connect/send failed: {exc}", file=sys.stderr)
        return 1

    if reply is None:
        print("no reply frame received", file=sys.stderr)
        return 1

    route_id, reply_body = reply
    print(f"reply route={route_id} body={reply_body}")
    return 0


def read_frame(sock: socket.socket, timeout: float):
    """读取一条 idlen 帧，返回 (route_id, body 字符串) 或 None。"""
    sock.settimeout(timeout)
    header = recv_exact(sock, 4)
    if header is None:
        return None
    route_id, length = struct.unpack(">HH", header)
    body = recv_exact(sock, length)
    if body is None:
        return None
    return route_id, body.decode("utf-8", errors="replace")


def recv_exact(sock: socket.socket, count: int):
    buf = b""
    while len(buf) < count:
        chunk = sock.recv(count - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


if __name__ == "__main__":
    sys.exit(main())

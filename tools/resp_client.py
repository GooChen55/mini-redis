"""Minimal binary-safe RESP2 client; standard library only."""
import socket


def encode(*args):
    parts = [f"*{len(args)}\r\n".encode()]
    for arg in args:
        data = arg if isinstance(arg, bytes) else str(arg).encode()
        parts.extend((f"${len(data)}\r\n".encode(), data, b"\r\n"))
    return b"".join(parts)


class RedisError(Exception):
    pass


class Client:
    def __init__(self, host="127.0.0.1", port=6379, timeout=10):
        self.socket = socket.create_connection((host, port), timeout=timeout)
        self.file = self.socket.makefile("rb")

    def read(self):
        line = self.file.readline()
        if not line or not line.endswith(b"\r\n"):
            raise ConnectionError("incomplete RESP reply")
        kind, payload = line[:1], line[1:-2]
        if kind == b"+":
            return payload
        if kind == b"-":
            raise RedisError(payload.decode(errors="replace"))
        if kind == b":":
            return int(payload)
        if kind == b"$":
            length = int(payload)
            if length == -1:
                return None
            data = self.file.read(length)
            if len(data) != length or self.file.read(2) != b"\r\n":
                raise ConnectionError("incomplete bulk reply")
            return data
        if kind == b"*":
            return [self.read() for _ in range(int(payload))]
        raise ValueError(f"unknown RESP reply {line!r}")

    def command(self, *args):
        self.socket.sendall(encode(*args))
        return self.read()

    def pipeline(self, commands):
        self.socket.sendall(b"".join(encode(*args) for args in commands))
        return [self.read() for _ in commands]

    def close(self):
        self.file.close()
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

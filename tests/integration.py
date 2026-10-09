import concurrent.futures
import pathlib
import signal
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from resp_client import Client, RedisError, encode

BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())


class Integration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.aof = pathlib.Path(self.temp.name) / "data.aof"
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]
        self.process = None
        self.start()

    def start(self):
        self.log = open(pathlib.Path(self.temp.name) / "server.log", "ab")
        self.process = subprocess.Popen(
            [BINARY, "--port", str(self.port), "--aof", str(self.aof), "--threads", "4"],
            stdout=self.log, stderr=self.log)
        for _ in range(200):
            if self.process.poll() is not None:
                raise RuntimeError((pathlib.Path(self.temp.name) / "server.log").read_text())
            try:
                with self.client() as client:
                    if client.command("PING") == b"PONG":
                        return
            except OSError:
                time.sleep(0.01)
        raise TimeoutError("server did not start")

    def stop(self, crash=False):
        if self.process is not None:
            self.process.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
            self.process.wait(timeout=10)
            if not crash:
                self.assertEqual(self.process.returncode, 0)
            self.process = None
            self.log.close()

    def tearDown(self):
        self.stop()
        self.temp.cleanup()

    def client(self):
        return Client(port=self.port, timeout=30)

    def test_commands_and_errors(self):
        with self.client() as c:
            self.assertEqual(c.command("ping", "hello"), b"hello")
            self.assertEqual(c.command("ECHO", b"\x00\xff\r\n"), b"\x00\xff\r\n")
            self.assertEqual(c.command("GET", "missing"), None)
            self.assertEqual(c.command("SET", "key", "value"), b"OK")
            self.assertEqual(c.command("MGET", "key", "missing"), [b"value", None])
            self.assertEqual(c.command("EXISTS", "key", "key"), 2)
            self.assertEqual(c.command("TTL", "key"), -1)
            self.assertEqual(c.command("DBSIZE"), 1)
            self.assertEqual(c.command("del", "key", "key"), 1)
            self.assertEqual(c.command("INCR", "n"), 1)
            self.assertEqual(c.command("DECR", "n"), 0)
            for args in [("GET",), ("NOPE",), ("SET", "x", "y", "EX", "0"),
                         ("SET", "x", "y", "NX"), ("EXPIRE", "n", "bad")]:
                with self.assertRaises(RedisError):
                    c.command(*args)
            c.command("SET", "n", "9223372036854775807")
            with self.assertRaises(RedisError):
                c.command("INCR", "n")
            c.command("SET", "n", "text")
            with self.assertRaises(RedisError):
                c.command("DECR", "n")

    def test_pipeline_fragmentation_and_half_close(self):
        with self.client() as c:
            commands = [("SET", "n", "0")] + [("INCR", "n")] * 500 + [("GET", "n")]
            self.assertEqual(c.pipeline(commands), [b"OK"] + list(range(1, 501)) + [b"500"])
            packet = encode("SET", b"binary\x00key", b"a\r\nb\x00c")
            for byte in packet:
                c.socket.sendall(bytes([byte]))
            self.assertEqual(c.read(), b"OK")
            self.assertEqual(c.command("GET", b"binary\x00key"), b"a\r\nb\x00c")
            c.socket.sendall(encode("PING") * 300)
            c.socket.shutdown(socket.SHUT_WR)
            self.assertEqual([c.read() for _ in range(300)], [b"PONG"] * 300)
            self.assertEqual(c.file.read(1), b"")

    def test_protocol_errors_and_quit(self):
        with self.client() as c:
            c.socket.sendall(b"pInG\r\n" + encode("PING"))
            self.assertEqual(c.read(), b"PONG")
            self.assertEqual(c.read(), b"PONG")
        for bad in [b"hello\r\n", b"*0\r\n", b"*1\r\n$-1\r\n", b"*1\r\n$999999999\r\n"]:
            with self.client() as c:
                c.socket.sendall(encode("PING") + bad)
                self.assertEqual(c.read(), b"PONG")
                with self.assertRaises(RedisError):
                    c.read()
                self.assertEqual(c.file.read(1), b"")
        with self.client() as c:
            c.socket.sendall(b"*2\r\n$3\r\nGET\r\n")
            c.socket.shutdown(socket.SHUT_WR)
            with self.assertRaises(RedisError):
                c.read()
        with self.client() as c:
            c.socket.sendall(encode("qUiT") + encode("SET", "ignored", "yes"))
            self.assertEqual(c.read(), b"OK")
            self.assertEqual(c.file.read(1), b"")
        with self.client() as c:
            self.assertIsNone(c.command("GET", "ignored"))

    def test_atomic_concurrent_increment(self):
        def worker(_):
            with self.client() as c:
                return c.pipeline([("INCR", "counter")] * 200)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(worker, range(8)))
        self.assertEqual(sorted(n for values in results for n in values), list(range(1, 1601)))
        with self.client() as c:
            self.assertEqual(c.command("GET", "counter"), b"1600")

    def test_aof_exclusive_lock(self):
        result = subprocess.run([BINARY, "--port", str(self.port), "--aof", str(self.aof)], capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"AOF is already in use", result.stderr)
        with self.client() as c:
            self.assertEqual(c.command("PING"), b"PONG")

    @unittest.skipUnless(shutil.which("redis-cli"), "redis-cli is not installed")
    def test_redis_cli_compatibility(self):
        for command, expected in [(["PING"], b"PONG\n"), (["SET", "cli", "works"], b"OK\n"),
                                  (["GET", "cli"], b"works\n"), (["INCR", "cli-count"], b"1\n")]:
            result = subprocess.run(["redis-cli", "-p", str(self.port), "--raw"] + command,
                                    capture_output=True, check=True, timeout=5)
            self.assertEqual(result.stdout, expected)

    def test_expiration_and_restart(self):
        with self.client() as c:
            c.command("SET", "lasting", "persist")
            c.command("SET", "short", "1", "PX", "150")
            self.assertTrue(0 <= c.command("PTTL", "short") <= 150)
            c.command("INCR", "short")
            self.assertTrue(0 <= c.command("PTTL", "short") <= 150)
            c.command("SET", "deleted", "x")
            c.command("del", "deleted")
            c.command("SET", "immediate", "x")
            self.assertEqual(c.command("EXPIRE", "immediate", -1), 1)
        self.stop(crash=True)
        time.sleep(0.2)
        self.start()
        with self.client() as c:
            self.assertEqual(c.command("GET", "lasting"), b"persist")
            self.assertIsNone(c.command("GET", "short"))
            self.assertIsNone(c.command("GET", "deleted"))
            self.assertIsNone(c.command("GET", "immediate"))
            c.command("SET", "reset", "x", "PX", "30")
            c.command("SET", "reset", "y")
            time.sleep(0.05)
            self.assertEqual(c.command("GET", "reset"), b"y")
            self.assertEqual(c.command("TTL", "reset"), -1)

    def test_partial_aof_tail_and_corruption(self):
        with self.client() as c:
            c.command("SET", "safe", "value")
        self.stop()
        original = self.aof.read_bytes()
        with self.aof.open("ab") as f:
            f.write(b"*3\r\n$3\r\nSET\r\n$4\r\nlost\r\n$10\r\npar")
        self.start()
        self.assertEqual(self.aof.read_bytes(), original)
        with self.client() as c:
            self.assertEqual(c.command("GET", "safe"), b"value")
            c.command("SET", "after", "repair")
        self.stop()
        with self.aof.open("ab") as f:
            f.write(b"!corrupt\r\n")
        result = subprocess.run([BINARY, "--port", str(self.port), "--aof", str(self.aof)], capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"corrupt AOF", result.stderr)

    def test_large_value_slow_reader_and_disconnects(self):
        value = b"x" * (2 * 1024 * 1024)
        with self.client() as c:
            self.assertEqual(c.command("SET", "large", value), b"OK")
            c.socket.sendall(encode("GET", "large") * 6)
            time.sleep(0.05)
            self.assertEqual([c.read() for _ in range(6)], [value] * 6)
        for _ in range(40):
            with self.client() as c:
                c.socket.sendall(encode("GET", "large"))
        with self.client() as c:
            self.assertEqual(c.command("PING"), b"PONG")


if __name__ == "__main__":
    unittest.main(verbosity=2)

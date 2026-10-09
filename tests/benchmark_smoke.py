"""Run isolated smoke benchmarks and the Redis CLI/benchmark tools."""
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from resp_client import Client


def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    for mode in ("memory", "aof-always"):
        with tempfile.TemporaryDirectory() as directory:
            with socket.socket() as s:
                s.bind(("127.0.0.1", 0))
                port = s.getsockname()[1]
            options = ["--no-aof"] if mode == "memory" else ["--aof", str(pathlib.Path(directory) / "bench.aof")]
            with open(pathlib.Path(directory) / "server.log", "wb") as log:
                process = subprocess.Popen([binary, "--port", str(port)] + options, stdout=log, stderr=log)
                try:
                    for _ in range(200):
                        if process.poll() is not None:
                            raise RuntimeError("server exited")
                        try:
                            with Client(port=port) as c:
                                assert c.command("PING") == b"PONG"
                            break
                        except OSError:
                            time.sleep(0.01)
                    else:
                        raise TimeoutError("server startup")
                    print(f"\nMode: {mode}", flush=True)
                    for command in ("PING", "SET", "GET", "INCR"):
                        subprocess.run([sys.executable, str(ROOT / "tools" / "benchmark.py"),
                                        "--port", str(port), "--requests", "2000", "--clients", "4",
                                        "--pipeline", "16", "--command", command], check=True, timeout=30)
                    if mode == "memory":
                        subprocess.run(["redis-benchmark", "-p", str(port), "-t", "ping,set,get,incr",
                                        "-n", "2000", "-c", "4", "-P", "16", "--csv"], check=True, timeout=30)
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    if process.returncode != 0:
                        raise RuntimeError(f"server exited with {process.returncode}")


if __name__ == "__main__":
    main()

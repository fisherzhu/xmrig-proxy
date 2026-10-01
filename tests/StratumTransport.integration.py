#!/usr/bin/env python3
"""Exercise plain and TLS proxy transport against a loopback-only fake pool."""

import argparse
import json
import socket
import ssl
import subprocess
import tempfile
import threading
import time
from pathlib import Path


JOB = {
    "blob": "1010e7e1f7d506218bde2c6f54497eb40f333054a819e299e653422b54ffa9935d3b2e7bf1fa1d00000099bc25f5320dff8e3ebb8e4a0113be0b0e0653a6814b97b060ea94bf214d148b4f2b",
    "job_id": "t6-fixture-job-1",
    "target": "8f000000",
    "algo": "rx/0",
    "height": 3774285,
    "seed_hash": "eebd2cd2d21d4ab67ba3a0428abe6889441281fc9ff0e7e30d08ce7f1a90f4cc",
}


def read_line(sock):
    data = bytearray()
    while not data.endswith(b"\n"):
        chunk = sock.recv(65536)
        if not chunk:
            raise RuntimeError("socket closed before a complete JSON frame")
        data.extend(chunk)
        if len(data) > 131072:
            raise RuntimeError("JSON frame exceeded test bound")
    return json.loads(data.decode().split("\n", 1)[0])


class FakePool(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.stop_event = threading.Event()
        self.submit_event = threading.Event()
        self.submits = []
        self.error = None

    def run(self):
        try:
            while not self.stop_event.is_set():
                try:
                    conn, _ = self.listener.accept()
                except socket.timeout:
                    continue
                with conn:
                    conn.settimeout(10)
                    while not self.stop_event.is_set():
                        try:
                            request = read_line(conn)
                        except (socket.timeout, RuntimeError):
                            break
                        method = request.get("method")
                        if method == "login":
                            result = {"id": "t6-upstream", "job": JOB, "status": "OK"}
                        elif method == "submit":
                            self.submits.append(request)
                            self.submit_event.set()
                            result = {"status": "OK"}
                        elif method == "keepalived":
                            result = {"status": "KEEPALIVED"}
                        else:
                            raise RuntimeError("unexpected upstream method: %r" % method)
                        response = {"id": request["id"], "jsonrpc": "2.0", "error": None, "result": result}
                        conn.sendall((json.dumps(response) + "\n").encode())
        except Exception as exc:
            self.error = exc
        finally:
            self.listener.close()

    def close(self):
        self.stop_event.set()
        self.join(timeout=2)


def reserve_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def connect_miner(port, tls, process, log_path):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("proxy exited early: " + log_path.read_text(errors="replace")[-2000:])
        try:
            raw = socket.create_connection(("127.0.0.1", port), timeout=2)
            raw.settimeout(10)
            if not tls:
                return raw
            context = ssl.create_default_context()
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            return context.wrap_socket(raw, server_hostname="localhost")
        except (OSError, ssl.SSLError):
            time.sleep(0.1)
    raise RuntimeError("proxy did not listen on the isolated port")


def run_case(binary, tls):
    pool = FakePool()
    pool.start()
    port = reserve_port()
    mode = "tls" if tls else "plain"
    with tempfile.TemporaryDirectory(prefix="t6-transport-") as temp:
        root = Path(temp)
        config = {
            "autosave": False,
            "watch": False,
            "colors": False,
            "mode": "simple",
            "donate-level": 0,
            "http": {"enabled": False},
            "bind": [{"host": "127.0.0.1", "port": port, "tls": tls}],
            "tls": True if tls else False,
            "pools": [{"url": "127.0.0.1:%d" % pool.port, "user": "fixture", "pass": "x", "tls": False, "enabled": True}],
        }
        config_path = root / "config.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        log_path = root / "proxy.log"
        with log_path.open("wb") as output:
            process = subprocess.Popen([str(binary), "-c", str(config_path), "--no-color"],
                                       cwd=str(root), stdout=output, stderr=subprocess.STDOUT)
            try:
                with connect_miner(port, tls, process, log_path) as miner:
                    login = {"id": 1, "method": "login", "params": {
                        "login": "t6-test-miner", "pass": "x", "agent": "xmrig/6.26.0", "algo": ["rx/0"]}}
                    miner.sendall((json.dumps(login) + "\n").encode())
                    response = read_line(miner)
                    assert response["error"] is None, response
                    assert response["result"]["status"] == "OK", response
                    job = response["result"]["job"]
                    assert job["job_id"] == JOB["job_id"], response
                    submit = {"id": 2, "method": "submit", "params": {
                        "id": response["result"]["id"], "job_id": job["job_id"],
                        "nonce": "00000001", "result": "00" * 24 + "0100000000000000", "algo": "rx/0"}}
                    miner.sendall((json.dumps(submit) + "\n").encode())
                    answer = read_line(miner)
                    assert answer["error"] is None and answer["result"]["status"] == "OK", answer
                    assert pool.submit_event.wait(2), "fake upstream did not receive submit"
                    assert len(pool.submits) == 1 and pool.submits[0]["params"]["job_id"] == JOB["job_id"]
                    print("%s login/job/submit OK" % mode)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                pool.close()
                if pool.error:
                    raise pool.error


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    if not args.binary.is_file():
        parser.error("proxy binary does not exist")
    run_case(args.binary, False)
    run_case(args.binary, True)


if __name__ == "__main__":
    main()

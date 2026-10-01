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
import urllib.request
from pathlib import Path


JOB = {
    "blob": "1010e7e1f7d506218bde2c6f54497eb40f333054a819e299e653422b54ffa9935d3b2e7bf1fa1d00000099bc25f5320dff8e3ebb8e4a0113be0b0e0653a6814b97b060ea94bf214d148b4f2b",
    "job_id": "t6-fixture-job-1",
    "target": "8f000000",
    "algo": "rx/0",
    "height": 3774285,
    "seed_hash": "eebd2cd2d21d4ab67ba3a0428abe6889441281fc9ff0e7e30d08ce7f1a90f4cc",
}


class JsonLines:
    def __init__(self, sock):
        self.sock = sock
        self.pending = bytearray()

    def read(self):
        while b"\n" not in self.pending:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("socket closed before a complete JSON frame")
            self.pending.extend(chunk)
            if len(self.pending) > 131072:
                raise RuntimeError("JSON frame exceeded test bound")
        line, _, remainder = self.pending.partition(b"\n")
        self.pending = bytearray(remainder)
        return json.loads(line.decode())


class FakePool(threading.Thread):
    def __init__(self, tls_context=None):
        super().__init__(daemon=True)
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.tls_context = tls_context
        self.stop_event = threading.Event()
        self.submit_event = threading.Event()
        self.login_event = threading.Event()
        self.send_lock = threading.Lock()
        self.conn = None
        self.submits = []
        self.error = None

    def run(self):
        try:
            while not self.stop_event.is_set():
                try:
                    conn, _ = self.listener.accept()
                except socket.timeout:
                    continue
                if self.tls_context:
                    conn = self.tls_context.wrap_socket(conn, server_side=True)
                with conn:
                    self.conn = conn
                    conn.settimeout(10)
                    lines = JsonLines(conn)
                    while not self.stop_event.is_set():
                        try:
                            request = lines.read()
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
                        with self.send_lock:
                            conn.sendall((json.dumps(response) + "\n").encode())
                        if method == "login":
                            self.login_event.set()
                    self.conn = None
        except Exception as exc:
            self.error = exc
        finally:
            self.listener.close()

    def close(self):
        self.stop_event.set()
        self.join(timeout=2)

    def send_jobs(self, count):
        if not self.login_event.wait(5) or not self.conn:
            raise RuntimeError("fake upstream login did not complete")
        with self.send_lock:
            for number in range(2, count + 2):
                job = dict(JOB, job_id="t6-fixture-job-%d" % number)
                message = {"jsonrpc": "2.0", "method": "job", "params": job}
                self.conn.sendall((json.dumps(message) + "\n").encode())


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
            raw.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            if not tls:
                return raw
            context = ssl.create_default_context()
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            return context.wrap_socket(raw, server_hostname="localhost")
        except (OSError, ssl.SSLError):
            time.sleep(0.1)
    raise RuntimeError("proxy did not listen on the isolated port")


def run_case(binary, tls, upstream_tls, proxy_mode, cert, key):
    context = None
    if upstream_tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(cert), str(key))
    pool = FakePool(context)
    pool.start()
    port = reserve_port()
    http_port = reserve_port()
    while http_port == port:
        http_port = reserve_port()
    mode = "%s downstream %s / upstream %s" % (
        proxy_mode, "tls" if tls else "plain", "tls" if upstream_tls else "plain")
    with tempfile.TemporaryDirectory(prefix="t6-transport-") as temp:
        root = Path(temp)
        config = {
            "autosave": False,
            "watch": False,
            "colors": False,
            "mode": proxy_mode,
            "donate-level": 0,
            "custom-diff": 100,
            "http": {"enabled": True, "host": "127.0.0.1", "port": http_port, "restricted": True},
            "bind": [{"host": "127.0.0.1", "port": port, "tls": tls}],
            "tls": True if tls else False,
            "pools": [{"url": "127.0.0.1:%d" % pool.port, "user": "fixture", "pass": "x", "tls": upstream_tls, "enabled": True}],
        }
        config_path = root / "config.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        log_path = root / "proxy.log"
        with log_path.open("wb") as output:
            process = subprocess.Popen([str(binary), "-c", str(config_path), "--no-color"],
                                       cwd=str(root), stdout=output, stderr=subprocess.STDOUT)
            failure = None
            try:
                with connect_miner(port, tls, process, log_path) as miner:
                    lines = JsonLines(miner)
                    login = {"id": 1, "method": "login", "params": {
                        "login": "t6-test-miner", "pass": "x", "agent": "xmrig/6.26.0", "algo": ["rx/0"]}}
                    miner.sendall((json.dumps(login) + "\n").encode())
                    response = lines.read()
                    assert response["error"] is None, response
                    assert response["result"]["status"] == "OK", response
                    job = response["result"]["job"]
                    assert job["job_id"] == JOB["job_id"], response
                    pool.send_jobs(24)
                    time.sleep(0.05)
                    for number in range(2, 26):
                        notification = lines.read()
                        assert notification["method"] == "job", notification
                        assert notification["params"]["job_id"] == "t6-fixture-job-%d" % number, notification
                    job = notification["params"]
                    nonce = "000000" + job["blob"][84:86] if proxy_mode == "nicehash" else "00000001"
                    rejected = {"id": 2, "method": "submit", "params": {
                        "id": response["result"]["id"], "job_id": "not-assigned",
                        "nonce": nonce, "result": "00" * 24 + "0100000000000000", "algo": "rx/0"}}
                    miner.sendall((json.dumps(rejected) + "\n").encode())
                    rejection = lines.read()
                    assert rejection["error"] is not None, rejection
                    local_target = ((1 << 64) - 1) // 1000
                    local_result = "00" * 24 + local_target.to_bytes(8, "little").hex()
                    local_share = {"id": 3, "method": "submit", "params": {
                        "id": response["result"]["id"], "job_id": job["job_id"],
                        "nonce": nonce, "result": local_result, "algo": "rx/0"}}
                    miner.sendall((json.dumps(local_share) + "\n").encode())
                    local_answer = lines.read()
                    assert local_answer["error"] is None and local_answer["result"]["status"] == "OK", local_answer
                    assert not pool.submit_event.is_set(), "custom-diff share unexpectedly reached upstream"
                    submit = {"id": 4, "method": "submit", "params": {
                        "id": response["result"]["id"], "job_id": job["job_id"],
                        "nonce": nonce, "result": "00" * 24 + "0100000000000000", "algo": "rx/0"}}
                    miner.sendall((json.dumps(submit) + "\n").encode())
                    answer = lines.read()
                    assert answer["error"] is None and answer["result"]["status"] == "OK", answer
                    assert pool.submit_event.wait(2), "fake upstream did not receive submit"
                    assert len(pool.submits) == 1 and pool.submits[0]["params"]["job_id"] == job["job_id"]
                    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
                    with opener.open("http://127.0.0.1:%d/1/summary" % http_port, timeout=5) as reply:
                        summary = json.load(reply)
                    writes = summary["results"]["owned_writes"]
                    assert writes["accepted"] >= 26, writes
                    assert writes["completed"] <= writes["accepted"], writes
                    assert writes["peak_owned_bytes"] > 0, writes
                    assert writes["over_limit"] == 0 and writes["immediate_error"] == 0, writes
                    assert writes["callback_error"] == 0, writes
                    print("%s login/24 ordered jobs/reject/local share/submit OK" % mode)
            except Exception as exc:
                failure = exc
                tail = log_path.read_text(errors="replace")[-4000:]
                raise RuntimeError("%s failed: %s; proxy exit=%r; proxy log tail:\n%s" %
                                   (mode, exc, process.poll(), tail)) from exc
            finally:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                pool.close()
                if pool.error and failure is None:
                    raise RuntimeError("fake upstream failed: %s" % pool.error)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--openssl", default="openssl")
    args = parser.parse_args()
    if not args.binary.is_file():
        parser.error("proxy binary does not exist")
    with tempfile.TemporaryDirectory(prefix="t6-cert-") as temp:
        cert = Path(temp) / "cert.pem"
        key = Path(temp) / "key.pem"
        subprocess.run([args.openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-subj", "/CN=localhost", "-keyout", str(key), "-out", str(cert), "-days", "1"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        run_case(args.binary, False, False, "simple", cert, key)
        run_case(args.binary, True, False, "simple", cert, key)
        run_case(args.binary, True, True, "simple", cert, key)
        run_case(args.binary, True, True, "nicehash", cert, key)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Disposable G5 TUI lab with real Xray traffic counters on loopback only."""

import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Loopback(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"xrm-g5-stats-loopback\n"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("out/real-core"))
    parser.add_argument("--core", type=Path, default=Path(".deps/tools/xray-26.9.9/xray"))
    parser.add_argument("--record", type=Path, help="Write non-secret verification evidence here")
    parser.add_argument("--verify-only", action="store_true", help="Check counters and exit without TUI")
    args = parser.parse_args()
    build, core = args.build.resolve(), args.core.resolve()
    for executable in (build / "xrm", build / "xrmd", core):
        if not executable.is_file() or not os.access(executable, os.X_OK):
            parser.error(f"executable missing: {executable}")
    version = subprocess.check_output([str(core), "version"], text=True).splitlines()[0]
    if "26.9.9" not in version:
        parser.error(f"fixture requires fixed Xray 26.9.9, got: {version}")

    with tempfile.TemporaryDirectory(prefix="xrm-g5-stats-") as directory:
        root = Path(directory)
        env = os.environ.copy()
        for key, child in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
                           ("XDG_DATA_HOME", "data"), ("XDG_STATE_HOME", "state"),
                           ("XDG_CACHE_HOME", "cache"), ("XDG_RUNTIME_DIR", "run")):
            path = root / child
            path.mkdir(mode=0o700)
            env[key] = str(path)
        socket_path = root / "run/xrmd.sock"
        database = root / "state/xrmd.sqlite"
        http_server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Loopback)
        threading.Thread(target=http_server.serve_forever, daemon=True).start()
        daemon_log = (root / "xrmd.log").open("w")
        daemon = subprocess.Popen([str(build / "xrmd"), "--foreground", "--socket",
                                   str(socket_path), "--database", str(database)],
                                  env=env, stdout=daemon_log, stderr=subprocess.STDOUT)
        stop_traffic = threading.Event()
        traffic_thread = None
        watcher = None
        watcher_log = (root / "stats-watch.jsonl").open("w")
        try:
            deadline = time.monotonic() + 8
            while not socket_path.exists() and time.monotonic() < deadline:
                if daemon.poll() is not None:
                    raise RuntimeError(f"xrmd exited: {daemon_log.name}")
                time.sleep(.05)
            if not socket_path.exists():
                raise RuntimeError("isolated xrmd socket did not appear")

            def cli(*argv):
                result = subprocess.run([str(build / "xrm"), "--socket", str(socket_path),
                                         "--json", *argv], env=env, capture_output=True, text=True,
                                        timeout=20)
                if result.returncode:
                    raise RuntimeError(f"xrm {' '.join(argv)}: {result.stderr.strip()}")
                return json.loads(result.stdout)

            registered = cli("core", "register", str(core), "--name", "G5 fixed Xray")
            profile = cli("profile", "create", "--name", "G5 Stats", "--core", registered["id"])
            profile_id = profile["id"]
            node = cli("node", "add", "--name", "G5 loopback direct", "--outbound-json",
                       '{"protocol":"freedom","settings":{}}')
            cli("select", "node", profile_id, node["id"])
            socks_port, metrics_port = free_port(), free_port()
            if socks_port == metrics_port or http_server.server_port in (socks_port, metrics_port):
                raise RuntimeError("fixture port collision; rerun for new ephemeral ports")
            inbounds = {"inbounds": [{"tag": "g5-socks", "listen": "127.0.0.1",
                                      "port": socks_port, "protocol": "socks",
                                      "settings": {"auth": "noauth", "udp": False}}]}
            cli("config", "edit", profile_id, "inbounds", "--text", json.dumps(inbounds))
            cli("config", "enable", profile_id, "inbounds")
            cli("config", "statistics", "enable", profile_id, "--metrics-listen",
                f"127.0.0.1:{metrics_port}")
            source = {key: cli("config", "show", profile_id, key)
                      for key in ("inbounds", "stats", "policy", "metrics")}
            cli("connect", profile_id)
            status = cli("status")
            if status.get("state") != "running" or not status.get("active"):
                raise RuntimeError(f"fixed Xray did not start: {status}")
            # Prime the run endpoint, then keep one viewer connected so the
            # server's periodic sampler remains active during the TUI session.
            cli("stats")
            watcher = subprocess.Popen([str(build / "xrm"), "--socket", str(socket_path),
                                        "--json", "stats", "--watch"], env=env,
                                       stdout=watcher_log, stderr=subprocess.STDOUT)

            target = f"http://127.0.0.1:{http_server.server_port}/proof"

            def traffic_once():
                result = subprocess.run(["curl", "--silent", "--show-error", "--fail",
                                         "--max-time", "4", "--noproxy", "", "--proxy",
                                         f"socks5h://127.0.0.1:{socks_port}", target],
                                        env=env, capture_output=True, text=True, timeout=6)
                if result.returncode or result.stdout != "xrm-g5-stats-loopback\n":
                    raise RuntimeError(f"loopback SOCKS request failed: {result.stderr.strip()}")

            def traffic_loop():
                while not stop_traffic.wait(1):
                    try:
                        traffic_once()
                    except (OSError, RuntimeError, subprocess.TimeoutExpired):
                        if not stop_traffic.is_set():
                            break

            traffic_once()
            traffic_thread = threading.Thread(target=traffic_loop, daemon=True)
            traffic_thread.start()
            sample = None
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                current = cli("stats")
                counters = (current.get("sample") or {}).get("traffic", [])
                if current.get("availability") == "available" and any(
                        type(row.get(direction)) is int and row[direction] > 0
                        for row in counters for direction in ("uplink", "downlink")):
                    sample = current
                    break
                time.sleep(.5)
            if sample is None:
                raise RuntimeError(f"real nonzero Xray counters not observed: {current}")
            runtime = Path(status["active"]["confdir"])
            files = sorted(path.name for path in runtime.glob("*.json"))
            if not {"02_inbounds.json", "07_metrics.json", "08_stats.json",
                    "09_policy.json"}.issubset(files) or "config.json" in files:
                raise RuntimeError(f"native runtime file set unexpected: {files}")
            evidence = {"build": str(build), "xrm_sha256": sha256(build / "xrm"),
                        "xrmd_sha256": sha256(build / "xrmd"), "xray": str(core),
                        "xray_version": version, "xray_sha256": sha256(core),
                        "isolated_root": str(root), "socket": str(socket_path),
                        "database": str(database), "daemon_pid": daemon.pid,
                        "profile_id": profile_id, "socks_port": socks_port,
                        "metrics_port": metrics_port, "http_port": http_server.server_port,
                        "source": source, "status": status, "runtime_files": files,
                        "stats": sample, "loopback_response": "xrm-g5-stats-loopback"}
            if args.record:
                args.record.parent.mkdir(parents=True, exist_ok=True)
                args.record.write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n")
            print(f"G5 isolated Stats lab: {root}", flush=True)
            print(f"Real Xray counters: {sample['sample']['traffic']}", flush=True)
            if not args.verify_only:
                print("Open Overview for Stats. Exit TUI to stop and remove this lab.", flush=True)
                subprocess.run([str(build / "xrm"), "--socket", str(socket_path),
                                "tui", "--profile", profile_id], env=env, check=True)
        finally:
            stop_traffic.set()
            if traffic_thread:
                traffic_thread.join(timeout=5)
            if watcher and watcher.poll() is None:
                watcher.terminate()
                try:
                    watcher.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    watcher.kill()
                    watcher.wait()
            if daemon.poll() is None:
                daemon.terminate()
                try:
                    daemon.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait()
            http_server.shutdown()
            http_server.server_close()
            daemon_log.close()
            watcher_log.close()


if __name__ == "__main__":
    main()

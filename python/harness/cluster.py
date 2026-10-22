"""Start and control a local quorumkv cluster of real server processes.

Clients connect to each node's client port directly. Peer links go through ProxyHub, so the
harness can partition, delay, or cut one direction of any link. Process faults use psutil:
kill is SIGKILL on POSIX and TerminateProcess on Windows; pause and resume are SIGSTOP and
SIGCONT on POSIX and thread suspension on Windows.
"""

from __future__ import annotations

import json
import pathlib
import socket
import subprocess
import time
from dataclasses import dataclass

import psutil

from harness.proxy import ProxyHub


def free_ports(count: int) -> list[int]:
    sockets = []
    try:
        for _ in range(count):
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listener.bind(("127.0.0.1", 0))
            sockets.append(listener)
        return [listener.getsockname()[1] for listener in sockets]
    finally:
        for listener in sockets:
            listener.close()


@dataclass
class Node:
    id: int
    peer_port: int
    client_port: int
    data_dir: pathlib.Path
    log_path: pathlib.Path
    process: subprocess.Popen | None = None
    paused: bool = False


class Cluster:
    def __init__(
        self,
        server: pathlib.Path,
        client: pathlib.Path,
        root: pathlib.Path,
        size: int = 3,
        tick_ms: int = 10,
        snapshot_threshold: int = 500,
        unsafe_reads: bool = False,
        direct: bool = False,
        server_args: list[str] | None = None,
    ) -> None:
        if size not in (3, 5):
            raise ValueError("cluster size must be 3 or 5")
        self.server, self.client, self.root = server, client, root
        self.tick_ms, self.snapshot_threshold = tick_ms, snapshot_threshold
        self.unsafe_reads = unsafe_reads
        # Direct clusters skip the proxies, for benchmarks; link faults then do nothing.
        self.direct = direct
        self.server_args = list(server_args or [])
        self.hub = ProxyHub()
        links = [(i, j) for i in range(1, size + 1) for j in range(1, size + 1) if i != j]
        ports = free_ports(2 * size + len(links))
        self.nodes = {
            i: Node(
                id=i,
                peer_port=ports[2 * (i - 1)],
                client_port=ports[2 * (i - 1) + 1],
                data_dir=root / f"n{i}",
                log_path=root / f"n{i}.log",
            )
            for i in range(1, size + 1)
        }
        self.proxy_ports = dict(zip(links, ports[2 * size :], strict=True))
        if not direct:
            for link, port in self.proxy_ports.items():
                self.hub.add_link(link, port, self.nodes[link[1]].peer_port)

    @property
    def size(self) -> int:
        return len(self.nodes)

    def client_addresses(self) -> str:
        return ",".join(f"127.0.0.1:{self.nodes[i].client_port}" for i in sorted(self.nodes))

    def start(self, node_id: int, timeout_s: float = 10.0) -> None:
        node = self.nodes[node_id]
        node.data_dir.mkdir(parents=True, exist_ok=True)
        command = [
            str(self.server),
            "--id", str(node_id),
            "--members", ",".join(str(i) for i in sorted(self.nodes)),
            "--peer-listen", f"127.0.0.1:{node.peer_port}",
            "--client-listen", f"127.0.0.1:{node.client_port}",
            "--data", str(node.data_dir),
            "--tick-ms", str(self.tick_ms),
            "--snapshot-threshold", str(self.snapshot_threshold),
        ]  # fmt: skip
        for other in sorted(self.nodes):
            if other != node_id:
                port = (
                    self.nodes[other].peer_port
                    if self.direct
                    else self.proxy_ports[(node_id, other)]
                )
                command += ["--peer", f"{other}=127.0.0.1:{port}"]
        command += self.server_args
        if self.unsafe_reads:
            command.append("--unsafe-reads")
        log = node.log_path.open("ab")
        node.process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        log.close()
        node.paused = False
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if node.process.poll() is not None:
                raise RuntimeError(f"node {node_id} exited: see {node.log_path}")
            if f"ready id={node_id}".encode() in node.log_path.read_bytes():
                return
            time.sleep(0.02)
        raise RuntimeError(f"node {node_id} did not become ready")

    def start_all(self) -> None:
        for node_id in sorted(self.nodes):
            self.start(node_id)

    def alive(self, node_id: int) -> bool:
        process = self.nodes[node_id].process
        return process is not None and process.poll() is None

    def kill(self, node_id: int) -> None:
        node = self.nodes[node_id]
        if self.alive(node_id):
            psutil.Process(node.process.pid).kill()
            node.process.wait(timeout=10)
        node.paused = False

    def pause(self, node_id: int) -> None:
        node = self.nodes[node_id]
        if self.alive(node_id) and not node.paused:
            psutil.Process(node.process.pid).suspend()
            node.paused = True

    def resume(self, node_id: int) -> None:
        node = self.nodes[node_id]
        if self.alive(node_id) and node.paused:
            psutil.Process(node.process.pid).resume()
            node.paused = False

    def partition(self, groups: dict[int, int]) -> None:
        for source, target in self.hub.links():
            self.hub.set_blocked((source, target), groups[source] != groups[target])

    def cut(self, source: int, target: int) -> None:
        self.hub.set_blocked((source, target), True)

    def delay(self, source: int, target: int, delay_s: float) -> None:
        self.hub.set_delay((source, target), delay_s)

    def heal(self) -> None:
        self.hub.heal()

    def status(self) -> list[dict]:
        result = subprocess.run(
            [str(self.client), "--nodes", self.client_addresses(), "status"],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]

    def leader(self) -> int | None:
        leaders = [
            entry
            for entry in self.status()
            if entry.get("up") and entry.get("role") == "leader" and self.alive(entry["id"])
        ]
        # A deposed leader may not know yet; the highest term is the current one.
        return max(leaders, key=lambda entry: entry["term"])["id"] if leaders else None

    def stop(self) -> None:
        for node_id in sorted(self.nodes):
            self.resume(node_id)
            self.kill(node_id)
        self.hub.close()

"""Run every M8 benchmark against real localhost clusters and write results/m8/.

    python bench/run_benchmarks.py --bin build/release [--repeats 5] [--quick]

Each configuration starts a fresh cluster (peers connected directly, without the fault
proxies) and runs `quorumkv_load` against it. Throughput benchmarks run at least five times
and report the median with min and max; latencies report the median of each run's p50 and p99.
Failover runs 50 trials. Machine information is recorded with every result.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from harness.cluster import Cluster  # noqa: E402

SUFFIX = ".exe" if sys.platform == "win32" else ""


def machine(binaries: pathlib.Path) -> dict:
    import psutil

    info = {
        "os": platform.platform(),
        "cpu": _cpu_model(),
        "physical_cores": psutil.cpu_count(logical=False),
        "logical_cores": psutil.cpu_count(logical=True),
        "memory_gib": round(psutil.virtual_memory().total / 2**30, 1),
        "python": platform.python_version(),
        "cluster": "localhost, one process per node",
        "thread_pinning": "not pinned; the OS scheduler places threads",
        "turbo_boost": "not disabled; it cannot be changed from user space here",
    }
    cache = binaries / "CMakeCache.txt"
    if cache.exists():
        text = cache.read_text(errors="replace")
        for key in ("CMAKE_CXX_COMPILER", "CMAKE_BUILD_TYPE", "CMAKE_CXX_FLAGS_RELEASE"):
            match = re.search(rf"^{key}:\w+=(.*)$", text, re.MULTILINE)
            if match:
                info[key.lower()] = match.group(1).strip()
    compiler = info.get("cmake_cxx_compiler", "")
    if compiler:
        result = subprocess.run([compiler], capture_output=True, text=True, check=False)
        banner = (result.stderr or result.stdout).splitlines()
        info["compiler_version"] = banner[0] if banner else compiler
    return info


def _cpu_model() -> str:
    if sys.platform == "win32":
        result = subprocess.run(
            ["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_Processor).Name"],
            capture_output=True,
            text=True,
            check=False,
        )
        return result.stdout.strip() or platform.processor()
    cpuinfo = pathlib.Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    return platform.processor()


class Bench:
    def __init__(self, binaries: pathlib.Path, scratch: pathlib.Path, quick: bool) -> None:
        self.server = binaries / f"quorumkv_server{SUFFIX}"
        self.client = binaries / f"quorumkv_client{SUFFIX}"
        self.load = binaries / f"quorumkv_load{SUFFIX}"
        self.scratch = scratch
        self.duration_ms = 3000 if quick else 10000
        self.counter = 0

    def cluster(self, nodes: int, server_args: list[str] | None = None) -> Cluster:
        self.counter += 1
        root = self.scratch / f"cluster-{self.counter}"
        shutil.rmtree(root, ignore_errors=True)
        cluster = Cluster(
            self.server,
            self.client,
            root,
            size=nodes,
            snapshot_threshold=1000,
            direct=True,
            server_args=server_args,
        )
        cluster.start_all()
        self.await_leader(cluster)
        return cluster

    @staticmethod
    def await_leader(cluster: Cluster, timeout_s: float = 20.0) -> int:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            leader = cluster.leader()
            if leader is not None:
                return leader
            time.sleep(0.05)
        raise RuntimeError("no leader elected")

    def run_load(self, cluster: Cluster, mode: str, value_size: int = 100, threads: int = 32):
        result = subprocess.run(
            [
                str(self.load),
                "--nodes",
                cluster.client_addresses(),
                "--mode",
                mode,
                "--threads",
                str(threads),
                "--duration-ms",
                str(self.duration_ms),
                "--warmup-ms",
                "2000",
                "--value-size",
                str(value_size),
                "--keys",
                "1000",
            ],  # fmt: skip
            capture_output=True,
            text=True,
            check=True,
        )
        return json.loads(result.stdout.strip().splitlines()[-1])

    def throughput(self, nodes: int, mode: str, repeats: int, **options) -> dict:
        server_args = options.pop("server_args", None)
        runs = []
        for _ in range(repeats):
            cluster = self.cluster(nodes, server_args)
            try:
                runs.append(self.run_load(cluster, mode, **options))
            finally:
                cluster.stop()
        return summarize(runs)

    def failover(self, trials: int, nodes: int = 3) -> dict:
        samples = []
        elections = []
        cluster = None
        for trial in range(trials):
            # A fresh cluster every ten trials keeps log growth and restarts comparable.
            if cluster is None or trial % 10 == 0:
                if cluster is not None:
                    cluster.stop()
                cluster = self.cluster(nodes)
            leader = self.await_leader(cluster)
            old_term = max(entry["term"] for entry in self._poll(cluster) if entry.get("up"))
            probe = subprocess.Popen(
                [str(self.load), "--nodes", cluster.client_addresses(), "--mode", "probe",
                 "--duration-ms", "8000"],
                stdout=subprocess.PIPE,
                text=True,
            )  # fmt: skip
            successes = 0
            while successes < 20:
                line = probe.stdout.readline()
                if not line:
                    break
                successes += line.startswith("ok ")
            killed = time.monotonic_ns()
            cluster.kill(leader)
            # The old leader can still acknowledge writes until the kill takes effect, so only
            # writes invoked after the process is gone count.
            dead = time.monotonic_ns()
            elected = self._await_new_leader(cluster, leader, old_term)
            if elected is not None:
                elections.append((elected - killed) / 1e6)
            first = None
            for line in probe.stdout:
                parts = line.split()
                if parts[0] == "ok" and int(parts[1]) > dead:
                    first = int(parts[2])
                    break
            probe.kill()
            probe.wait()
            if first is not None:
                samples.append((first - killed) / 1e6)
            cluster.start(leader)
        if cluster is not None:
            cluster.stop()
        ordered = sorted(samples)
        elected = sorted(elections)
        return {
            "trials": trials,
            "measured": len(samples),
            "failover_ms": samples,
            "median_ms": statistics.median(ordered),
            "p99_ms": percentile(ordered, 0.99),
            "min_ms": ordered[0],
            "max_ms": ordered[-1],
            "election_ms": elections,
            "election_median_ms": statistics.median(elected),
            "election_p99_ms": percentile(elected, 0.99),
        }

    def _await_new_leader(self, cluster: Cluster, old: int, old_term: int) -> int | None:
        # Polls only the survivors: on Windows a connection to the dead node's port takes
        # about two seconds to be refused, which would swamp the measurement. Each poll is one
        # client process, so the resolution is a few tens of milliseconds.
        survivors = ",".join(
            f"127.0.0.1:{cluster.nodes[i].client_port}" for i in sorted(cluster.nodes) if i != old
        )
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            result = subprocess.run(
                [str(self.client), "--nodes", survivors, "status"],
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            now = time.monotonic_ns()
            for line in result.stdout.splitlines():
                entry = json.loads(line) if line.strip() else {}
                if entry.get("role") == "leader" and entry.get("term", 0) > old_term:
                    return now
        return None

    def catch_up(self, entries: int, repeats: int) -> dict:
        runs = []
        for _ in range(repeats):
            cluster = self.cluster(3)
            load = None
            try:
                leader = self.await_leader(cluster)
                follower = next(i for i in sorted(cluster.nodes) if i != leader)
                cluster.kill(follower)
                start_commit = self._commit(cluster)
                load = subprocess.Popen(
                    [str(self.load), "--nodes", cluster.client_addresses(), "--mode", "write",
                     "--threads", "64", "--duration-ms", "3600000", "--warmup-ms", "0",
                     "--value-size", "100", "--keys", "1000"],
                    stdout=subprocess.DEVNULL,
                )  # fmt: skip
                written_from = time.monotonic()
                while self._commit(cluster) - start_commit < entries:
                    time.sleep(0.5)
                load.kill()
                load.wait()
                write_seconds = time.monotonic() - written_from
                target = self._commit(cluster)
                restarted = time.monotonic()
                cluster.start(follower)
                while (self._status(cluster, follower) or {}).get("applied", 0) < target:
                    if time.monotonic() - restarted > 900:
                        raise RuntimeError("the follower did not catch up within 15 minutes")
                    time.sleep(0.05)
                caught_up = time.monotonic() - restarted
                print(f"catch-up run {len(runs) + 1}: {caught_up:.1f} s", flush=True)
                runs.append(
                    {
                        "entries_behind": target - start_commit,
                        "catch_up_seconds": round(caught_up, 3),
                        "write_seconds": round(write_seconds, 1),
                        "follower_snapshot_index": (self._status(cluster, follower) or {}).get(
                            "snapshot"
                        ),
                    }
                )
            finally:
                if load is not None and load.poll() is None:
                    load.kill()
                    load.wait()
                cluster.stop()
        seconds = sorted(run["catch_up_seconds"] for run in runs)
        return {
            "runs": runs,
            "median_s": statistics.median(seconds),
            "min_s": seconds[0],
            "max_s": seconds[-1],
        }

    @staticmethod
    def _commit(cluster: Cluster) -> int:
        # Under heavy load a status request can miss a node, and leadership can move, so this
        # takes the highest commit index any live node reports.
        while True:
            commits = [entry["commit"] for entry in Bench._poll(cluster) if entry.get("up")]
            if commits:
                return max(commits)
            time.sleep(0.2)

    @staticmethod
    def _poll(cluster: Cluster) -> list[dict]:
        # A node busy installing a large snapshot can leave a status request unanswered.
        try:
            return cluster.status()
        except subprocess.TimeoutExpired:
            return []

    @staticmethod
    def _status(cluster: Cluster, node: int) -> dict | None:
        for entry in Bench._poll(cluster):
            if entry["id"] == node and entry.get("up"):
                return entry
        return None


def percentile(ordered: list[float], fraction: float) -> float:
    return ordered[min(len(ordered) - 1, int(fraction * (len(ordered) - 1) + 0.5))]


def summarize(runs: list[dict]) -> dict:
    rates = sorted(run["ops_per_sec"] for run in runs)
    return {
        "runs": runs,
        "ops_per_sec_median": statistics.median(rates),
        "ops_per_sec_min": rates[0],
        "ops_per_sec_max": rates[-1],
        "p50_us_median": statistics.median(run["p50_us"] for run in runs),
        "p99_us_median": statistics.median(run["p99_us"] for run in runs),
        "unknown_total": sum(run["unknown"] for run in runs),
        "fail_total": sum(run["fail"] for run in runs),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "results" / "m8")
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--failover-trials", type=int, default=50)
    parser.add_argument("--catch-up-entries", type=int, default=1_000_000)
    parser.add_argument("--catch-up-repeats", type=int, default=5)
    parser.add_argument("--quick", action="store_true", help="short runs for a smoke test")
    parser.add_argument("--only", nargs="*", help="run only these benchmark names")
    args = parser.parse_args()
    if args.repeats < 5 and not args.quick:
        parser.error("benchmarks run at least five times")
    args.out.mkdir(parents=True, exist_ok=True)
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="quorumkv-bench-"))
    bench = Bench(args.bin, scratch, args.quick)
    output = args.out / ("benchmarks-quick.json" if args.quick else "benchmarks.json")
    report = {}
    if args.only and output.exists():
        # A partial rerun keeps the other benchmarks' results.
        report = json.loads(output.read_text())
    report.update({"producer": "bench/run_benchmarks.py", "machine": machine(args.bin)})
    report["settings"] = {
        "repeats": args.repeats,
        "duration_ms": bench.duration_ms,
        "warmup_ms": 2000,
        "client_threads": 32,
        "keys": 1000,
        "tick_ms": 10,
        "snapshot_threshold": 1000,
    }
    plan = {
        "write_3_nodes": lambda: bench.throughput(3, "write", args.repeats),
        "write_5_nodes": lambda: bench.throughput(5, "write", args.repeats),
        "batching_on_pipelining_on": lambda: bench.throughput(3, "write", args.repeats),
        "batching_off": lambda: bench.throughput(
            3, "write", args.repeats, server_args=["--no-batching"]
        ),
        "pipelining_off": lambda: bench.throughput(
            3, "write", args.repeats, server_args=["--no-pipeline"]
        ),
        "batching_and_pipelining_off": lambda: bench.throughput(
            3, "write", args.repeats, server_args=["--no-batching", "--no-pipeline"]
        ),
        "read_index": lambda: bench.throughput(3, "read-index", args.repeats),
        "read_through_log": lambda: bench.throughput(3, "read-log", args.repeats),
        "value_100_b": lambda: bench.throughput(3, "write", args.repeats, value_size=100),
        "value_1_kb": lambda: bench.throughput(3, "write", args.repeats, value_size=1000),
        "value_10_kb": lambda: bench.throughput(3, "write", args.repeats, value_size=10000),
        "failover": lambda: bench.failover(args.failover_trials),
        "snapshot_catch_up": lambda: bench.catch_up(args.catch_up_entries, args.catch_up_repeats),
    }
    for name, run in plan.items():
        if args.only and name not in args.only:
            continue
        started = time.monotonic()
        report[name] = run()
        report[name]["wall_seconds"] = round(time.monotonic() - started, 1)
        output.write_text(json.dumps(report, indent=2) + "\n")
        print(name, json.dumps({k: v for k, v in report[name].items() if k != "runs"})[:300])
    shutil.rmtree(scratch, ignore_errors=True)
    print(f"Wrote {output}")


if __name__ == "__main__":
    os.environ.setdefault("PYTHONUNBUFFERED", "1")
    main()

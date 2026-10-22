"""Run clients against a real cluster under a seeded nemesis and record histories.

    python -m harness.run --server PATH --client PATH --seed 7 --duration 300 --out runs/seed-7

The nemesis acts every 2 to 5 seconds: random partitions, one-way cuts, link delays, SIGKILL of
the leader with a restart, and pauses (SIGSTOP/SIGCONT) that simulate long GC stalls. Every
client writes a JSONL history; `history.jsonl` merges them, and `run.json` summarizes the run,
the nemesis events, and the linearizability verdict when the checker is importable.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import random
import shutil
import subprocess
import sys
import time

from harness.cluster import Cluster


class Nemesis:
    def __init__(self, cluster: Cluster, rng: random.Random, log) -> None:
        self.cluster, self.rng, self.log = cluster, rng, log
        self.restarts: list[tuple[float, int]] = []
        self.resumes: list[tuple[float, int]] = []
        self.counts: dict[str, int] = {}

    def record(self, action: str, **details) -> None:
        self.counts[action] = self.counts.get(action, 0) + 1
        self.log.write(json.dumps({"ts": time.monotonic_ns(), "action": action, **details}) + "\n")
        self.log.flush()

    def due(self) -> None:
        now = time.monotonic()
        for when, node in list(self.restarts):
            if when <= now:
                self.restarts.remove((when, node))
                self.cluster.start(node)
                self.record("restart", node=node)
        for when, node in list(self.resumes):
            if when <= now:
                self.resumes.remove((when, node))
                self.cluster.resume(node)
                self.record("resume", node=node)

    def act(self) -> None:
        nodes = sorted(self.cluster.nodes)
        choice = self.rng.choice(
            [
                "partition",
                "partition",
                "cut",
                "delay",
                "kill_leader",
                "kill_leader",
                "pause",
                "heal",
            ]
        )
        down = {node for _, node in self.restarts} | {node for _, node in self.resumes}
        if choice == "partition":
            groups = {node: self.rng.randrange(2) for node in nodes}
            groups[nodes[0]], groups[nodes[-1]] = 0, 1
            self.cluster.partition(groups)
            self.record("partition", groups=groups)
        elif choice == "cut":
            source, target = self.rng.sample(nodes, 2)
            self.cluster.cut(source, target)
            self.record("cut", source=source, target=target)
        elif choice == "delay":
            source, target = self.rng.sample(nodes, 2)
            delay = self.rng.uniform(0.05, 0.4)
            self.cluster.delay(source, target, delay)
            self.record("delay", source=source, target=target, seconds=round(delay, 3))
        elif choice == "kill_leader":
            leader = self.cluster.leader()
            target = leader if leader is not None else self.rng.choice(nodes)
            if target not in down:
                self.cluster.kill(target)
                self.restarts.append((time.monotonic() + self.rng.uniform(1, 3), target))
                self.record("kill", node=target, was_leader=leader == target)
        elif choice == "pause":
            target = self.rng.choice(nodes)
            if target not in down and self.cluster.alive(target):
                self.cluster.pause(target)
                self.resumes.append((time.monotonic() + self.rng.uniform(1, 3), target))
                self.record("pause", node=target)
        else:
            self.cluster.heal()
            self.record("heal")

    def settle(self) -> None:
        self.cluster.heal()
        for _, node in self.resumes:
            self.cluster.resume(node)
        for _, node in self.restarts:
            self.cluster.start(node)
        self.resumes.clear()
        self.restarts.clear()
        self.record("settle")


def merge(directory: pathlib.Path, clients: int) -> list[dict]:
    history = []
    for client in range(1, clients + 1):
        path = directory / f"client-{client}.jsonl"
        history += [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    history.sort(key=lambda op: op["invoke_ts"])
    return history


def run(args: argparse.Namespace) -> dict:
    out = pathlib.Path(args.out)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    rng = random.Random(args.seed)
    cluster = Cluster(
        pathlib.Path(args.server),
        pathlib.Path(args.client),
        out / "cluster",
        size=args.nodes,
        unsafe_reads=args.unsafe_reads,
    )
    started = time.monotonic()
    processes = []
    try:
        cluster.start_all()
        # Clients run slightly longer than the nemesis so the healed cluster serves them again.
        client_ms = int((args.duration + args.recovery) * 1000)
        for client in range(1, args.clients + 1):
            processes.append(
                subprocess.Popen(
                    [
                        args.client,
                        "--nodes",
                        cluster.client_addresses(),
                        "workload",
                        "--client",
                        str(client),
                        "--seed",
                        str(args.seed),
                        "--duration-ms",
                        str(client_ms),
                        "--keys",
                        str(args.keys),
                        "--out",
                        str(out / f"client-{client}.jsonl"),
                    ]  # fmt: skip
                )
            )
        with (out / "nemesis.jsonl").open("w") as log:
            nemesis = Nemesis(cluster, rng, log)
            next_action = time.monotonic() + rng.uniform(2, 5)
            while time.monotonic() - started < args.duration:
                nemesis.due()
                if time.monotonic() >= next_action:
                    nemesis.act()
                    next_action = time.monotonic() + rng.uniform(2, 5)
                time.sleep(0.05)
            nemesis.settle()
            for process in processes:
                process.wait(timeout=args.recovery + 120)
            counts = nemesis.counts
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
        cluster.stop()
    history = merge(out, args.clients)
    with (out / "history.jsonl").open("w") as merged:
        for op in history:
            merged.write(json.dumps(op) + "\n")
    summary = {
        "seed": args.seed,
        "nodes": args.nodes,
        "clients": args.clients,
        "keys": args.keys,
        "duration_s": args.duration,
        "unsafe_reads": args.unsafe_reads,
        "operations": len(history),
        "results": {
            name: sum(op["result"] == name for op in history) for name in ("ok", "fail", "info")
        },
        "nemesis": counts,
    }
    try:
        from linearizability.checker import check_history
    except ImportError:
        summary["linearizable"] = None
    else:
        verdict = check_history(history)
        summary["linearizable"] = verdict.ok
        if not verdict.ok:
            summary["counterexample"] = verdict.explanation
    (out / "run.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--duration", type=float, default=300)
    parser.add_argument("--recovery", type=float, default=10)
    parser.add_argument("--nodes", type=int, default=3)
    parser.add_argument("--clients", type=int, default=4)
    parser.add_argument("--keys", type=int, default=3)
    parser.add_argument("--unsafe-reads", action="store_true")
    parser.add_argument("--out", required=True)
    summary = run(parser.parse_args())
    print(json.dumps(summary))
    sys.exit(0 if summary.get("linearizable") is not False else 1)


if __name__ == "__main__":
    main()

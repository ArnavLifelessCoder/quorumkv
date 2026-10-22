import itertools
import json
import pathlib
import random
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "python"))
from linearizability.checker import check_history, linearizable, parse, step


def op(client, kind, invoke, complete, key="k", value=None, expected=None, output=None):
    return {
        "client": client,
        "op": kind,
        "key": key,
        "value": value,
        "expected": expected,
        "invoke_ts": invoke,
        "complete_ts": complete,
        "result": "ok" if complete is not None else "info",
        "output": output or {},
    }


def get(client, invoke, complete, value, key="k"):
    output = {"found": value is not None, "value": value or ""}
    return op(client, "get", invoke, complete, key=key, output=output)


def brute_force(history):
    """Reference: try every order that respects real time; info ops may be left out."""
    for ops in parse(history).values():
        required = [o for o in ops if o.complete is not None]
        optional = [o for o in ops if o.complete is None]
        found = False
        for count in range(len(optional) + 1):
            for chosen in itertools.combinations(optional, count):
                for order in itertools.permutations(required + list(chosen)):
                    if _valid(order):
                        found = True
                        break
                if found:
                    break
            if found:
                break
        if not found:
            return False
    return True


def _valid(order):
    for i, a in enumerate(order):
        for b in order[i + 1 :]:
            if b.complete is not None and b.complete < a.invoke:
                return False
    state = None
    for o in order:
        matches, state = step(state, o)
        if not matches:
            return False
    return True


def test_sequential_and_concurrent_histories_are_accepted():
    history = [
        op(1, "put", 0, 10, value="a"),
        get(2, 5, 15, "a"),
        op(1, "append", 20, 30, value="b"),
        get(2, 25, 35, None),  # Impossible after the put; see the next test.
    ]
    assert not check_history(history).ok
    history[-1] = get(2, 25, 35, "a")  # Concurrent with the append: either order is fine.
    assert check_history(history).ok
    history.append(get(3, 40, 50, "ab"))
    assert check_history(history).ok


def test_stale_read_after_acknowledged_write_is_rejected_with_explanation():
    history = [
        op(1, "put", 0, 10, value="1"),
        op(1, "put", 20, 30, value="2"),
        get(2, 40, 50, "1"),
    ]
    verdict = check_history(history)
    assert not verdict.ok
    assert "cannot be placed" in verdict.explanation
    assert "get(k) -> '1'" in verdict.explanation


def test_info_writes_may_apply_late_or_never():
    timed_out = op(1, "put", 0, None, value="x")
    assert check_history([timed_out, get(2, 100, 110, None)]).ok
    assert check_history([timed_out, get(2, 100, 110, "x")]).ok
    # But not before it was invoked.
    assert not check_history([get(2, 0, 5, "x"), op(1, "put", 10, None, value="x")]).ok


def test_fail_operations_and_info_reads_are_ignored():
    history = [
        {**op(1, "put", 0, 10, value="x"), "result": "fail"},
        {**get(2, 0, 10, "zzz"), "result": "info"},
        get(3, 20, 30, None),
    ]
    assert check_history(history).ok


def test_cas_results_must_match_the_register():
    put = op(1, "put", 0, 10, value="a")
    ok = op(2, "cas", 20, 30, value="b", expected="a",
            output={"exchanged": True, "found": True, "previous": "a"})  # fmt: skip
    assert check_history([put, ok, get(3, 40, 50, "b")]).ok
    lie = {**ok, "output": {"exchanged": False, "found": True, "previous": "a"}}
    assert not check_history([put, lie]).ok
    missing = op(2, "cas", 20, 30, value="b", expected="",
                 output={"exchanged": False, "found": False, "previous": ""})  # fmt: skip
    assert check_history([missing]).ok


def test_keys_are_checked_independently():
    history = [op(1, "put", 0, 10, key="a", value="1"), get(2, 20, 30, None, key="b")]
    assert check_history(history).ok


def test_duplicate_application_of_an_append_is_detected():
    history = [
        op(1, "append", 0, 10, value="x;"),
        get(2, 20, 30, "x;x;"),
    ]
    assert not check_history(history).ok


def random_history(rng: random.Random) -> list[dict]:
    """Small concurrent histories, mostly from a real register, sometimes corrupted."""
    history = []
    state = None
    clock = 0
    for client in range(1, rng.randint(2, 4) + 1):
        start = rng.randint(0, 20)
        for _ in range(rng.randint(1, 2)):
            kind = rng.choice(["put", "append", "cas", "get", "get"])
            invoke = start + rng.randint(0, 10)
            complete = invoke + rng.randint(0, 15) if rng.random() > 0.15 else None
            start = (complete or invoke) + 1
            value = f"v{clock}"
            clock += 1
            entry = {"put": None, "append": None, "cas": None, "get": None}[kind]
            if kind == "get":
                observed = rng.choice([state, value, None, "v0", "v1"])
                entry = get(client, invoke, complete, observed)
                if complete is None:
                    entry["result"] = "info"
            elif kind == "cas":
                expected = rng.choice([state or "", "v0", "v1"])
                exchanged = rng.random() < 0.5
                entry = op(client, "cas", invoke, complete, value=value, expected=expected,
                           output={"exchanged": exchanged, "found": state is not None,
                                   "previous": state or ""})  # fmt: skip
            else:
                entry = op(client, kind, invoke, complete, value=value)
                state = value if kind == "put" else (state or "") + value
            history.append(entry)
    return history


@pytest.mark.parametrize("seed", range(3000))
def test_differential_against_brute_force(seed):
    history = random_history(random.Random(seed))
    ops = parse(history).get("k", [])
    assert linearizable(ops)[0] == brute_force(history), history


FIXTURES = pathlib.Path(__file__).with_name("fixtures")


def test_recorded_unsafe_reads_history_is_flagged_with_a_minimal_counterexample():
    """Recorded from the real cluster with --unsafe-reads (seed 2, 30 s, nemesis on)."""
    history = [json.loads(line) for line in (FIXTURES / "unsafe_reads_seed2.jsonl").open()]
    verdict = check_history(history)
    assert not verdict.ok
    lines = [line for line in verdict.explanation.splitlines() if line.startswith("  client")]
    assert len(lines) <= 6, verdict.explanation
    assert "cannot be placed" in verdict.explanation


def test_recorded_safe_history_from_the_same_harness_is_linearizable():
    """Recorded from the real cluster with ReadIndex reads (seed 1, 30 s, nemesis on)."""
    history = [json.loads(line) for line in (FIXTURES / "safe_seed1.jsonl").open()]
    assert check_history(history).ok

"""Wing-Gong / Lowe linearizability checker for quorumkv client histories.

    python -m linearizability.checker history.jsonl [more.jsonl ...]

Each history line is one operation:
    {"client", "op", "key", "value", "expected", "invoke_ts", "complete_ts", "result", "output"}
with op in put/append/cas/get and result in ok/fail/info.

The store is a map of independent registers, and linearizability is P-compositional, so every
key is checked on its own against a register model whose state is the value or None (missing):

    put(v)          state = v
    append(s)       state = (state or "") + s
    cas(e, n)       if state == e (and present): state = n, exchanged; else unchanged
    get             returns {found, value}; must equal the state

"fail" operations definitely had no effect and are dropped. "info" writes may take effect at
any point after their invocation, or never: they have no return event and need not be
linearized. "info" reads constrain nothing and are dropped.

The search is Lowe's iterative WGL: walk the call/return event list, linearize the first
call whose model step matches its recorded output, memoize (linearized set, state) pairs that
already failed, and backtrack at a return whose operation is not yet linearized. On failure
the history is shrunk greedily to a minimal subset that is still not linearizable.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import time
from dataclasses import dataclass, field

MISSING = None


@dataclass
class Operation:
    index: int
    client: int
    op: str
    key: str
    value: str | None
    expected: str | None
    invoke: int
    complete: int | None  # None: info, no return event
    output: dict = field(default_factory=dict)

    def describe(self) -> str:
        if self.op == "get":
            shown = repr(self.output["value"]) if self.output.get("found") else "missing"
            body = f"get({self.key}) -> {shown}"
        elif self.op == "cas":
            outcome = "?" if self.complete is None else self.output.get("exchanged")
            body = f"cas({self.key}, {self.expected!r} -> {self.value!r}) exchanged={outcome}"
        else:
            body = f"{self.op}({self.key}, {self.value!r})"
        end = "unknown" if self.complete is None else str(self.complete)
        return f"client {self.client} {body} [{self.invoke}, {end}]"


@dataclass
class Verdict:
    ok: bool
    explanation: str = ""
    keys: dict[str, int] = field(default_factory=dict)


def step(state, op: Operation):
    """Returns (matches_recorded_output, new_state)."""
    if op.op == "put":
        return True, op.value
    if op.op == "append":
        return True, (state or "") + op.value
    if op.op == "cas":
        exchanged = state is not MISSING and state == op.expected
        if op.complete is not None:
            out = op.output
            if (
                out.get("exchanged") != exchanged
                or out.get("found") != (state is not MISSING)
                or out.get("previous", "") != (state or "")
            ):
                return False, state
        return True, (op.value if exchanged else state)
    if op.op == "get":
        found = op.output.get("found", False)
        if found != (state is not MISSING):
            return False, state
        return (not found or op.output.get("value") == state), state
    raise ValueError(f"unknown operation {op.op}")


def parse(history: list[dict]) -> dict[str, list[Operation]]:
    by_key: dict[str, list[Operation]] = {}
    for index, raw in enumerate(history):
        result = raw["result"]
        if result == "fail" or (result == "info" and raw["op"] == "get"):
            continue
        if result not in ("ok", "info"):
            raise ValueError(f"unknown result {result!r}")
        op = Operation(
            index=index,
            client=raw["client"],
            op=raw["op"],
            key=raw["key"],
            value=raw.get("value"),
            expected=raw.get("expected"),
            invoke=raw["invoke_ts"],
            complete=raw["complete_ts"] if result == "ok" else None,
            output=raw.get("output") or {},
        )
        if op.complete is not None and op.complete < op.invoke:
            raise ValueError(f"operation completes before it is invoked: {op.describe()}")
        by_key.setdefault(op.key, []).append(op)
    return by_key


class _Event:
    __slots__ = ("is_call", "op", "slot", "prev", "next", "match")

    def __init__(self, is_call: bool, op: Operation, slot: int) -> None:
        self.is_call, self.op, self.slot = is_call, op, slot
        self.prev = self.next = self.match = None


def _linked(ops: list[Operation]) -> _Event:
    events = []
    for slot, op in enumerate(ops):
        call = _Event(True, op, slot)
        # Calls sort before returns at equal times: equal timestamps are concurrent.
        events.append((op.invoke, 0, call))
        if op.complete is not None:
            ret = _Event(False, op, slot)
            call.match, ret.match = ret, call
            events.append((op.complete, 1, ret))
    events.sort(key=lambda item: (item[0], item[1], item[2].slot))
    head = _Event(True, None, -1)
    tail = head
    for _, _, event in events:
        tail.next, event.prev = event, tail
        tail = event
    return head


def _lift(event: _Event) -> None:
    event.prev.next = event.next
    if event.next:
        event.next.prev = event.prev
    if event.match:
        ret = event.match
        ret.prev.next = ret.next
        if ret.next:
            ret.next.prev = ret.prev


def _unlift(event: _Event) -> None:
    if event.match:
        ret = event.match
        ret.prev.next = ret
        if ret.next:
            ret.next.prev = ret
    event.prev.next = event
    if event.next:
        event.next.prev = event


def linearizable(ops: list[Operation]):
    """Returns (True, None) or (False, blocking_operation).

    The blocking operation is the completed operation that stopped the deepest partial order:
    the search had linearized as much as it ever could, and nothing explained this one.
    """
    head = _linked(ops)
    deepest, blocking = -1, None
    remaining = sum(op.complete is not None for op in ops)
    state, bits = MISSING, 0
    cache: set[tuple[int, object]] = set()
    stack: list[tuple[_Event, object]] = []
    entry = head.next
    while remaining:
        if entry is None:
            # Only unlinearized info calls are left after this point; undo the last choice.
            if not stack:
                return False, blocking
            event, state = stack.pop()
            bits &= ~(1 << event.slot)
            remaining += event.op.complete is not None
            _unlift(event)
            entry = event.next
            continue
        if entry.is_call:
            matches, new_state = step(state, entry.op)
            if matches:
                new_bits = bits | (1 << entry.slot)
                if (new_bits, new_state) not in cache:
                    cache.add((new_bits, new_state))
                    stack.append((entry, state))
                    state, bits = new_state, new_bits
                    remaining -= entry.op.complete is not None
                    _lift(entry)
                    entry = head.next
                    continue
            entry = entry.next
        else:
            # A completed operation that nothing before its return could linearize.
            if len(stack) > deepest:
                deepest, blocking = len(stack), entry.op
            if not stack:
                return False, blocking
            event, state = stack.pop()
            bits &= ~(1 << event.slot)
            remaining += event.op.complete is not None
            _unlift(event)
            entry = event.next
    return True, None


def _prefix(ops: list[Operation], cut: int) -> list[Operation]:
    """Operations invoked by `cut`; any still running at `cut` become info (pending)."""
    result = []
    for op in ops:
        if op.invoke > cut:
            continue
        if op.complete is not None and op.complete > cut:
            if op.op == "get":
                continue  # A pending read constrains nothing.
            op = Operation(**{**op.__dict__, "complete": None})
        result.append(op)
    return result


def _observed(ops: list[Operation]) -> list[str]:
    """Register values that completed reads and CAS operations report having seen."""
    values = []
    for op in ops:
        if op.complete is None or not op.output.get("found"):
            continue
        if op.op == "get":
            values.append(op.output.get("value", ""))
        elif op.op == "cas":
            values.append(op.output.get("previous", ""))
    return values


def _protected(op: Operation, observed: list[str]) -> bool:
    """Writes whose value an observation shows must stay, or that observation is unexplained."""
    if op.op == "get" or op.value is None:
        return False
    if op.op == "append":
        return any(op.value in value for value in observed)
    # A put or exchanged CAS value, possibly followed by appended tokens (which start with c).
    return any(
        value == op.value or (value.startswith(op.value) and value[len(op.value)] == "c")
        for value in observed
    )


def minimize(ops: list[Operation], budget_s: float = 20.0) -> list[Operation]:
    """Shrinks a non-linearizable history to a small subset that still fails.

    Linearizability is prefix-closed, so the earliest failing cut is found by binary search,
    with operations pending at the cut treated as info. Operations are then dropped greedily
    until nothing more can go, except writes whose values remaining observations show, which
    would leave those observations unexplained.
    """
    times = sorted({op.invoke for op in ops} | {op.complete for op in ops if op.complete})
    low, high = 0, len(times) - 1
    while low < high:
        middle = (low + high) // 2
        if linearizable(_prefix(ops, times[middle]))[0]:
            low = middle + 1
        else:
            high = middle
    ops = _prefix(ops, times[low])
    deadline = time.monotonic() + budget_s
    changed = True
    while changed and time.monotonic() < deadline:
        changed = False
        index = 0
        while index < len(ops) and time.monotonic() < deadline:
            candidate = ops[:index] + ops[index + 1 :]
            if (
                candidate
                and not _protected(ops[index], _observed(candidate))
                and not linearizable(candidate)[0]
            ):
                ops = candidate
                changed = True
            else:
                index += 1
    return ops


def explain(key: str, ops: list[Operation]) -> str:
    lines = [f"key {key!r}: no order consistent with real time explains these operations:"]
    lines += ["  " + op.describe() for op in sorted(ops, key=lambda op: op.invoke)]
    _, failing = linearizable(ops)
    if failing is not None:
        before = [o for o in ops if o.complete is not None and o.complete < failing.invoke]
        states = sorted({repr(s) for s in _reachable_states(ops, failing)})
        lines.append(
            f"  {failing.describe()} cannot be placed: {len(before)} operation(s) finished "
            f"before it started, and every valid order leaves the register in one of "
            f"{', '.join(states) or 'no state'} at that point, none of which matches its result."
        )
    return "\n".join(lines)


def _reachable_states(ops: list[Operation], target: Operation) -> set:
    """Register values possible just before `target` in orders that respect real time.

    Every operation that finished before `target` started must already have taken effect.
    """
    others = [op for op in ops if op is not target and op.invoke < (target.complete or 0)]
    required = frozenset(
        op.index for op in others if op.complete is not None and op.complete < target.invoke
    )
    states = set()
    frontier = {(frozenset(), MISSING)}
    seen = set(frontier)
    while frontier and len(seen) < 50000:
        expanded = set()
        for done, state in frontier:
            if required <= done:
                states.add(state)
            for op in others:
                if op.index in done:
                    continue
                if any(
                    o.index not in done and o.complete is not None and o.complete < op.invoke
                    for o in others
                ):
                    continue
                matches, new_state = step(state, op)
                item = (done | {op.index}, new_state)
                if matches and item not in seen:
                    seen.add(item)
                    expanded.add(item)
        frontier = expanded
    return states


def check_history(history: list[dict]) -> Verdict:
    verdict = Verdict(ok=True)
    for key, ops in sorted(parse(history).items()):
        verdict.keys[key] = len(ops)
        if not linearizable(ops)[0]:
            minimal = minimize(ops)
            return Verdict(ok=False, explanation=explain(key, minimal), keys=verdict.keys)
    return verdict


def load(paths: list[pathlib.Path]) -> list[dict]:
    history = []
    for path in paths:
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                history.append(json.loads(line))
    return history


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("history", nargs="+", type=pathlib.Path)
    args = parser.parse_args()
    started = time.monotonic()
    verdict = check_history(load(args.history))
    seconds = time.monotonic() - started
    summary = {"linearizable": verdict.ok, "keys": verdict.keys, "seconds": round(seconds, 3)}
    print(json.dumps(summary))
    if not verdict.ok:
        print(verdict.explanation, file=sys.stderr)
    sys.exit(0 if verdict.ok else 1)


if __name__ == "__main__":
    main()

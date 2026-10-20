"""Generate one deliberately broken node for a test-only library; fail on source drift."""

import argparse
import pathlib

MUTATIONS = (
    "count_stale_votes",
    "reply_before_persist",
    "self_vote_after_send",
    "commit_old_term",
    "append_reply_before_persist",
    "double_vote",
    "two_of_five",
)


def once(source: str, old: str, new: str) -> str:
    if source.count(old) != 1:
        raise ValueError(f"mutation anchor must occur exactly once: {old!r}")
    return source.replace(old, new, 1)


def mutate(source: str, mutation: str) -> str:
    if mutation == "count_stale_votes":
        return once(
            source,
            "const bool current_term = message.term == hard_state_.term;",
            "const bool current_term = message.term <= hard_state_.term;",
        )
    if mutation == "reply_before_persist":
        reply = "  Send(message.from, VoteResponse{.granted = grant}, message.rpc_id);\n"
        source = once(source, reply, "")
        return once(source, "  if (grant) {", reply + "  if (grant) {")
    if mutation == "self_vote_after_send":
        save = "  Save({.term = hard_state_.term + 1, .voted_for = config_.id});"
        source = once(source, save, "  ++hard_state_.term;\n  hard_state_.voted_for = config_.id;")
        return once(
            source,
            "\n  }\n}\n\nvoid RaftNode::BecomeLeader()",
            "\n  }\n  Save(hard_state_);\n}\n\nvoid RaftNode::BecomeLeader()",
        )
    if mutation == "commit_old_term":
        return once(
            source,
            "candidate > commit_index_ && log_.term(candidate) == hard_state_.term",
            "candidate > commit_index_",
        )
    if mutation == "append_reply_before_persist":
        save = "  if (batch.truncate_from || !batch.append.empty()) {\n    Persist(batch);\n  }\n"
        source = once(source, save, "")
        return once(
            source,
            "  reply.match_index = expected;\n  respond();",
            "  reply.match_index = expected;\n  respond();\n" + save.rstrip(),
        )
    if mutation == "double_vote":
        return once(
            source,
            "(!hard_state_.voted_for || hard_state_.voted_for == message.from)",
            "(message.from != 0)",
        )
    if mutation == "two_of_five":
        anchor = "votes_.size() > config_.members.size() / 2"
        if source.count(anchor) != 2:
            raise ValueError("expected both election majority checks")
        return source.replace(anchor, "votes_.size() >= 2")
    raise ValueError(f"unknown mutation: {mutation}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("mutation", choices=MUTATIONS)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    text = mutate(args.source.read_text(encoding="utf-8"), args.mutation)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()

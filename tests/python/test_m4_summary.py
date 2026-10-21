import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
from summarize_m4 import kv_records, negative_records

LINE = (
    "[kv] first_seed=0 seeds=2 operations=40 acknowledged=40 retries=5 timeouts=4 "
    "not_leader=90 logged_reads=3 reads_served=12 reads_failed=1 applied_commands=120 "
    "crashes=30 partitions=16 steps=900 safety_checks=900 max_quiesce_ticks=54 digest=3e10\n"
)
NEGATIVES = (
    "[kv-negative] mode=unsafe_reads first_seed=14 error=Stale read: client 1 get(a0)\n"
    "[kv-negative] mode=no_dedupe first_seed=0 error=Exactly-once: key a0 applied twice\n"
)


def test_complete_campaign_parses():
    record = kv_records(LINE)[0]
    assert record["seeds"] == 2
    assert record["digest"] == "3e10"


@pytest.mark.parametrize(
    ("old", "new"),
    [
        ("acknowledged=40", "acknowledged=39"),  # An operation never finished.
        ("safety_checks=900", "safety_checks=899"),  # A step skipped the safety oracle.
        ("partitions=16", "partitions=15"),  # The nemesis schedule did not run.
        ("max_quiesce_ticks=54", "max_quiesce_ticks=1501"),  # Recovery exceeded its bound.
        ("digest=3e10", "digest=xyz"),
    ],
)
def test_incomplete_or_tampered_campaign_is_rejected(old, new):
    with pytest.raises(ValueError):
        kv_records(LINE.replace(old, new))


def test_unknown_fields_are_rejected():
    with pytest.raises(ValueError, match="fields"):
        kv_records(LINE.replace(" digest=", " extra=1 digest="))


def test_both_negative_controls_must_be_flagged_with_the_right_property():
    found = negative_records(NEGATIVES)
    assert found["unsafe_reads"]["first_seed"] == 14
    assert found["no_dedupe"]["error"].startswith("Exactly-once")
    with pytest.raises(ValueError, match="every"):
        negative_records(NEGATIVES.splitlines()[0])
    with pytest.raises(ValueError, match="did not flag"):
        negative_records(NEGATIVES.replace("error=Exactly-once", "error=Stale read"))
    with pytest.raises(ValueError, match="did not flag"):
        negative_records(NEGATIVES.replace("first_seed=14", "first_seed=1000"))
    with pytest.raises(ValueError, match="duplicate"):
        negative_records(NEGATIVES + NEGATIVES.splitlines()[0])

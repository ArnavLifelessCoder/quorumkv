import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
from summarize_m5 import longrun_record, negative_record, snapshot_records

SNAPSHOT = (
    "[kv-snapshot] first_seed=0 seeds=2 snapshots_taken=9 snapshot_installs=2 "
    "snapshot_chunks=7 restore_checks=11 max_retained_entries=40\n"
)
LONGRUN = (
    "[kv-longrun] ticks=3000 threshold=16 operations=4474 applied_commands=15885 "
    "snapshots_taken=975 max_retained_entries=20\n"
)
NEGATIVE = (
    "[kv-snapshot-negative] mode=sessionless_snapshot first_seed=1 "
    "error=replayed state differs from a snapshot of the same index\n"
)


def test_complete_records_parse():
    assert snapshot_records(SNAPSHOT)[0]["snapshot_installs"] == 2
    assert longrun_record(LONGRUN)["max_retained_entries"] == 20
    assert negative_record(NEGATIVE)["first_seed"] == 1


@pytest.mark.parametrize(
    ("old", "new"),
    [
        ("snapshot_installs=2", "snapshot_installs=0"),  # InstallSnapshot never completed.
        ("snapshot_chunks=7", "snapshot_chunks=2"),  # Never more than one chunk.
        ("restore_checks=11", "restore_checks=0"),  # Restores were never compared.
        ("snapshots_taken=9", "snapshots_taken=0"),
    ],
)
def test_campaign_without_snapshot_coverage_is_rejected(old, new):
    with pytest.raises(ValueError):
        snapshot_records(SNAPSHOT.replace(old, new))


@pytest.mark.parametrize(
    ("old", "new"),
    [
        ("max_retained_entries=20", "max_retained_entries=33"),  # Log grew past the bound.
        ("operations=4474", "operations=900"),  # Too short to show anything.
        ("snapshots_taken=975", "snapshots_taken=3"),
    ],
)
def test_unbounded_or_short_long_run_is_rejected(old, new):
    with pytest.raises(ValueError):
        longrun_record(LONGRUN.replace(old, new))


def test_negative_control_must_flag_within_the_search():
    with pytest.raises(ValueError):
        negative_record(NEGATIVE.replace("first_seed=1", "first_seed=1000"))
    with pytest.raises(ValueError):
        negative_record(NEGATIVE.split("error=")[0] + "error=Stale read: client 1 get(a0)\n")
    with pytest.raises(ValueError):
        negative_record("")

"""Tests for the session checks in host.py (IEC 62443-4-2 CR 3.8).

No broker or QuestDB needed: messages go straight into on_message().

    python -m grpc_tools.protoc -I ../../IoTEdge/proto --python_out=. ../../IoTEdge/proto/sparkplug_b.proto
    python -m unittest test_host -v
"""

import unittest
from unittest import mock

import host
import sparkplug_b_pb2 as sp

GROUP, NODE = "UCL", "device-test"
DT_UINT64, DT_FLOAT = 8, 9


class RecordingSink:
    def __init__(self):
        self.metrics = []
        self.events = []

    def metric(self, group, node, name, value, ts_ms, historical, msg_type):
        self.metrics.append((name, value, msg_type))

    def event(self, group, node, event, bd_seq, ts_ms):
        self.events.append((event, bd_seq))

    def flush(self):
        pass


class Msg:
    def __init__(self, kind, payload):
        self.topic = f"spBv1.0/{GROUP}/{kind}/{NODE}"
        self.payload = payload.SerializeToString()


def nbirth(bd_seq):
    p = sp.Payload(timestamp=1000, seq=0)
    if bd_seq is not None:
        p.metrics.add(name="bdSeq", datatype=DT_UINT64, long_value=bd_seq)
    p.metrics.add(name="temp", alias=2, datatype=DT_FLOAT, float_value=20.0)
    return Msg("NBIRTH", p)


def ndata(seq, value=21.0):
    p = sp.Payload(timestamp=2000, seq=seq)
    p.metrics.add(alias=2, datatype=DT_FLOAT, float_value=value)
    return Msg("NDATA", p)


def ndeath(bd_seq):
    p = sp.Payload(timestamp=3000)
    p.metrics.add(name="bdSeq", datatype=DT_UINT64, long_value=bd_seq)
    return Msg("NDEATH", p)


class HostTest(unittest.TestCase):
    def setUp(self):
        self.sink = RecordingSink()
        self.host = host.SparkplugHost(mock.Mock(), self.sink, "+", node_timeout_s=100)

    def send(self, *msgs):
        for m in msgs:
            self.host.on_message(None, None, m)

    def session(self, bd_seq):
        self.send(nbirth(bd_seq), ndata(1), ndeath(bd_seq))

    def ndata_count(self):
        return sum(1 for _, _, t in self.sink.metrics if t == "NDATA")

    def test_newer_bdseq_accepted(self):
        self.session(5)
        self.session(6)
        self.session(9)  # gap: failed connects skipped 7 and 8
        self.assertEqual([e for e, _ in self.sink.events],
                         ["BIRTH", "DEATH"] * 3)
        self.assertEqual(self.ndata_count(), 3)

    def test_wraps_255_to_0(self):
        self.session(255)
        self.session(0)
        self.assertNotIn("REJECTED", [e for e, _ in self.sink.events])

    def test_replayed_bdseq_rejected_and_data_dropped(self):
        self.session(5)
        self.session(5)  # same bdSeq again = replay
        self.assertEqual(self.sink.events[-1], ("REJECTED", 5))
        self.assertEqual(self.ndata_count(), 1)  # 2nd session's NDATA dropped

    def test_older_bdseq_rejected(self):
        self.session(10)
        self.send(nbirth(3), ndata(1))
        self.assertEqual(self.sink.events[-1], ("REJECTED", 3))
        self.assertEqual(self.ndata_count(), 1)

    def test_missing_bdseq_rejected(self):
        self.send(nbirth(None), ndata(1))
        self.assertEqual(self.sink.events, [("REJECTED", None)])
        self.assertEqual(self.ndata_count(), 0)

    def test_valid_birth_after_rejection_recovers(self):
        self.session(5)
        self.session(5)
        self.session(6)
        self.assertEqual(self.sink.events[-2:], [("BIRTH", 6), ("DEATH", 6)])
        self.assertEqual(self.ndata_count(), 2)

    def test_first_birth_after_host_start_accepted(self):
        self.session(42)  # host has no history: anything goes
        self.assertEqual(self.sink.events[0], ("BIRTH", 42))

    def test_timeout_marks_offline_and_needs_new_birth(self):
        with mock.patch.object(host.time, "monotonic", return_value=1000.0):
            self.send(nbirth(1), ndata(1))  # node dies: no NDEATH
        with mock.patch.object(host.time, "monotonic", return_value=1050.0):
            self.host.check_timeouts()
        self.assertEqual(self.sink.events[-1], ("BIRTH", 1))  # not yet
        with mock.patch.object(host.time, "monotonic", return_value=1101.0):
            self.host.check_timeouts()
            self.assertEqual(self.sink.events[-1], ("TIMEOUT", 1))
            self.send(ndata(2))  # data after the timeout, without NBIRTH
        self.assertEqual(self.ndata_count(), 1)

    def test_bdseq_is_newer(self):
        self.assertTrue(host.bdseq_is_newer(1, 0))
        self.assertTrue(host.bdseq_is_newer(0, 255))
        self.assertTrue(host.bdseq_is_newer(127, 0))
        self.assertFalse(host.bdseq_is_newer(128, 0))
        self.assertFalse(host.bdseq_is_newer(0, 0))
        self.assertFalse(host.bdseq_is_newer(4, 5))


if __name__ == "__main__":
    unittest.main()

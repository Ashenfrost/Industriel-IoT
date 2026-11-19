"""Sparkplug B host application: MQTT (Mosquitto) -> QuestDB.

Subscribes to spBv1.0/<group>/# and does what a Sparkplug "host" must do:
  * NBIRTH: learn alias -> name for the node, reset expected seq, mark online
  * NDATA:  check seq is last+1 (mod 256), resolve aliases, store metrics
            using each metric's own timestamp (so batched samples land at the
            time they were measured, not when they arrived)
  * NDEATH: mark offline, but only if its bdSeq matches the current birth
  * a gap in seq, or data from a node we never saw a birth from, makes us
    send NCMD "Node Control/Rebirth" = true

Session integrity (IEC 62443-4-2 CR 3.8):
  * an NBIRTH must carry a bdSeq NEWER than the node's last one (1..127 steps
    ahead, mod 256; gaps are fine because a failed connect skips a value).
    The same or an older bdSeq looks like a replayed session: it is logged,
    stored as a REJECTED event, and its data is dropped until a valid NBIRTH.
  * the cellular modem can't register NDEATH as MQTT Will, so a node that
    dies mid-session would stay "online" forever. A node silent for longer
    than NODE_TIMEOUT_S is marked offline (TIMEOUT event); its next data must
    start with a new NBIRTH.

Configuration is environment variables only (see README.md).
"""

import logging
import os
import signal
import ssl
import sys
import threading
import time
from dataclasses import dataclass, field

import paho.mqtt.client as mqtt

import sparkplug_b_pb2 as sp  # generated from IoTEdge/proto/sparkplug_b.proto at build

log = logging.getLogger("sparkplug-host")

NAMESPACE = "spBv1.0"
REBIRTH_METRIC = "Node Control/Rebirth"
DT_BOOLEAN = 11
REBIRTH_MIN_INTERVAL_S = 30  # don't flood a node that keeps misbehaving
BDSEQ_MAX_AHEAD = 127  # half of the 0..255 range counts as "newer"
TIMEOUT_CHECK_S = 30

METRICS_TABLE = "sparkplug_metrics"
EVENTS_TABLE = "sparkplug_events"


@dataclass
class NodeState:
    aliases: dict = field(default_factory=dict)  # alias -> metric name
    last_seq: int | None = None
    bd_seq: int | None = None
    online: bool = False
    last_rebirth_req: float = 0.0
    # Last accepted bdSeq. Unlike bd_seq it survives NDEATH, so a later
    # NBIRTH can be checked against it. None until the first birth we see.
    last_bd_seq: int | None = None
    rejected: bool = False  # current session failed the bdSeq check
    last_seen: float = 0.0  # time.monotonic() of the last message


# ---------------------------------------------------------------- storage


class QuestDBSink:
    """Writes rows to QuestDB over ILP (questdb python client)."""

    def __init__(self, conf: str):
        from questdb import Sender

        self._sender = Sender.from_conf(conf)
        self._sender.establish()

    def metric(self, group, node, name, value, ts_ms, historical, msg_type):
        from questdb import TimestampNanos

        self._sender.row(
            METRICS_TABLE,
            symbols={"group_id": group, "edge_node": node, "metric": name,
                     "msg_type": msg_type},
            columns={"value": float(value), "historical": bool(historical)},
            at=TimestampNanos(ts_ms * 1_000_000),
        )

    def event(self, group, node, event, bd_seq, ts_ms):
        from questdb import TimestampNanos

        self._sender.row(
            EVENTS_TABLE,
            symbols={"group_id": group, "edge_node": node, "event": event},
            columns={"bd_seq": -1 if bd_seq is None else int(bd_seq)},
            at=TimestampNanos(ts_ms * 1_000_000),
        )

    def flush(self):
        self._sender.flush()

    def close(self):
        self._sender.close()


class LogSink:
    """Used when QUESTDB_CONF is empty: prints what would be stored."""

    def metric(self, group, node, name, value, ts_ms, historical, msg_type):
        log.info("STORE %s/%s %s=%s ts=%d%s", group, node, name, value, ts_ms,
                 " (historical)" if historical else "")

    def event(self, group, node, event, bd_seq, ts_ms):
        log.info("EVENT %s/%s %s bdSeq=%s", group, node, event, bd_seq)

    def flush(self):
        pass

    def close(self):
        pass


# ---------------------------------------------------------------- helpers


def metric_value(m):
    """Numeric value of a metric, or None (null / string / unsupported)."""
    if m.is_null:
        return None
    kind = m.WhichOneof("value")
    if kind in ("int_value", "long_value", "float_value", "double_value"):
        return getattr(m, kind)
    if kind == "boolean_value":
        return 1.0 if m.boolean_value else 0.0
    return None


def now_ms():
    return int(time.time() * 1000)


def bdseq_is_newer(new, last):
    """True if bdSeq `new` comes after `last` (both 0..255, wrapping)."""
    return 1 <= (new - last) % 256 <= BDSEQ_MAX_AHEAD


def find_bdseq(p):
    """bdSeq metric of a payload as int, or None if missing."""
    for m in p.metrics:
        if m.name == "bdSeq":
            v = metric_value(m)
            return None if v is None else int(v)
    return None


# ---------------------------------------------------------------- host


class SparkplugHost:
    def __init__(self, client: mqtt.Client, sink, group_filter: str,
                 node_timeout_s: float = 0):
        self.client = client
        self.sink = sink
        self.group_filter = group_filter
        self.node_timeout_s = node_timeout_s  # 0 = no timeout
        self.nodes: dict[tuple[str, str], NodeState] = {}
        # paho calls on_message from its own thread, check_timeouts() runs
        # in the main thread: both change NodeState, so they take this lock.
        self.lock = threading.Lock()

    def node(self, group, node) -> NodeState:
        return self.nodes.setdefault((group, node), NodeState())

    # --- MQTT callbacks (paho v2 API)

    def on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.error("Broker refused connection: %s", reason_code)
            return
        topic = f"{NAMESPACE}/{self.group_filter}/#"
        client.subscribe(topic, qos=0)
        log.info("Connected, subscribed to %s", topic)

    def on_disconnect(self, client, userdata, flags, reason_code, properties):
        log.warning("Disconnected from broker (%s), paho will reconnect",
                    reason_code)

    def on_message(self, client, userdata, msg):
        parts = msg.topic.split("/")
        # spBv1.0/<group>/<type>/<node>[/<device>]
        if len(parts) < 4 or parts[0] != NAMESPACE:
            return
        group, msg_type, node = parts[1], parts[2], parts[3]
        if msg_type in ("NCMD", "DCMD"):
            return  # our own commands echo back to us

        payload = sp.Payload()
        try:
            payload.ParseFromString(msg.payload)
        except Exception as e:  # noqa: BLE001 - anything malformed
            log.error("Bad protobuf on %s: %s", msg.topic, e)
            return

        handler = {
            "NBIRTH": self.handle_nbirth,
            "NDATA": self.handle_ndata,
            "NDEATH": self.handle_ndeath,
        }.get(msg_type)
        if handler is None:
            log.debug("Ignoring %s (device-level messages not used)", msg_type)
            return
        try:
            with self.lock:
                self.node(group, node).last_seen = time.monotonic()
                handler(group, node, payload)
                self.sink.flush()
        except Exception:  # keep running whatever one message does
            log.exception("Failed to handle %s", msg.topic)

    # --- Sparkplug message types

    def handle_nbirth(self, group, node, p):
        st = self.node(group, node)
        ts = p.timestamp or now_ms()
        bd_seq = find_bdseq(p)

        if not self.check_bdseq(group, node, st, bd_seq, ts):
            return

        st.aliases.clear()
        st.bd_seq = bd_seq
        st.last_bd_seq = bd_seq
        st.rejected = False

        for m in p.metrics:
            if m.name == "bdSeq":
                continue
            if m.HasField("alias"):
                st.aliases[m.alias] = m.name
            if m.name.startswith("Node Control/"):
                continue
            value = metric_value(m)
            if value is not None:
                self.sink.metric(group, node, m.name, value,
                                 m.timestamp or ts, m.is_historical, "NBIRTH")

        st.last_seq = p.seq
        st.online = True
        if p.seq != 0:
            log.warning("%s/%s NBIRTH with seq=%d (expected 0)", group, node, p.seq)
        self.sink.event(group, node, "BIRTH", st.bd_seq, ts)
        log.info("%s/%s born: bdSeq=%s, %d alias(es)", group, node, st.bd_seq,
                 len(st.aliases))

    def check_bdseq(self, group, node, st, bd_seq, ts):
        """CR 3.8: accept an NBIRTH only with a new bdSeq. On rejection the
        node is offline until a valid NBIRTH, and its data is dropped."""
        if bd_seq is None:
            reason = "no bdSeq"
        elif st.last_bd_seq is None:
            # first birth since the host started: nothing to compare with
            log.info("%s/%s first NBIRTH seen (bdSeq %d)", group, node, bd_seq)
            return True
        elif bdseq_is_newer(bd_seq, st.last_bd_seq):
            return True
        else:
            reason = f"bdSeq {bd_seq} not newer than {st.last_bd_seq}"

        log.error("%s/%s NBIRTH rejected (%s): possible replayed session",
                  group, node, reason)
        st.online = False
        st.rejected = True
        self.sink.event(group, node, "REJECTED", bd_seq, ts)
        return False

    def handle_ndata(self, group, node, p):
        st = self.node(group, node)

        if st.rejected:
            log.warning("%s/%s NDATA from a rejected session dropped", group, node)
            return

        if not st.online:
            log.warning("%s/%s NDATA without a known NBIRTH", group, node)
            self.request_rebirth(group, node)
            return  # we can't resolve aliases, so the data is unusable

        expected = (st.last_seq + 1) % 256
        if p.seq != expected:
            log.warning("%s/%s seq gap: got %d, expected %d", group, node,
                        p.seq, expected)
            self.request_rebirth(group, node)
            # the values themselves are still fine (aliases + timestamps)
        st.last_seq = p.seq

        ts = p.timestamp or now_ms()
        for m in p.metrics:
            name = m.name or st.aliases.get(m.alias)
            if name is None:
                log.warning("%s/%s unknown alias %d", group, node, m.alias)
                self.request_rebirth(group, node)
                continue
            value = metric_value(m)
            if value is not None:
                self.sink.metric(group, node, name, value, m.timestamp or ts,
                                 m.is_historical, "NDATA")

    def handle_ndeath(self, group, node, p):
        st = self.node(group, node)
        bd_seq = find_bdseq(p)

        if st.rejected:
            # end of a rejected session: nothing to close, no DEATH event
            st.rejected = False
            return

        if st.bd_seq is not None and bd_seq is not None and bd_seq != st.bd_seq:
            # Will from an older session, published after the node already
            # reconnected. Acting on it would wrongly mark a live node dead.
            log.info("%s/%s stale NDEATH (bdSeq %d, current %d) ignored",
                     group, node, bd_seq, st.bd_seq)
            return

        st.online = False
        self.sink.event(group, node, "DEATH", bd_seq, p.timestamp or now_ms())
        log.info("%s/%s died (bdSeq=%s)", group, node, bd_seq)

    def check_timeouts(self):
        """Marks nodes offline that have been silent for node_timeout_s.
        Stands in for the MQTT Will the cellular modem can't send."""
        if self.node_timeout_s <= 0:
            return
        now = time.monotonic()
        with self.lock:
            for (group, node), st in self.nodes.items():
                if st.online and now - st.last_seen > self.node_timeout_s:
                    st.online = False
                    self.sink.event(group, node, "TIMEOUT", st.bd_seq, now_ms())
                    self.sink.flush()
                    log.warning("%s/%s silent for %.0f s: marked offline "
                                "(bdSeq=%s)", group, node, now - st.last_seen,
                                st.bd_seq)

    # --- commands

    def request_rebirth(self, group, node):
        st = self.node(group, node)
        if time.monotonic() - st.last_rebirth_req < REBIRTH_MIN_INTERVAL_S:
            return
        st.last_rebirth_req = time.monotonic()

        p = sp.Payload()
        p.timestamp = now_ms()
        m = p.metrics.add()
        m.name = REBIRTH_METRIC
        m.datatype = DT_BOOLEAN
        m.boolean_value = True

        topic = f"{NAMESPACE}/{group}/NCMD/{node}"
        self.client.publish(topic, p.SerializeToString(), qos=0, retain=False)
        log.info("Rebirth requested from %s/%s", group, node)


def main():
    logging.basicConfig(
        level=os.environ.get("LOG_LEVEL", "INFO"),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    host = os.environ.get("MQTT_HOST", "mosquitto")
    port = int(os.environ.get("MQTT_PORT", "1883"))
    group = os.environ.get("SPARKPLUG_GROUP", "+")
    questdb_conf = os.environ.get("QUESTDB_CONF", "")
    # Default: 2 x the firmware's 1 h publish interval + 5 min margin
    node_timeout_s = float(os.environ.get("NODE_TIMEOUT_S", "7500"))

    sink = QuestDBSink(questdb_conf) if questdb_conf else LogSink()
    if not questdb_conf:
        log.warning("QUESTDB_CONF not set: printing instead of storing")

    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=os.environ.get("MQTT_CLIENT_ID", "sparkplug-host"),
        protocol=mqtt.MQTTv311,
        clean_session=True,
    )
    if os.environ.get("MQTT_USERNAME"):
        client.username_pw_set(os.environ["MQTT_USERNAME"],
                               os.environ.get("MQTT_PASSWORD", ""))
    if os.environ.get("MQTT_TLS", "0") == "1":
        client.tls_set(ca_certs=os.environ.get("MQTT_CA_FILE") or None,
                       cert_reqs=ssl.CERT_REQUIRED)

    app = SparkplugHost(client, sink, group, node_timeout_s)
    client.on_connect = app.on_connect
    client.on_disconnect = app.on_disconnect
    client.on_message = app.on_message
    client.reconnect_delay_set(min_delay=1, max_delay=30)

    stopping = threading.Event()

    def stop(*_):
        log.info("Shutting down")
        stopping.set()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    # The broker container may still be starting: keep trying
    while True:
        try:
            client.connect(host, port, keepalive=60)
            break
        except OSError as e:
            log.warning("Broker %s:%d not reachable (%s), retrying", host, port, e)
            time.sleep(5)

    # paho runs the network loop (and reconnects) in its own thread; this
    # thread only checks for silent nodes.
    client.loop_start()
    while not stopping.wait(TIMEOUT_CHECK_S):
        app.check_timeouts()
    client.disconnect()
    client.loop_stop()
    sink.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

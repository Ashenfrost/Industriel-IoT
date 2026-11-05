"""Sparkplug B host application: MQTT (Mosquitto) -> QuestDB.

Subscribes to spBv1.0/<group>/# and does what a Sparkplug "host" must do:
  * NBIRTH: learn alias -> name for the node, reset expected seq, mark online
  * NDATA:  check seq is last+1 (mod 256), resolve aliases, store metrics
            using each metric's own timestamp (so batched samples land at the
            time they were measured, not when they arrived)
  * NDEATH: mark offline, but only if its bdSeq matches the current birth
  * a gap in seq, or data from a node we never saw a birth from, makes us
    send NCMD "Node Control/Rebirth" = true

Configuration is environment variables only (see README.md).
"""

import logging
import os
import signal
import ssl
import sys
import time
from dataclasses import dataclass, field

import paho.mqtt.client as mqtt

import sparkplug_b_pb2 as sp  # generated from proto/sparkplug_b.proto at build

log = logging.getLogger("sparkplug-host")

NAMESPACE = "spBv1.0"
REBIRTH_METRIC = "Node Control/Rebirth"
DT_BOOLEAN = 11
REBIRTH_MIN_INTERVAL_S = 30  # don't flood a node that keeps misbehaving

METRICS_TABLE = "sparkplug_metrics"
EVENTS_TABLE = "sparkplug_events"


@dataclass
class NodeState:
    aliases: dict = field(default_factory=dict)  # alias -> metric name
    last_seq: int | None = None
    bd_seq: int | None = None
    online: bool = False
    last_rebirth_req: float = 0.0


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


# ---------------------------------------------------------------- host


class SparkplugHost:
    def __init__(self, client: mqtt.Client, sink, group_filter: str):
        self.client = client
        self.sink = sink
        self.group_filter = group_filter
        self.nodes: dict[tuple[str, str], NodeState] = {}

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
            handler(group, node, payload)
            self.sink.flush()
        except Exception:  # keep running whatever one message does
            log.exception("Failed to handle %s", msg.topic)

    # --- Sparkplug message types

    def handle_nbirth(self, group, node, p):
        st = self.node(group, node)
        st.aliases.clear()
        st.bd_seq = None
        ts = p.timestamp or now_ms()

        for m in p.metrics:
            if m.name == "bdSeq":
                st.bd_seq = metric_value(m)
                st.bd_seq = None if st.bd_seq is None else int(st.bd_seq)
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

    def handle_ndata(self, group, node, p):
        st = self.node(group, node)

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
        bd_seq = next((int(metric_value(m)) for m in p.metrics
                       if m.name == "bdSeq" and metric_value(m) is not None),
                      None)

        if st.bd_seq is not None and bd_seq is not None and bd_seq != st.bd_seq:
            # Will from an older session, published after the node already
            # reconnected. Acting on it would wrongly mark a live node dead.
            log.info("%s/%s stale NDEATH (bdSeq %d, current %d) ignored",
                     group, node, bd_seq, st.bd_seq)
            return

        st.online = False
        self.sink.event(group, node, "DEATH", bd_seq, p.timestamp or now_ms())
        log.info("%s/%s died (bdSeq=%s)", group, node, bd_seq)

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

    app = SparkplugHost(client, sink, group)
    client.on_connect = app.on_connect
    client.on_disconnect = app.on_disconnect
    client.on_message = app.on_message
    client.reconnect_delay_set(min_delay=1, max_delay=30)

    def stop(*_):
        log.info("Shutting down")
        client.disconnect()

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

    client.loop_forever(retry_first_connection=True)
    sink.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

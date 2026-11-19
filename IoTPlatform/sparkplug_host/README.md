# Sparkplug B host (Mosquitto → QuestDB)

The third service in the cloud stack. Mosquitto only moves bytes; this
service is the Sparkplug B *host application* that understands them.

```
ESP32 ──MQTT──▶ mosquitto ──▶ sparkplug-host ──ILP/HTTP──▶ questdb
   ▲                                │
   └────────── NCMD (rebirth) ──────┘
```

## What it does

| Message | Host behaviour |
|---|---|
| `NBIRTH` | checks that `bdSeq` is newer than the node's last one (see below), stores alias → name map for that node, expects `seq = 0`, marks node online, logs a `BIRTH` event |
| `NDATA`  | checks `seq == last + 1 (mod 256)`, resolves aliases, stores every metric **with its own timestamp** |
| `NDEATH` | marks node offline, but ignores it if its `bdSeq` belongs to an older session |
| problems | `seq` gap, unknown alias or data before any birth → sends `NCMD` with `Node Control/Rebirth = true` (max once per 30 s per node) |
| silence  | no message for `NODE_TIMEOUT_S` → node marked offline, `TIMEOUT` event |

### Session integrity (IEC 62443-4-2 CR 3.8)

* **bdSeq must move forward.** An `NBIRTH` is only accepted if its `bdSeq` is
  1–127 steps after the node's last accepted one (mod 256, so 255 → 0 is fine;
  gaps are fine because a failed connect skips a value). The same or an older
  `bdSeq`, or none at all, looks like a replayed session: it is logged as an
  error, stored as a `REJECTED` event, and the session's `NDATA` is dropped
  until a valid `NBIRTH`. The firmware keeps `bdSeq` in flash, so a reboot
  continues the sequence. After a host restart the first `NBIRTH` per node
  is accepted, because there is nothing to compare with yet.
* **Timeout instead of Will.** The SIM7080G modem can't register the binary
  `NDEATH` as MQTT Will, so a node that dies mid-session would look online
  forever. A node silent for `NODE_TIMEOUT_S` is marked offline (`TIMEOUT`),
  and its next data must start with a new `NBIRTH`.

## QuestDB tables (created automatically on first write)

* `sparkplug_metrics`: `group_id`, `edge_node`, `metric`, `msg_type` (symbols), `value` (double), `historical` (bool), `timestamp`
* `sparkplug_events`: `group_id`, `edge_node`, `event` (`BIRTH`/`DEATH`/`TIMEOUT`/`REJECTED`), `bd_seq`, `timestamp`

```sql
SELECT timestamp, edge_node, value FROM sparkplug_metrics
WHERE metric = 'Temperature' ORDER BY timestamp DESC LIMIT 50;
```

## Configuration (environment variables)

| Variable | Default | |
|---|---|---|
| `MQTT_HOST` / `MQTT_PORT` | `mosquitto` / `1883` | broker on the compose network |
| `MQTT_USERNAME` / `MQTT_PASSWORD` | – | own broker user for the host |
| `MQTT_TLS`, `MQTT_CA_FILE` | `0` | set `1` when the broker listener uses TLS |
| `SPARKPLUG_GROUP` | `+` | which group(s) to subscribe to |
| `QUESTDB_CONF` | – | e.g. `http::addr=questdb:9000;` – if empty, rows are only logged |
| `NODE_TIMEOUT_S` | `7500` | silence before a node is marked offline: 2 × the 1 h publish interval + 5 min. `0` = off |
| `LOG_LEVEL` | `INFO` | |

## Running it

See `docker-compose.example.yml` for the service block. The Docker build
context must be the **repo root**, because the image compiles the same
`IoTEdge/proto/sparkplug_b.proto` the firmware uses. That way the firmware and cloud
can never disagree on the format.

Give the host its own broker user:

```
docker compose exec mosquitto mosquitto_passwd /mosquitto/config/passwd_file sparkplug-host
```

## Mosquitto ACL (recommended)

`acl_file /mosquitto/config/acl` in `mosquitto.conf`, then:

```
# Edge nodes may only publish their own node messages and read their NCMD.
# %c = MQTT client id (the firmware uses device-<mac> as client id and node id)
pattern write spBv1.0/+/NBIRTH/%c
pattern write spBv1.0/+/NDATA/%c
pattern write spBv1.0/+/NDEATH/%c
pattern read  spBv1.0/+/NCMD/%c

user sparkplug-host
topic read  spBv1.0/#
topic write spBv1.0/+/NCMD/#
```

The Will (NDEATH) is published by the broker on behalf of the client, and
still has to pass the client's write ACL. That is why NDEATH is in the list.

## Testing without the ESP32

`fake_edge_node.py` behaves like the firmware:

```
pip install paho-mqtt protobuf grpcio-tools
python -m grpc_tools.protoc -I ../../IoTEdge/proto --python_out=. ../../IoTEdge/proto/sparkplug_b.proto
python fake_edge_node.py --host <vps-ip> -u <user> -P <pw>
python fake_edge_node.py ... --skip-seq 2    # host logs a seq gap
python fake_edge_node.py ... --die 2         # session 2 dies, broker sends the Will
python fake_edge_node.py ... --die 2 --no-will   # like cellular: host times it out
python fake_edge_node.py ... --bdseq-start 5     # run twice: 2nd run is REJECTED
```

The session checks also have unit tests (no broker needed):

```
python -m unittest test_host -v
```

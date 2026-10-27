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
| `NBIRTH` | stores alias → name map for that node, expects `seq = 0`, marks node online, logs a `BIRTH` event |
| `NDATA`  | checks `seq == last + 1 (mod 256)`, resolves aliases, stores every metric **with its own timestamp** |
| `NDEATH` | marks node offline, but ignores it if its `bdSeq` belongs to an older session |
| problems | `seq` gap, unknown alias or data before any birth → sends `NCMD` with `Node Control/Rebirth = true` (max once per 30 s per node) |

## QuestDB tables (created automatically on first write)

* `sparkplug_metrics`: `group_id`, `edge_node`, `metric`, `msg_type` (symbols), `value` (double), `historical` (bool), `timestamp`
* `sparkplug_events`: `group_id`, `edge_node`, `event` (`BIRTH`/`DEATH`), `bd_seq`, `timestamp`

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
| `LOG_LEVEL` | `INFO` | |

## Running it

See `docker-compose.example.yml` for the service block. The Docker build
context must be the **repo root**, because the image compiles the same
`proto/sparkplug_b.proto` the firmware uses. That way the firmware and cloud
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
python -m grpc_tools.protoc -I ../../proto --python_out=. ../../proto/sparkplug_b.proto
python fake_edge_node.py --host <vps-ip> -u <user> -P <pw>
python fake_edge_node.py ... --skip-seq 3    # host should request a rebirth
python fake_edge_node.py ... --die-after 20  # host should log a DEATH
```

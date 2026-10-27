"""Fake Sparkplug B edge node, for testing the cloud side without an ESP32.

Behaves like the firmware: NDEATH Will with bdSeq, NBIRTH (seq 0) with
aliases, batched NDATA with historical flags, and answers rebirth requests.

    python fake_edge_node.py --host <vps-ip> -u esp32client -P <pw>
    python fake_edge_node.py --skip-seq 3     # drop a seq to test rebirth
    python fake_edge_node.py --die-after 30   # vanish without DISCONNECT

Needs: pip install paho-mqtt protobuf, and sparkplug_b_pb2.py
(protoc --python_out=. sparkplug_b.proto).
"""

import argparse
import os
import random
import time

import paho.mqtt.client as mqtt

import sparkplug_b_pb2 as sp

DT_UINT64, DT_FLOAT, DT_BOOLEAN = 8, 9, 11
ALIAS_REBIRTH, ALIAS_TEMP = 1, 2


def now_ms():
    return int(time.time() * 1000)


class FakeNode:
    def __init__(self, args):
        self.a = args
        self.seq = 0
        self.bd_seq = -1
        self.base = f"spBv1.0/{args.group}"
        self.rebirth = False
        self.connected = False

    def topic(self, t):
        return f"{self.base}/{t}/{self.a.node}"

    def ndeath(self):
        p = sp.Payload()
        m = p.metrics.add(name="bdSeq", datatype=DT_UINT64)
        m.long_value = self.bd_seq
        return p.SerializeToString()

    def nbirth(self, temp):
        self.seq = 0
        p = sp.Payload(timestamp=now_ms(), seq=self.next_seq())
        m = p.metrics.add(name="bdSeq", datatype=DT_UINT64, timestamp=now_ms())
        m.long_value = self.bd_seq
        m = p.metrics.add(name="Node Control/Rebirth", alias=ALIAS_REBIRTH,
                          datatype=DT_BOOLEAN, timestamp=now_ms())
        m.boolean_value = False
        m = p.metrics.add(name="Temperature", alias=ALIAS_TEMP,
                          datatype=DT_FLOAT, timestamp=now_ms())
        m.float_value = temp
        return p.SerializeToString()

    def ndata(self, samples):
        p = sp.Payload(timestamp=now_ms(), seq=self.next_seq())
        for i, (ts, temp) in enumerate(samples):
            m = p.metrics.add(alias=ALIAS_TEMP, timestamp=ts)
            m.float_value = temp
            if i < len(samples) - 1:
                m.is_historical = True
        return p.SerializeToString()

    def next_seq(self):
        s = self.seq
        self.seq = (self.seq + 1) % 256
        return s

    def on_connect(self, client, userdata, flags, rc, props):
        print("connected:", rc)
        client.subscribe(self.topic("NCMD"))
        self.rebirth = True  # send NBIRTH from the main loop
        self.connected = True

    def on_message(self, client, userdata, msg):
        p = sp.Payload()
        p.ParseFromString(msg.payload)
        for m in p.metrics:
            if (m.name == "Node Control/Rebirth" or m.alias == ALIAS_REBIRTH) \
                    and m.boolean_value:
                print("rebirth requested")
                self.rebirth = True

    def run(self):
        a = self.a
        self.bd_seq += 1
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=a.node,
                        protocol=mqtt.MQTTv311, clean_session=True)
        if a.username:
            c.username_pw_set(a.username, a.password)
        c.will_set(self.topic("NDEATH"), self.ndeath(), qos=1, retain=False)
        c.on_connect = self.on_connect
        c.on_message = self.on_message
        c.connect(a.host, a.port, keepalive=a.keepalive)
        c.loop_start()

        temp, batch, sent, start = 21.0, [], 0, time.time()
        while True:
            temp += random.uniform(-0.2, 0.2)
            batch.append((now_ms(), round(temp, 2)))

            if self.connected and self.rebirth:
                self.rebirth = False
                c.publish(self.topic("NBIRTH"), self.nbirth(temp))
                print("NBIRTH")

            if self.connected and len(batch) >= a.batch:
                sent += 1
                if a.skip_seq and sent == a.skip_seq:
                    self.next_seq()  # pretend one message got lost
                    print("skipping a seq number")
                c.publish(self.topic("NDATA"), self.ndata(batch))
                print(f"NDATA seq={(self.seq - 1) % 256} samples={len(batch)}")
                batch = []

            if a.die_after and time.time() - start > a.die_after:
                print("dying without DISCONNECT (broker should send NDEATH)")
                c.loop_stop()
                c.socket().close()  # no DISCONNECT packet -> Will fires
                os._exit(0)

            time.sleep(a.interval)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("-u", "--username")
    ap.add_argument("-P", "--password")
    ap.add_argument("--group", default="UCL")
    ap.add_argument("--node", default="device-fake000001")
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--batch", type=int, default=3)
    ap.add_argument("--keepalive", type=int, default=5)
    ap.add_argument("--skip-seq", type=int, default=0)
    ap.add_argument("--die-after", type=float, default=0)
    FakeNode(ap.parse_args()).run()


if __name__ == "__main__":
    main()

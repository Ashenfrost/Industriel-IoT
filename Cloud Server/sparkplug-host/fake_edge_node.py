"""Fake Sparkplug B edge node, for testing the cloud side without an ESP32.

Behaves like the firmware ("option A"): it samples every --sample seconds
and every --cycle seconds runs one short session:

    CONNECT (Will = NDEATH) -> NBIRTH -> NDATA x n -> NDEATH -> DISCONNECT

    python fake_edge_node.py --host <vps-ip> -u <user> -P <pw>
    python fake_edge_node.py ... --skip-seq 2    # lose a seq -> host warns
    python fake_edge_node.py ... --die 2         # 2nd session dies mid-way
                                                 # (broker publishes the Will)

Needs: pip install paho-mqtt protobuf grpcio-tools, then
python -m grpc_tools.protoc -I ../../proto --python_out=. ../../proto/sparkplug_b.proto
"""

import argparse
import random
import time

import paho.mqtt.client as mqtt

import sparkplug_b_pb2 as sp

DT_UINT64, DT_FLOAT, DT_BOOLEAN = 8, 9, 11
ALIAS_REBIRTH, ALIAS_TEMP = 1, 2
MAX_SAMPLES_PER_NDATA = 60


def now_ms():
    return int(time.time() * 1000)


class FakeNode:
    def __init__(self, args):
        self.a = args
        self.seq = 0
        self.bd_seq = -1
        self.base = f"spBv1.0/{args.group}"
        self.queue = []          # (timestamp_ms, temperature)
        self.last = None
        self.temp = 21.0

    def topic(self, t):
        return f"{self.base}/{t}/{self.a.node}"

    def next_seq(self):
        s = self.seq
        self.seq = (self.seq + 1) % 256
        return s

    def ndeath(self):
        p = sp.Payload()
        m = p.metrics.add(name="bdSeq", datatype=DT_UINT64)
        m.long_value = self.bd_seq
        return p.SerializeToString()

    def nbirth(self):
        self.seq = 0
        t = now_ms()
        p = sp.Payload(timestamp=t, seq=self.next_seq())
        m = p.metrics.add(name="bdSeq", datatype=DT_UINT64, timestamp=t)
        m.long_value = self.bd_seq
        m = p.metrics.add(name="Node Control/Rebirth", alias=ALIAS_REBIRTH,
                          datatype=DT_BOOLEAN, timestamp=t)
        m.boolean_value = False
        m = p.metrics.add(name="Temperature", alias=ALIAS_TEMP, datatype=DT_FLOAT)
        if self.last:
            m.timestamp, m.float_value = self.last
        else:
            m.timestamp, m.is_null = t, True
        return p.SerializeToString()

    def ndata(self, samples):
        p = sp.Payload(timestamp=now_ms(), seq=self.next_seq())
        for i, (ts, temp) in enumerate(samples):
            m = p.metrics.add(alias=ALIAS_TEMP, timestamp=ts)
            m.float_value = temp
            if i < len(samples) - 1:
                m.is_historical = True
        return p.SerializeToString()

    def session(self, number):
        a = self.a
        self.bd_seq = (self.bd_seq + 1) % 256
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=a.node,
                        protocol=mqtt.MQTTv311, clean_session=True)
        if a.username:
            c.username_pw_set(a.username, a.password)
        c.will_set(self.topic("NDEATH"), self.ndeath(), qos=1, retain=False)
        c.connect(a.host, a.port, keepalive=a.keepalive)
        c.loop_start()

        c.publish(self.topic("NBIRTH"), self.nbirth()).wait_for_publish(5)
        print(f"session {number}: NBIRTH bdSeq={self.bd_seq}")

        sent = 0
        while self.queue:
            chunk = self.queue[:MAX_SAMPLES_PER_NDATA]
            sent += 1
            if a.skip_seq and sent == a.skip_seq:
                self.next_seq()
                print("  skipping a seq number")
            c.publish(self.topic("NDATA"), self.ndata(chunk)).wait_for_publish(5)
            print(f"  NDATA seq={(self.seq - 1) % 256} samples={len(chunk)}")
            self.last = chunk[-1]
            self.queue = self.queue[len(chunk):]

            if a.die and number == a.die:
                print("  dying without DISCONNECT (broker should send NDEATH)")
                c.loop_stop()
                c.socket().close()
                return

        c.publish(self.topic("NDEATH"), self.ndeath()).wait_for_publish(5)
        c.disconnect()
        c.loop_stop()
        print("  NDEATH + disconnect")

    def run(self):
        a = self.a
        next_session = time.time()
        number = 0
        while number < a.sessions:
            if time.time() >= next_session:
                number += 1
                self.session(number)
                next_session += a.cycle
            self.temp += random.uniform(-0.2, 0.2)
            self.queue.append((now_ms(), round(self.temp, 2)))
            time.sleep(a.sample)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("-u", "--username")
    ap.add_argument("-P", "--password")
    ap.add_argument("--group", default="UCL")
    ap.add_argument("--node", default="device-fake000001")
    ap.add_argument("--sample", type=float, default=1.0, help="s between samples")
    ap.add_argument("--cycle", type=float, default=10.0, help="s between sessions")
    ap.add_argument("--sessions", type=int, default=3)
    ap.add_argument("--keepalive", type=int, default=5)
    ap.add_argument("--skip-seq", type=int, default=0)
    ap.add_argument("--die", type=int, default=0, help="session number to die in")
    FakeNode(ap.parse_args()).run()


if __name__ == "__main__":
    main()

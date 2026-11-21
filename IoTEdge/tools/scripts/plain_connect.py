"""Send an unencrypted MQTT CONNECT (no login) to host:port and report the reply.
On a TLS port the broker should drop it; on 1883 it answers CONNACK (refused, no login)."""
import socket, sys

host, port = sys.argv[1], int(sys.argv[2])
cid = b"ucl-plain-test"
var = b"\x00\x04MQTT\x04\x02\x00\x3c"           # MQTT 3.1.1, clean session, keepalive 60
payload = len(cid).to_bytes(2, "big") + cid
pkt = b"\x10" + bytes([len(var) + len(payload)]) + var + payload
s = socket.create_connection((host, port), timeout=10)
s.sendall(pkt)
try:
    data = s.recv(16)
    if not data:
        print(f"{port}: connection closed by broker, no reply")
    elif data[0] == 0x20:
        print(f"{port}: CONNACK, return code {data[3]} (plain MQTT accepted on this port)")
    else:
        print(f"{port}: unexpected reply {data.hex()}")
except (ConnectionResetError, socket.timeout) as e:
    print(f"{port}: {type(e).__name__} (no MQTT reply)")
s.close()

import json, math, os
R = os.path.dirname(os.path.abspath(__file__))
s1, s2, s3 = (json.load(open(f"{R}/seg_test{k}.json")) for k in (1, 2, 3))
p4 = json.load(open(f"{R}/seg_test4.json"))["phases"]
ph = {k: v["avg_ma"] for k, v in p4.items()}

def extra(s):  # session charge above the idle level it replaces (mC)
    return s["session_q_mc"] - s["idle_ma"] * s["session_dur_s"]

e1, e2, e3 = extra(s1), extra(s2), extra(s3)
samples2 = 300 / 10
print("== recordings (average over 15 min)")
for n, s in (("1 baseline", s1), ("2 batching 5 min", s2), ("3 no batching", s3)):
    print(f"  test {n:18s} {s['recording_avg_ma']:6.1f} mA  idle {s['idle_ma']:5.1f} mA  "
          f"active {s['session_dur_s']:4.1f} s / {s['session_q_mc']:5.0f} mC  extra {extra(s):5.0f} mC")
print("== transmission energy per sample (above idle)")
print(f"  test 1 (stay connected):   {e1:6.0f} mC per sample")
print(f"  test 3 (session/sample):   {e3:6.0f} mC per sample")
print(f"  test 2 (30 per session):   {e2 / samples2:6.1f} mC per sample  ({e2:.0f} mC per session)")
print(f"  batching saves {(1 - (e2 / samples2) / e3) * 100:.0f} % vs test 3")
print("== test 4 states (whole system, mA)")
for k in ("boot", "P1", "P2", "P3", "P4", "P5", "deep"):
    print(f"  {k:5s} {ph[k]:6.2f}")
d = {
    "ESP busy vs idle (P1-P2)": ph["P1"] - ph["P2"],
    "ESP awake idle vs light sleep, modem on (P2-P3)": ph["P2"] - ph["P3"],
    "ESP awake idle vs light sleep, radio off (P5-P4)": ph["P5"] - ph["P4"],
    "modem registered vs radio off, ESP sleeping (P3-P4)": ph["P3"] - ph["P4"],
    "modem registered vs radio off, ESP idle (P2-P5)": ph["P2"] - ph["P5"],
    "light sleep vs deep sleep (P4-deep)": ph["P4"] - ph["deep"],
    "Wi-Fi + Modbus (test 2 idle - P2), estimate": s2["idle_ma"] - ph["P2"],
}
print("== differences (mA)")
for k, v in d.items():
    print(f"  {k:52s} {v:6.2f}")

# --- 1 h interval: estimate a 360-sample session from tests 2/3
per_msg = (e2 - e3) / (5 - 1)                     # ~extra per additional NDATA
e_1h = e3 + (math.ceil(360 / 7) - 1) * per_msg
attach = (ph["boot"] - ph["P2"]) * p4["boot"]["dur_s"]   # re-attach after radio off
print(f"== 1 h session estimate: {per_msg:.0f} mC per extra NDATA, {e_1h:.0f} mC per hourly session, "
      f"re-attach ~{attach:.0f} mC")
T = 3600.0
scen = [
    ("A today: ESP awake, Wi-Fi+Modbus, modem registered", s2["idle_ma"] + e_1h / T),
    ("B as A, modem radio off between sessions", s2["idle_ma"] - (ph["P2"] - ph["P5"]) + (e_1h + attach) / T),
    ("C floor: ESP light sleep, radio off (sampling cost not included)", ph["P4"] + (e_1h + attach) / T),
    ("D ESP light sleep, modem registered (sampling not incl.)", ph["P3"] + e_1h / T),
]
print("== average current at 1 h interval (battery 2000 mAh, 80 % usable)")
for n, i in scen:
    h = 2000 * 0.8 / i
    print(f"  {n:62s} {i:6.2f} mA  {h:6.0f} h ({h / 24:4.1f} d)")

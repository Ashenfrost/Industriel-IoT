"""Like at.py, but replaces {HOST} {USER} {PASS} with the values from
IoTEdge/src/mqtt_settings.h and masks them again in the printed output,
so broker address and login never show up in logs or chats.

usage: python at2.py COM6 'AT+SMCONF="URL","{HOST}",8883' 'AT+SMCONF="PASSWORD","{PASS}"' ...
"""
import os
import re
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
hdr = open(os.path.join(here, "..", "..", "src", "mqtt_settings.h"), encoding="utf-8").read()


def val(name):
    m = re.search(r'^\s*#define\s+' + name + r'\s+"?([^"\r\n]*?)"?\s*(//.*)?$', hdr, re.M)
    return m.group(1)


subs = {"{HOST}": val("MQTT_BROKER_HOSTNAME"), "{USER}": val("MQTT_ESP32_USER"),
        "{PASS}": val("MQTT_ESP32_PASS")}

cmds = []
for c in sys.argv[2:]:
    for k, v in subs.items():
        c = c.replace(k, v)
    cmds.append(c)

out = subprocess.run([sys.executable, os.path.join(here, "at.py"), sys.argv[1], *cmds],
                     capture_output=True, text=True).stdout
for k, v in subs.items():
    if v:
        out = out.replace(v, k)
print(out)

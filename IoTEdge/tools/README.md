# IoTEdge/tools

Test tools for the ESP32-C6 + SIM7080G (M5Stamp CatM) setup. None of this is
part of the normal firmware build in `IoTEdge/`.

Build and flash like the firmware, but with a separate build folder so the
normal build isn't overwritten (from `C:\Users\<you>\zephyrproject`, venv active):

```powershell
west build -p always -d build-atbridge -b esp32c6_devkitc/esp32c6/hpcore <repo>\IoTEdge\tools\at_bridge -S espressif-flash-4M
west flash -d build-atbridge --esp-device COM6
```

Run `west` from PowerShell, not Git Bash.

## Firmware tools

| Folder | What it does |
|---|---|
| `at_bridge/` | Forwards everything between the USB console and the modem (UART1, TX=GPIO23, RX=GPIO22). Lets you send AT commands from the PC with `scripts/at.py`. |
| `modem_time_test/` | Runs `modem_get_utc_ms()` (network clock and NTP via the modem) and `time_sync` with the modem as source, and logs the results. |
| `modem_tls_test/` | Connects to our broker 4 ways and checks the result: TLS with our CA (must connect), TLS with a wrong CA (must fail), plain on 8883 (must fail), plain on 1883 (must connect). Nothing is published. |

`modem_time_test` and `modem_tls_test` compile the real `modem.c`, `at_parser.c` and
`time_sync.c` from `IoTEdge/src`, so they test the actual code, not copies.
`modem_tls_test` also uses `mqtt_settings.h` (broker + login) and `mqtt_ca_cert.h`.
`src/test_certs.h` holds public test certificates only (a throw-away "wrong" CA).

## Scripts (`scripts/`, need `pyserial` from the Zephyr venv)

| Script | Use |
|---|---|
| `capture.py COM6 120 out.log` | Record the USB console without resetting the board. Opens with DTR/RTS off, see below. |
| `at.py COM6 "AT" "ATI" "WAIT=5" "AT+CNTP\|30"` | Send AT commands through `at_bridge`, wait for OK/ERROR. `WAIT=n` listens for URCs, `\|n` sets a timeout, `CMD@<hex>` waits for `>` and sends raw bytes. |
| `at2.py COM6 'AT+SMCONF="URL","{HOST}",8883'` | Like `at.py`, fills in `{HOST}` `{USER}` `{PASS}` from `mqtt_settings.h` and masks them in the output. |
| `pem2h.py out.h NAME=cert.pem` | Turns a PEM certificate into a C string `#define` (as in `mqtt_ca_cert.h`). |
| `plain_connect.py <host> 8883` | Sends an unencrypted MQTT CONNECT, to check that a TLS port rejects it. |

## Gotchas found the hard way

- **Download mode:** opening the C6's USB-Serial/JTAG with DTR/RTS asserted (pyserial's
  default) resets it into the ROM bootloader (`boot:0x4 ... waiting for download`). Only
  unplugging helps. `capture.py` and `at.py` open with DTR/RTS off. Reset safely with
  `python -m esptool --chip esp32c6 -p COM6 --after hard-reset chip-id`.
- **Bytes lost after `>`:** in TLS mode the SIM7080 loses the first bytes if data follows
  the `>` prompt immediately. `at_parser.c` waits `AT_PROMPT_DELAY_MS` (20 ms). The bridge
  doesn't show the problem because USB adds a few ms anyway.
- **No binary MQTT Will:** the SIM7080 only takes the Will message as a quoted string
  (`SMPUBHEX` and `MESSAGELEN` don't help), so a Sparkplug NDEATH can't be the Will.
- **Modem settings persist:** `AT+SMCONF`/`AT+CSSLCFG` values stay in the modem across
  ESP resets. `modem_mqtt_connect()` sets every field each time; when testing by hand,
  check `AT+SMCONF?` first.

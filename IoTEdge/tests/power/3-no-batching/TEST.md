# Test 3: Awake-cyklus uden batching

Branch: `power-test/3-no-batching`. Fælles opkobling, PPK2-indstillinger og
resultatskema: se [`../README.md`](../README.md).

## Formål
At måle strømforbruget for IoT Edge (ESP32-C6 + SIM7080G), når hver måling
sendes for sig i sin egen session (forbind → send → afbryd hvert 10. sekund).
Sammen med test 2 viser det, hvad batching sparer.

## Forudsætninger
- Firmware fra denne branch. Den er sat op til:
  - samme option A-session som test 2, men `APP_PUBLISH_INTERVAL_S = 10`:
    **én session pr. måling** (NBIRTH + én NDATA med 1 måling + NDEATH)
  - ingen sleep: ESP'en er vågen, Wi-Fi forbundet, Modbus hvert 5. s
  - `APP_POWER_MARKERS = 1`
- SimPLC tændt og på samme hotspot; Edge bygget med SimPLC'ens IP
- PPK2 i **Ampere Meter**-tilstand, måling A. Gerne også B og C

Bemærk: en session tager typisk 1-3 s, så modemet når kun kort at hvile
mellem sessionerne. Tager en session over 10 s (dårligt signal), starter den
næste med det samme. Noter det, hvis det sker.

## Byg og flash
```powershell
west build -p always -b esp32c6_devkitc/esp32c6/hpcore <repo>\IoTEdge -S espressif-flash-4M -- -DEXTRA_CFLAGS=-DMODBUS_SERVER_IP_RAW=<simplc-ip>
west flash --esp-device COM6
```

## Fremgangsmåde
1. Flash og kontrollér i konsollen at der kommer en session ca. hvert 10. s
   (`NDATA sent: 1 sample(s)` + `Session done`). Tag USB ud.
2. Kobl op som måling A, tænd, vent ca. 1 min.
3. Optag **10 minutter** (ca. 60 sessioner), 10 kS/s.
4. Aflæs:
   - gennemsnit over hele optagelsen (mA)
   - for 5 tilfældige sessioner: varighed (s) og **ladning (mC)** (D0 høj → lav)
   - gennemsnit mellem to sessioner (mA)

## Mål
- Gennemsnitsstrøm (mA) når hver måling sendes for sig
- **Q_session** for en session med 1 måling (mC), til sammenligning med test 2

## Succeskriterier
- Gennemsnit for hele optagelsen og Q_session (middel af 5 sessioner) er
  aflæst og kan sammenlignes med baseline (test 1) og batching (test 2).
- Ud fra test 2 og 3 kan vi regne: energi pr. måling med og uden batching
  (`Q_session / antal målinger i sessionen`).

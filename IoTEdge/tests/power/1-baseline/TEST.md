# Test 1: Baseline uden strømbesparende tiltag

Branch: `power-test/1-baseline`. Fælles opkobling, PPK2-indstillinger og
resultatskema: se [`../README.md`](../README.md).

## Formål
At give et baseline-strømforbrug for IoT Edge (ESP32-C6 + SIM7080G), når
intet er gjort for at spare strøm. Alle andre tests sammenlignes med den.

## Forudsætninger
- Firmware fra denne branch. Den er sat op til:
  - `APP_PERSISTENT_SESSION = 1`: forbinder til brokeren **én gang og bliver forbundet**
  - hver måling (hvert 10. s) sendes **straks** som sin egen NDATA (ingen batching)
  - ingen sleep: ESP'en er vågen hele tiden, Wi-Fi forbundet, Modbus poller SimPLC hvert 5. s
  - modemet registreret, datakontekst (PDP) og MQTT/TLS-forbindelse oppe hele tiden
  - `APP_POWER_MARKERS = 1`: fasemarkører på D0-D3
- SimPLC tændt og på samme hotspot; Edge bygget med SimPLC'ens IP (se nedenfor)
- PPK2 i **Ampere Meter**-tilstand, måling A (hele systemet). Gerne også B og C

## Byg og flash
```powershell
cd C:\Users\<dig>\zephyrproject
west build -p always -b esp32c6_devkitc/esp32c6/hpcore <repo>\IoTEdge -S espressif-flash-4M -- -DEXTRA_CFLAGS=-DMODBUS_SERVER_IP_RAW=<simplc-ip>
west flash --esp-device COM6
```
SimPLC'ens IP står i dens seriel-log ved opstart (`IPv4 address acquired: ...`).

## Fremgangsmåde
1. Flash, kontrollér i konsollen at du ser `Persistent session up` og at
   `NDATA sent: 1 sample(s)` kommer hvert 10. sekund. Tag USB ud.
2. Kobl op som måling A, tænd forsyningen, vent til D0 (SESSION) er gået **høj
   og bliver høj** (forbindelsen er oppe).
3. Optag **10 minutter** (10 kS/s er nok).
4. Aflæs og noter (resultatskema i README):
   - **gennemsnit over hele optagelsen** (mA): det er baseline
   - max (mA)
   - én publish (D1 høj): varighed og ladning
   - én måling + Modbus-poll (D2/D3 høj): ladning
   - strøm i "rolige" perioder mellem publishes (mA)
5. Gentag som måling B (kun ESP) og C (kun modem), hvis tid.

## Mål
Baseline-strøm i mA (gennemsnit), samt fordelingen ESP vs. modem (måling B/C).

## Succeskriterier
- Optagelsen dækker mindst 10 min uden genforbindelser (D0 er høj hele tiden).
- Der er en gennemsnitsværdi i mA for måling A (og gerne B og C), som de andre
  tests kan sammenlignes med.
- Antallet af publishes i optagelsen passer (ca. 60 på 10 min); ellers er
  måleperioden ikke repræsentativ.

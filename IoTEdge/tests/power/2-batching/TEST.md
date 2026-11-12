# Test 2: Awake-cyklus med batching

Branch: `power-test/2-batching`. Fælles opkobling, PPK2-indstillinger og
resultatskema: se [`../README.md`](../README.md).

## Formål
At måle strømforbruget for IoT Edge (ESP32-C6 + SIM7080G), mens den kører
vores normale cyklus ("option A"): målinger samles i en kø og sendes samlet i
én kort session, hvorefter forbindelsen og datakonteksten lukkes.

## Forudsætninger
- Firmware fra denne branch. Den er sat op til:
  - option A-sessioner: forbind → NBIRTH → alle købte målinger som NDATA → NDEATH → afbryd → PDP ned
  - `APP_PUBLISH_INTERVAL_S = 300` (én session hvert 5. minut, så en måling
    på 20 min indeholder 4 sessioner; i drift er det 3600)
  - måling hvert 10. s, dvs. 30 målinger pr. session (5 NDATA-beskeder à 7)
  - ingen sleep: ESP'en er vågen mellem sessionerne, Wi-Fi forbundet, Modbus hvert 5. s
  - `APP_POWER_MARKERS = 1`
- SimPLC tændt og på samme hotspot; Edge bygget med SimPLC'ens IP
- PPK2 i **Ampere Meter**-tilstand, måling A. Gerne også B og C

## Byg og flash
```powershell
west build -p always -b esp32c6_devkitc/esp32c6/hpcore <repo>\IoTEdge -S espressif-flash-4M -- -DEXTRA_CFLAGS=-DMODBUS_SERVER_IP_RAW=<simplc-ip>
west flash --esp-device COM6
```

## Fremgangsmåde
1. Flash og kontrollér i konsollen at sessionerne kører (`Session done,
   disconnected` hvert 5. min, med ca. `NDATA sent: 7 sample(s)` × 4-5). Tag USB ud.
2. Kobl op som måling A, tænd, vent til den første session (lige efter opstart)
   er overstået.
3. Optag **mindst 20 minutter** (≥ 4 sessioner), 10 kS/s.
4. For **hver session** (D0 høj → lav) aflæs:
   - varighed (s), **ladning (mC)**, gennemsnit og max (mA)
   - antal publishes (D1-pulser) og ladning for én publish
5. For perioden **mellem to sessioner** aflæs gennemsnit (mA).
6. Aflæs gennemsnit over **hele optagelsen** fra start af en session til start
   af en session 4 cyklusser senere (så det er hele cyklusser).
7. Gerne: noter hvor lang tid modemet bruger på at registrere og få PDP op
   i starten af sessionen (fra D0 går høj til første D1-puls).

## Mål
- **Q_session** (mC pr. session) og **t_session** (s)
- **I_mellem** (mA mellem sessioner, vågen)
- Gennemsnit pr. cyklus (mA), til sammenligning med test 1

## Succeskriterier
- Mindst 4 hele sessioner er optaget og aflæst; spredningen på Q_session
  mellem dem er noteret (så vi ved hvor sikkert tallet er).
- Værdierne kan indsættes i formlen i README'en og sammenlignes med baseline
  (test 1) og med test 3.

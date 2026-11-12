# Strømmålinger med PPK2

Målet er tal, vi kan bruge som guideline til den endelige sleep/wake-cyklus og
til at optimere strømforbruget på IoT Edge (ESP32-C6 + SIM7080G CatM-modem).

Der er fire tests. Hver test har sin egen branch med firmware sat op til netop
den test og en `TEST.md` med fremgangsmåden:

| # | Test | Branch | Hvad den giver os |
|---|---|---|---|
| 1 | Baseline (ingen strømbesparelse) | `power-test/1-baseline` | Strøm når alt altid er tændt og forbundet: referencen alt sammenlignes med |
| 2 | Awake-cyklus **med** batching | `power-test/2-batching` | Ladning (mC) pr. session + strøm mellem sessioner (option A, som vores design) |
| 3 | Awake-cyklus **uden** batching | `power-test/3-no-batching` | Ladning pr. session når hver måling sendes for sig (session hvert 10. s) |
| 4 | Sleep-tilstande | `power-test/4-sleep` | Strøm i hver søvntilstand for ESP og modem: gulvet vi kan komme ned på |

Test 2 og 3 måler det samme på to måder, så vi kan se hvad batching sparer.
Test 4 viser hvor lavt vi kan komme mellem målinger og sessioner.
Sammen giver de tallene til beregningen nederst.

---

## Udstyr

- Nordic **Power Profiler Kit II (PPK2)** + micro-USB-kabel (data/power)
- PC med **nRF Connect for Desktop** og appen **Power Profiler**
- **5 V-forsyning til testobjektet**: helst laboratorieforsyning, ellers en USB-oplader/powerbank (ikke PC'ens USB) med et USB-kabel hvor 5 V og GND er ført ud
- Jumperledninger (korte!) og evt. et breadboard til fordeling af 5 V/GND
- ESP32-C6 DevKitC + M5Stamp CatM (SIM7080G) med de *nye* ledninger (se nedenfor)
- SimPLC og telefon-hotspot (test 1-3 bruger Modbus over Wi-Fi som i drift)

## Vigtige forbehold (læs før opkobling)

1. **Brug Ampere Meter-tilstand**, ikke Source Meter. Modemet trækker korte
   strømspidser når det sender på LTE-M. Source Meter tåler kun **600 mA**,
   Ampere Meter **1 A** (PPK2 User Guide, tabel 7).
2. **USB-kablet til ESP'en skal være taget ud under målingen.** Ellers forsyner
   USB ESP'en uden om PPK2, og målingen er forkert. Flash først, tag så USB ud.
   Der er ingen konsol under målingen; det er derfor firmwaren har
   **fasemarkører** på logikporten (se nedenfor).
3. **Fælles GND** mellem PPK2, forsyning, ESP og modem (User Guide, Troubleshooting:
   "Ensure that the DUT ground is connected to the PPK2 even in ampere meter mode").
4. **Ledningerne til modemet**: de gamle jumperledninger gav bitfejl på UART'en
   (se commit `ab16e86`). Brug de nye, korte ledninger.
5. **DevKit'ets egne LED'er og spændingsregulator** bruger strøm hele tiden.
   Det er en del af målingen; noter det i resultaterne. Det er det samme i alle
   tests, så sammenligningerne holder.
6. **Tilbageforsyning via UART**: hvis kun den ene enhed er forsynet (måling B/C
   nedenfor), kan strøm løbe ind via TX/RX-benene. Hold begge enheder forsynet.

## Opkobling (Ampere Meter)

PPK2 sidder i serie i plus-ledningen: forsyning → **VIN** (PPK2) → **VOUT**
(PPK2) → testobjekt. GND går direkte fra forsyning til testobjekt og til PPK2's GND.

Vi måler i op til tre konfigurationer. **Måling A er obligatorisk**, B og C er
for at kunne skille ESP og modem ad:

| Måling | Hvad sidder efter PPK2's VOUT | Hvad sidder direkte på forsyningen |
|---|---|---|
| **A: hele systemet** | ESP DevKit 5V-ben **og** modem 5V | - |
| **B: kun ESP32-C6** | ESP DevKit 5V-ben | modem 5V |
| **C: kun modem** | modem 5V | ESP DevKit 5V-ben |

```
            5 V forsyning
             +        GND
             |         |
          PPK2 VIN     +-------------------+-------------+
          PPK2 VOUT    |                   |             |
             |      PPK2 GND        ESP DevKit GND   Modem GND
             +-----> (måling A: ESP 5V + modem 5V)
```

Signalledninger som i drift: ESP GPIO23 → modem RX, ESP GPIO22 ← modem TX.

### Logikport (fasemarkører)

| PPK2 logikport | ESP32-C6 | Betydning (test 1-3) | Betydning (test 4) |
|---|---|---|---|
| VCC | 3V3 | reference for logikniveau | |
| GND | GND | | |
| D0 | GPIO18 | **SESSION**: uplink-session (attach, PDP, tid, MQTT, publish) | fase-nr. bit 0 |
| D1 | GPIO19 | **PUBLISH**: én MQTT publish (AT+SMPUB) | fase-nr. bit 1 |
| D2 | GPIO20 | **SAMPLE**: måling tages | fase-nr. bit 2 |
| D3 | GPIO21 | **MODBUS**: én Modbus-poll af SimPLC | (ikke brugt) |

Markørerne er slået til på test-branchene (`APP_POWER_MARKERS=1`) og fra i
den normale firmware.

## Power Profiler-indstillinger

- Mode: **Ampere meter**
- Sampling: **100 kS/s** til korte optagelser (< 5 min), **10 kS/s** til lange
  (op til en time); lavere rate giver mindre filer, men glatter korte spidser ud
- **Digital channels**: slå D0-D3 til
- Optag til fil; eksportér bagefter et udsnit som **CSV** til databehandling

Aflæsning: markér et tidsrum i grafen (fx fra D0 går høj til den går lav), så
viser appen **average (mA)**, **max (mA)**, **charge (µC/mC)** og **tid** for udsnittet.

## Fælles fremgangsmåde

1. Tjek ud på test-branchen og byg (kommandoen står i testens `TEST.md`).
2. Flash ESP'en med USB tilsluttet. Kontrollér i konsollen at den starter
   normalt (Wi-Fi, modem, første session). Tag derefter **USB ud**.
3. Kobl op som måling A (se ovenfor) og tænd forsyningen. Vent til den første
   session er overstået (D0 har været høj og er gået lav), typisk < 1 min.
4. Start optagelsen og kør den tid testen angiver.
5. Aflæs værdierne i skemaet nedenfor og gem både optagelsen og CSV-eksporten
   i `IoTEdge/tests/power/results/<test>/` (ikke i git hvis filerne er store).
6. Gentag evt. som måling B og C.

## Resultatskema (kopiér til hver test)

| Felt | Værdi |
|---|---|
| Test / branch / commit | |
| Dato, sted, hotspot, signal (AT+CSQ fra konsollen) | |
| Måling (A/B/C), forsyningsspænding | |
| Optagetid og sample rate | |
| **Gennemsnit hele optagelsen** (mA) | |
| Max (mA) | |
| Session: varighed (s), ladning (mC), gennemsnit (mA) | |
| Mellem sessioner: gennemsnit (mA) | |
| Én publish: varighed (ms), ladning (µC) | |
| Én Modbus-poll / én måling: ladning (µC) | |
| Bemærkninger (fejl, genforbindelser, afvigelser) | |

## Fra målinger til design

Med tallene kan gennemsnitsstrømmen for en hvilken som helst cyklus regnes ud:

```
I_gns = ( Q_session + n_målinger · Q_måling + I_hvile · (T - t_aktiv) ) / T
```

- `T`: cyklustid (fx 3600 s ved én session i timen)
- `Q_session`: ladning for én session (test 2; mC = mA·s)
- `Q_måling`: ladning for én måling inkl. Modbus-poll (test 2/3, D2/D3)
- `n_målinger`: målinger pr. cyklus (360 ved 10 s og én time)
- `I_hvile`: strøm mellem aktivitet. Fra test 2 (vågen) eller test 4
  (light/deep sleep + modem radio off) afhængigt af designvalg
- `t_aktiv`: samlet tid hvor ESP/modem er aktive i cyklussen

Batteritid: `timer = kapacitet_mAh / I_gns_mA` (regn 70-80 % af den
nominelle kapacitet som brugbar).

Eksempler på spørgsmål tallene skal besvare:
- Hvor meget sparer batching (test 2 vs. test 3)?
- Hvor meget koster det at holde Wi-Fi + Modbus kørende mellem målinger, i
  forhold til at sove (test 2 vs. test 4)?
- Hvor stor en del af energien går til modemet (måling C) vs. ESP'en (måling B)?
- Kan modemet slukkes for radioen mellem sessioner (AT+CFUN=0, test 4), og hvad
  koster det at vække det igen (registrering i starten af en session, test 2)?
- Hvilket publish-interval (1 time? 15 min?) giver den bedste balance mellem
  datatab ved fejl og strømforbrug?

## Kilder

- Nordic Semiconductor, *Power Profiler Kit II User Guide v1.0.1* (målemodes,
  grænser 600 mA / 1 A, logikport, nøjagtighed ±10-15 %)
- Zephyr: `samples/boards/espressif/light_sleep` og `deep_sleep` (søvntilstande på ESP32-C6)

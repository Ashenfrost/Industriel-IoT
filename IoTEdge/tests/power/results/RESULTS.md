# Resultater: strømmålinger med PPK2 (12. november 2026)

Måling A (hele systemet: ESP32-C6 DevKitC + M5Stamp CatM/SIM7080G), PPK2 i
Ampere Meter-tilstand, 100 kS/s, ca. 15 min pr. test. Rådata (ca. 2,4 GB pr.
fil) ligger på Google Drive under *Industriel IoT > PPK test* og er ikke i git.

## Vigtigt forbehold: ingen fasemarkører
Logikportens kanaler (D0-D7) er **0 i alle fire optagelser**: markørerne er
ikke blevet optaget (logikkablet ikke tilsluttet eller kanalerne ikke slået
til). Sessioner og faser er derfor fundet **ud fra strømmen alene**
(`bins.py` + `segment.py`): en "aktiv periode" er hvor 0,1 s-gennemsnittet
ligger over 90 mA (mellem tomgang ~57 mA og aktiv ~118 mA). Det er et
estimat: de første/sidste hundrede ms af en session med lav strøm tælles som
tomgang. Gennemsnittene for hele optagelserne er uafhængige af det.

PPK2's nøjagtighed er ±10 % (5-50 mA) og ±15 % (50 mA-1 A), User Guide tabel 9.

## Hovedtal

| Test | Hvad | Gennemsnit | Max |
|---|---|---|---|
| 1 | Altid forbundet, hver måling sendes straks (hvert 10. s) | **93,2 mA** | 641 mA |
| 2 | Batching: session hvert 5. min med 30 målinger | **60,8 mA** | 602 mA |
| 3 | Session pr. måling, hvert 10. s | **93,6 mA** | 492 mA |
| 4 | Søvn-testappen (gennemsnit over hele forløbet) | 32,3 mA | 443 mA |

### Test 1-3: aktive perioder og tomgang

| Test | Aktive perioder | Varighed | Ladning | Gns. i perioden | Tomgang |
|---|---|---|---|---|---|
| 1 | 89 (hvert 10,0 s) | 5,7 s | 673 mC | 118,5 mA | 61,4 mA |
| 2 | 4 (hvert ~296 s) | 7,1 s (sd 0,5) | 856 mC (sd 37) | 120,3 mA | **58,9 mA** |
| 3 | 72 (forventet 90; nogle smelter sammen) | 6,9 s | 820 mC | 118,4 mA | 61,9 mA |

### Test 4: tilstande (7 runder, meget stabile)

| Tilstand | ESP32-C6 | Modem | Strøm |
|---|---|---|---|
| Opstart + registrering | starter | registrerer | 114,2 mA i ~6 s |
| P1 | CPU 100 % | registreret | 55,5 mA |
| P2 | tomgang | registreret | 49,0 mA |
| P3 | light sleep | registreret | 17,8 mA |
| P4 | light sleep | radio slukket | **7,7 mA** |
| P5 | tomgang | radio slukket | 40,5 mA |
| Deep sleep | deep sleep | radio slukket | **7,5 mA** |

## Hvad tallene siger

1. **Batching er den store gevinst for afsendelse.** Energi til afsendelse
   pr. måling (ladning over tomgang):
   - test 1 (forbundet): 324 mC pr. måling
   - test 3 (session pr. måling): 396 mC pr. måling
   - test 2 (30 pr. session): **14,5 mC pr. måling, ca. 96 % mindre**

   I gennemsnit er test 2 33 mA lavere end test 1 og 3.
2. **Modemets "hale" koster mere end selve opkoblingen.** Efter hver
   afsendelse bliver modemets radio i forbundet tilstand i ~5-7 s ved ~118 mA
   (LTE's inaktivitetstimer). Det sker også i test 1, hvor forbindelsen er
   åben, derfor er test 1 og 3 næsten ens (93,2 vs 93,6 mA). TLS-opkobling,
   datakontekst og NTP lægger kun ~70 mC oven i (396 vs 324 mC).
3. **Tomgangen dominerer ved lange intervaller.** Med én session i timen
   giver selve sessionen kun ~0,25 mA i gennemsnit. Resten er tomgang:
   ESP vågen (~31-33 mA mere end light sleep), Wi-Fi + Modbus (~10 mA,
   estimat), modem registreret (~8-10 mA mere end radio slukket).
4. **Light sleep er nok; deep sleep giver næsten intet her.** Light sleep +
   radio slukket er 7,7 mA, deep sleep 7,5 mA (0,2 mA forskel). Gulvet på
   ~7,5 mA er kortenes egne LED'er, regulatorer og modemets grundstrøm, ikke
   ESP-chippen. Light sleep beholder RAM (køen af målinger) og vågner på µs,
   så det er det oplagte valg.
5. **CPU-belastning betyder lidt**: 100 % CPU koster 6,6 mA mere end tomgang.

## Hvad det betyder ved 1 times interval

Estimat (beregnet i `derive.py`): en session med 360 målinger (52 NDATA à 7)
koster ~0,9 C over tomgang; genregistrering efter radio slukket ~0,4 C.
Batteri som eksempel: 2000 mAh, 80 % brugbart.

| Design | Gns. strøm | Batteritid |
|---|---|---|
| A: som i dag (ESP vågen, Wi-Fi + Modbus, modem registreret) | **59,2 mA** | ~27 timer |
| B: som A, men modem-radio slukket mellem sessioner | **50,8 mA** | ~32 timer |
| D: ESP light sleep mellem målinger, modem registreret | ≥ 18,0 mA | ≤ ~3,7 døgn |
| C: ESP light sleep, modem-radio slukket (gulvet) | ≥ 8,1 mA | ≤ ~8 døgn |

C og D er **nedre grænser**: prisen for at vågne hvert 10. s og hente data
fra SimPLC'en over Wi-Fi er ikke målt (Wi-Fi skal genforbindes eller holdes i
strømsparetilstand). Det er det næste der skal måles.

## Anbefalinger til sleep/wake-designet

1. **Behold batching** (option A) og et langt publish-interval. Sessionens
   pris er lille ved 1 time; intervallet kan vælges efter hvor meget data man
   tør tabe ved fejl, ikke efter strøm.
2. **Sluk modemets radio mellem sessionerne** (`AT+CFUN=0`): sparer ~8-10 mA
   for ~0,4 C pr. genregistrering. Klart en gevinst ved 1 times interval.
3. **Brug light sleep på ESP'en mellem målingerne** (CONFIG_PM). Den største
   enkeltbesparelse (~31-33 mA). Deep sleep er ikke besværet værd på denne
   hardware.
4. **Wi-Fi er nu den store ubekendte**: med light sleep skal Wi-Fi enten
   bruge strømspare-tilstand eller genforbindes ved hver måling. Mål det
   (en test 5), eller hent data fra SimPLC'en sjældnere.
5. **Gulvet på ~7,5 mA kommer fra udviklingskortene.** Et endeligt produkt
   bør bruge moduler uden LED'er og med mere effektive regulatorer; ellers
   bestemmer gulvet batteritiden, uanset firmwaren.

## Test 5 og 6: deep sleep-udgaven (26. november 2026)

Den endelige deep sleep-udgave (`APP_DEEP_SLEEP=1`, `wake_cycle.c`): IoT
SimPLC måler hvert 10. s i sin buffer, IoT Edge sover og vågner for at tjekke
alarmer og sender, når den ældste måling er gammel nok. For at få mange
opvågninger med på kort tid kørte testen med alarmtjek hvert **30 s** og
afsendelse ved **120 s** (`-DAPP_ALARM_CHECK_S=30 -DAPP_SEND_AGE_S=120`).

Opstilling: hele systemet (måling A), men PPK2 i **Source Meter** ved 5 V,
forsynet kun via USB DATA/POWER fra PC'en (ingen ekstern forsyning). 100 kS/s,
ingen fasemarkører (slået fra i buildet). Spidserne i afsendelsen nåede ca.
510 mA, og der var ingen genstarter, så strømgrænsen var ikke et problem.
Rådata (`ppk-test5.csv` 1,4 GB, `ppk-test6-medoguden-modem.csv` 0,4 GB) er
ikke i git.

- **Test 5** (8,5 min): normal drift, 15 opvågninger.
- **Test 6** (2,4 min): modemets 5V blev trukket ud midt i optagelsen, mens
  IoT Edge var i deep sleep.

### Målte faser (test 5)

| Fase | Antal | Tid | Gns. strøm | Ladning |
|---|---|---|---|---|
| Deep sleep | – | – | **7,7 mA** | – |
| Alarmtjek (Wi-Fi, Modbus, sov) | 10 | 4,1 s (3,3-5,7) | ca. 75 mA | **0,31 C** (0,25-0,45) |
| Afsendelse med ca. 14 målinger | 3 | 37 s (29-46) | ca. 120 mA | **4,5 C** (3,7-5,2) |

To opvågninger lige efter opstart (20 s og 8 s ved ca. 70 mA) er ikke
regnet med: Wi-Fi fandt ikke netværket med den gemte kanal og måtte søge.

En afsendelse ser sådan ud: Wi-Fi + Modbus ca. 3 s ved 90 mA, Wi-Fi ned,
modemet registrerer sig ved ca. 100 mA (**2-19 s**, det er her variationen
kommer fra), forbundet og sender ved ca. 165 mA, og til sidst Wi-Fi igen for
at kvittere over for SimPLC.

### Modemets andel (test 6)

| | Deep sleep |
|---|---|
| Med modem (radio slukket med `AT+CFUN=0`) | **7,77 mA** |
| Modemets 5V trukket ud | **1,20 mA** |
| **Modemets andel** | **6,6 mA (ca. 85 %)** |

Spidserne på ca. 110 mA under deep sleep forsvinder også uden modem, så de
kommer fra modemet. Uden modem er et alarmtjek lidt billigere (ca. 0,26 C).
M5Stamp CatM har kun benene G, 5V, TX, RX, 3V og RF, altså **ingen PWRKEY**.
Modemet kan derfor kun slukkes helt ved at afbryde 5V med en transistor, som
ESP'en styrer. Det er overvejet, men ikke bygget.

### Omregnet til drift (alarmtjek hvert 5. min, afsendelse hver time)

En times afsendelse har 360 målinger = 52 NDATA à 7 i stedet for ca. 2. Hver
ekstra besked koster ca. 1,1 s ved ca. 165 mA (pausen på 1 s mellem
publishes). Det giver ca. **13,7 C på ca. 93 s**. Det er den eneste del, der
er **beregnet** og ikke målt.

| Pr. time | Som i dag | Modem afbrudt (transistor, ikke bygget) |
|---|---|---|
| Deep sleep (ca. 3460 s) | 26,7 C | 4,2 C |
| 11 alarmtjek | 3,4 C | 3,1 C |
| 1 afsendelse | 13,7 C | 13,7 C + ca. 1 C opstart |
| **I alt** | **43,7 C = 12,2 mA** | **21,9 C = 6,1 mA** |
| Batteritid (2000 mAh, 80 %) | **ca. 5,5 døgn** | ca. 11 døgn |

- **Deep sleep er 61 %** af ladningen, afsendelsen 31 % og alarmtjek kun 8 %.
- Det tidligere skøn uden PPK2 (ca. 10 mA, 6,7 døgn) var for optimistisk.
- Næste store gevinst efter modemet er **færre beskeder pr. afsendelse**
  (flere målinger pr. NDATA).

## Næste gang

- **Slå logikkanalerne til** (og tjek at D0-D3 skifter i appen før optagelse),
  så sessioner og publishes kan måles præcist med `analyze.py markers`.
- Mål også **B (kun ESP) og C (kun modem)**, så fordelingen kendes direkte
  i stedet for ud fra forskelle mellem tilstande. Test 6 gav fordelingen i
  deep sleep, men ikke under afsendelse.
- Mål en **rigtig times afsendelse** (360 målinger), så den ikke skal regnes
  ud fra korte afsendelser.
- 10 kS/s er nok til 15 min; 100 kS/s gav filer på ~2,4 GB.

## Sådan er tallene lavet

```powershell
python ..\bins.py ppk-test2-003.csv --bin 0.1 --out test2_b100ms.csv      # 90 M rækker -> 9000
python ..\segment.py sessions test2_b100ms.csv --threshold 90 --min-s 3 --gap-s 1 --json seg_test2.json
python ..\segment.py sessions test1_b100ms.csv --threshold 90 --min-s 1 --gap-s 1 --json seg_test1.json
python ..\segment.py sessions test3_b100ms.csv --threshold 90 --min-s 1 --gap-s 1 --json seg_test3.json
python ..\segment.py phases   test4_b100ms.csv --json seg_test4.json
python derive.py                                                          # tabellerne ovenfor
python ..\bins.py ppk-test5.csv --bin 0.1 --out test5_b100ms.csv          # test 5 og 6
python ..\segment.py sessions test5_b100ms.csv                             # opvågninger, deep sleep = median
python ..\bins.py ppk-test6-medoguden-modem.csv --bin 0.1 --out test6_b100ms.csv
python ..\segment.py sessions test6_b100ms.csv
```
`markers_test*.md` / `phases_test4_markers.md` er `analyze.py`'s rapporter
(kun hele-optagelsens tal, fordi markørerne mangler).

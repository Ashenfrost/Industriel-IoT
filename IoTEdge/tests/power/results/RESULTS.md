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

## Næste gang

- **Slå logikkanalerne til** (og tjek at D0-D3 skifter i appen før optagelse),
  så sessioner og publishes kan måles præcist med `analyze.py markers`.
- Mål også **B (kun ESP) og C (kun modem)**, så fordelingen kendes direkte
  i stedet for ud fra forskelle mellem tilstande.
- 10 kS/s er nok til 15 min; 100 kS/s gav filer på ~2,4 GB.

## Sådan er tallene lavet

```powershell
python ..\bins.py ppk-test2-003.csv --bin 0.1 --out test2_b100ms.csv      # 90 M rækker -> 9000
python ..\segment.py sessions test2_b100ms.csv --threshold 90 --min-s 3 --gap-s 1 --json seg_test2.json
python ..\segment.py sessions test1_b100ms.csv --threshold 90 --min-s 1 --gap-s 1 --json seg_test1.json
python ..\segment.py sessions test3_b100ms.csv --threshold 90 --min-s 1 --gap-s 1 --json seg_test3.json
python ..\segment.py phases   test4_b100ms.csv --json seg_test4.json
python derive.py                                                          # tabellerne ovenfor
```
`markers_test*.md` / `phases_test4_markers.md` er `analyze.py`'s rapporter
(kun hele-optagelsens tal, fordi markørerne mangler).

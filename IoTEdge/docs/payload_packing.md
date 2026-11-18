# Guide: pakning af data til afsendelse

Hvordan vi får målingerne ud over LTE-M med færre bytes, og hvad det er
værd. Tallene bygger på vores egne målinger: beskedstørrelser set på
brokeren og strømmålingerne med PPK2 (`IoTEdge/tests/power/results/RESULTS.md`).

## Konklusion først

- **Pakning sparer data, ikke ret meget strøm.** En ekstra NDATA-besked koster
  ca. 10 mC; en hel times data (52 beskeder) ca. 0,5 C. Til sammenligning
  bruger systemet ca. 28 C i timen bare på at hvile (7,7 mA). Bedre pakning
  flytter strømforbruget under 2 %.
- **Det der betyder noget for data, er antal sessioner og antal metrics.**
  Hver session koster et TLS-handshake (~4-6 kB) uanset hvor lidt der sendes,
  og hver metric koster ~18 bytes uanset hvor lille værdien er.
- Anbefalet rækkefølge: **(1) send kun ændrede værdier**, (2) send
  sjældnere, (3) heltal i stedet for float, (4) DataSet hvis det stadig er for
  meget. Komprimering er sidste udvej på en ESP32.

## Hvad en afsendelse fylder i dag

En måling = 7 metrics (3 temperaturer, 3 fugtigheder, status). Hver metric i
NDATA (protobuf) indeholder:

| Felt | Bytes | Kommentar |
|---|---|---|
| metric-header (tag + længde) | 2 | |
| alias | 2 | 10-12, 20-22, 30 |
| timestamp (ms siden 1970) | 7 | den største post |
| is_historical | 2 | på alle undtagen den nyeste måling |
| værdi (float) | 5 | 4 bytes + tag |
| **I alt pr. metric** | **~18** | |

Pr. måling ~125 bytes; NDATA med 7 målinger ~850-880 bytes (målt: 727 bytes
for 6, 850 for 7). Oveni hver besked: MQTT (~45 bytes: topic
`spBv1.0/GRP1/NDATA/device-<mac>` + header) og TLS-record (~29 bytes).

| Med måling hvert 10. s | Pr. time | Pr. døgn | Pr. måned |
|---|---|---|---|
| 360 målinger → 52 NDATA à ~880 B | ~46 kB | ~1,1 MB | ~33 MB |
| + MQTT/TLS pr. besked | ~4 kB | ~0,1 MB | ~3 MB |
| + TLS-handshake pr. session (~5 kB) | ~5 kB | ~0,12 MB | ~3,6 MB |

Med måling hvert 10. minut er det kun 6 målinger i timen (1 NDATA på ~750 B),
og så er **TLS-handshaket ca. 85 % af dataforbruget** hvis der sendes hver
time. Der er svaret: send sjældnere.

## Optimeringer, rangeret

### 1. Send kun ændrede værdier (report by exception), stor gevinst, lille indsats
Sparkplug er lavet til det: NDATA må gerne kun indeholde de metrics der har
ændret sig. DHT11 måler i hele grader og hele procent, så de fleste målinger
er ens med den forrige.
- **Edge**: husk den senest sendte værdi pr. metric; tag kun metrics med i
  NDATA der afviger (evt. med en deadband, fx 0,5 °C). Status sendes kun ved
  ændring. NBIRTH sender altid alle værdier, så hosten har et startpunkt.
- **Host/QuestDB**: ingen ændring. Der kommer bare færre rækker; "ingen ny
  række" betyder "uændret". I forespørgsler: `SAMPLE BY 10s FILL(PREV)`.
- **Forventet**: 70-90 % færre metrics i stabile perioder.
- Ulempe: man kan ikke længere se en række pr. 10 s i QuestDB uden FILL.

### 2. Send sjældnere, gratis
Ét TLS-handshake pr. session. Fra 1 til 6 timers interval falder handshake-
dataen med 5/6. Begrænset af hvor meget data man tør tabe ved strømsvigt
(køen ligger i RAM) og hvor friske data skal være.

### 3. Heltal i stedet for float, ~10 %, lille indsats
Temperatur og fugt sendes i dag som float (5 bytes). Som heltal × 10 (fx 214
for 21,4 °C, datatype Int16) fylder de 2-3 bytes som varint.
- **Edge**: datatype `Int16`, `int_value = temp_x10` (vi har allerede værdien
  × 10 fra Modbus, så det er også mere præcist: ingen float-afrunding).
- **Host**: skal vide at værdien er × 10. Enten ved navnet (fx
  `Top/Temperature_x10`) eller en Sparkplug-property `scale = 0.1` i NBIRTH.
  `host.py` dividerer ved indlæsning.
- Fjerner også problemet med mange decimaler i QuestDB.

### 4. Sparkplug DataSet, ~75 %, større indsats
I stedet for 7 metrics pr. måling sendes én metric af typen DataSet: en
tabel med kolonnerne `timestamp, top_t, mid_t, bot_t, top_h, mid_h, bot_h,
status` og én række pr. måling. Alias, tag og timestamp gentages ikke pr.
værdi, så en række fylder ~25-30 bytes mod ~125 i dag.
- **Edge**: DataSet er sprunget over i `sparkplug_b.options` i dag; skal slås
  til og kodes.
- **Host**: `host.py` skal pakke tabellen ud til rækker i QuestDB.
- Giver også plads til ~30 målinger pr. besked i stedet for 7.

### 5. Komprimering, sidste udvej
Sparkplug tillader komprimerede payloads (DEFLATE/GZIP). Protobuf med
gentagne alias og næsten ens timestamps komprimerer godt, men på ESP32-C6
koster deflate ~32 kB RAM til ordbogen og mere CPU-tid, og hosten skal
pakke ud. Først relevant hvis 1-4 ikke er nok.

### 6. Mindre ting
- **Topic**: ~40 bytes pr. besked. Kortere node-id (fx `e1` i stedet for
  `device-14c19fe0ab24`) sparer ~17 bytes pr. besked; skal gøres i hele
  systemet.
- **TLS session resumption**: genbruger nøglerne fra sidste session, så
  handshaket går fra ~5 kB til < 1 kB. Kræver at SIM7080 og Mosquitto
  understøtter det (ikke undersøgt endnu; tjek SIM7080's SSL-manual).
- **Større beskeder**: vi er allerede tæt på SMPUB-grænsen på 1024 bytes.

## Hvad det giver samlet (måling hvert 10. s, én session i timen)

| Variant | Pr. døgn (ca.) |
|---|---|
| I dag | 1,3 MB |
| + kun ændrede værdier (80 % uændret) | 0,35 MB |
| + heltal | 0,3 MB |
| DataSet i stedet for 1+3 | 0,4 MB |
| DataSet + kun ændrede værdier | 0,15 MB |

Med måling hvert 10. minut og én session hver 6. time: under 0,05 MB pr. døgn.

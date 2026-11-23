# Strømforbrug på Edge-noden: resultater

*Industriel IoT, november 2026. Sammendrag til Product Owner.*

## Kort fortalt

- **Batching virker.** Når målingerne samles og sendes samlet i stedet for én ad
  gangen, falder det gennemsnitlige strømforbrug fra ca. 93 mA til ca. 61 mA
  (ca. 35 % lavere). Energien til at sende én måling falder med **96 %**.
- **Det dyre er modemets radio, ikke selve opkoblingen.** Efter hver afsendelse
  bliver radioen tændt i 5–7 sekunder ved ca. 118 mA.
- **Den største besparelse er at lade ESP'en sove og slukke modemets radio
  mellem afsendelserne.** Så falder hvilestrømmen fra ca. 59 mA til ca. 8 mA.
- **Beslutning:** Vi måler hvert 10. sekund og sender en gang i timen. Modemets
  radio slukkes mellem afsendelserne, og ESP'en går i light sleep mellem
  målingerne.

## Opstillingen

```
SimPLC (3 × DHT11) --Wi-Fi/Modbus--> ESP32-C6 (Edge) --UART--> SIM7080G modem --LTE-M/TLS--> VPS (Mosquitto + QuestDB)
                                     \_____________ målt med PPK2 _____________/
```

- PPK2 måler hele Edge-noden, dvs. ESP32-C6 og modemet tilsammen.
- Der tages 100.000 målinger i sekundet, og hver test varer ca. 15 minutter.
- PPK2 er nøjagtig til ±10–15 %.

## Resultat 1: tre måder at sende på

Alle tre tests tager en måling hvert 10. sekund. Det eneste, der er forskelligt,
er måden de sendes på.

| Test | Hvordan der sendes | Gennemsnitsstrøm |
|---|---|---|
| 1 | Altid forbundet, hver måling sendes straks | 93,2 mA |
| 3 | Ny forbindelse for hver måling | 93,6 mA |
| 2 | **Batching: 30 målinger samlet hvert 5. minut** | **60,8 mA** |

**Batching sparer 33 mA i gennemsnit (ca. 35 %).**

### Set pr. måling

Energien til at sende én måling er her regnet ud over hvilestrømmen:

| Test | Energi pr. måling |
|---|---|
| 1: altid forbundet | 324 mC |
| 3: forbindelse pr. måling | 396 mC |
| 2: batching | **14,5 mC (96 % mindre)** |

Én session med batching tager ca. 7 sekunder og koster ca. 856 mC, men de 30
målinger deler den.

## Resultat 2: hvorfor test 1 og 3 er næsten ens

Efter hver afsendelse lader mobilnettet modemets radio stå tændt i ca. 5–7
sekunder ved ca. 118 mA, før den går i dvale (LTE's inaktivitets-timer). Det
sker uanset om forbindelsen holdes åben eller ej.

- Selve opkoblingen (TLS, dataforbindelse, tidssynkronisering) koster kun ca.
  70 mC ekstra pr. gang.
- "Halen" efter afsendelsen koster langt mere.
- Derfor gælder det om at sende **sjældent**, ikke om at holde forbindelsen
  åben.

## Resultat 3: hvad hver tilstand bruger

| Tilstand | ESP32-C6 | Modem | Strøm |
|---|---|---|---|
| Opstart | starter | registrerer på nettet | 114 mA i ca. 6 s |
| Arbejder | CPU 100 % | på nettet | 55,5 mA |
| Venter | vågen, intet at lave | på nettet | 49,0 mA |
| Venter, radio slukket | vågen, intet at lave | radio slukket | 40,5 mA |
| Light sleep | sover | på nettet | 17,8 mA |
| **Light sleep, radio slukket** | sover | radio slukket | **7,7 mA** |
| Deep sleep | dyb søvn | radio slukket | 7,5 mA |

Hvad tabellen viser:

- **Light sleep er nok.** Deep sleep sparer kun 0,2 mA mere. Til gengæld mister
  ESP'en sin hukommelse og dermed de målinger, der venter på at blive sendt.
- **Gulvet på ca. 7,5 mA kommer fra udviklingskortene** (LED'er og
  spændingsregulatorer), ikke fra chippen. Et færdigt produkt med andre moduler
  kan komme længere ned.
- **CPU-belastning betyder lidt:** 100 % CPU koster kun 6,6 mA mere end at
  vente.

## Resultat 4: batteritid

Beregnet for en session i timen (360 målinger) og som eksempel et batteri på
2000 mAh, hvoraf 80 % kan bruges:

| Design | Gns. strøm | Batteritid |
|---|---|---|
| A: ESP vågen, modem altid på nettet | 59,2 mA | ca. 27 timer |
| B: som A, men modemets radio slukket mellem afsendelser | 50,8 mA | ca. 32 timer |
| D: ESP i light sleep, modem altid på nettet | mindst 18,0 mA | højst ca. 3,7 døgn |
| **C: ESP i light sleep og modemets radio slukket** | **mindst 8,1 mA** | **højst ca. 8 døgn** |

Tallene for C og D er det bedste, vi kan opnå. De indeholder endnu ikke prisen
for at vække ESP'en og hente data fra SimPLC'en over Wi-Fi hvert 10. sekund.
Den pris er det næste, vi skal måle.

## Beslutning

| | Valg | Hvorfor |
|---|---|---|
| Måling | hvert 10. sekund | kravspecifikationen siger 5–15 s |
| Afsendelse | en gang i timen, samlet | en session koster kun ca. 0,25 mA i gennemsnit ved 1 time |
| Modem | radio slukket mellem afsendelser | sparer ca. 8–10 mA |
| ESP32-C6 | light sleep mellem målinger | største enkeltbesparelse, ca. 31–33 mA |

## Data og lager

- **Lager på ESP'en:** Én måling fylder 40 bytes i RAM. Køen har plads til 720
  målinger (2 timer), dvs. ca. 29 kB af ESP32-C6's 512 kB RAM. Det er plads
  nok, også hvis en afsendelse fejler.
- **Dataforbrug i dag:** ca. 1,3 MB pr. døgn (pr. time: ca. 46 kB målinger,
  4 kB MQTT/TLS-overhead og 5 kB TLS-opkobling).
- **Kan sænkes** til ca. 0,15 MB pr. døgn, hvis vi kun sender ændrede værdier
  og pakker målingerne som en tabel (Sparkplug DataSet).
- **Bedre pakning sparer data, men næsten ikke strøm** (under 2 %). Strømmen
  afgøres af, hvor tit modemet tændes.

## Status

| | Status |
|---|---|
| Batching, afsendelse samlet | ✅ virker |
| Modemets radio slukket mellem afsendelser | ✅ virker, testet på hardware |
| Wi-Fi kun tændt under læsning fra SimPLC | ✅ virker, testet på hardware |
| Fejl hvor sidste besked gik tabt ved afbrydelse | ✅ fundet og rettet |
| Light sleep mellem målinger | 🔶 implementeret, testes nu |

Wi-Fi tager 4–15 sekunder at forbinde til en mobil-hotspot. Ved måling hvert 10.
sekund kan det derfor ikke betale sig at slukke Wi-Fi mellem målingerne. Det
giver først mening ved længere intervaller (60 s eller mere).

## Næste skridt

1. **Mål prisen for Wi-Fi** (test 5): hvad koster det at hente data fra
   SimPLC'en hvert 10. sekund, når ESP'en ellers sover?
2. **Langtidstest af light sleep** med data i QuestDB.
3. **Mål ESP og modem hver for sig** (måling B og C), så fordelingen kendes
   direkte.
4. **Slå fasemarkørerne til på PPK2**, så sessioner kan måles præcist.

## Forbehold

- Ved målingerne blev PPK2's markørkanaler ikke optaget. Sessioner er derfor
  fundet ud fra strømkurven (over 90 mA = aktiv). Gennemsnittene for hele
  optagelserne påvirkes ikke af det.
- Alle tal gælder udviklingskort. Et færdigt produkt vil have et andet gulv.
- Rådata (ca. 2,4 GB pr. test) ligger på Google Drive under
  *Industriel IoT > PPK test*. Udregningerne findes i
  [`tests/power/results/RESULTS.md`](../tests/power/results/RESULTS.md).

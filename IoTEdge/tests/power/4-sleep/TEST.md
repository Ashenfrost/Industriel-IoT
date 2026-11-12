# Test 4: Sleep-tilstande

Branch: `power-test/4-sleep`. Fælles opkobling, PPK2-indstillinger og
resultatskema: se [`../README.md`](../README.md).

## Formål
At måle strømforbruget for IoT Edge, når ESP32-C6 sover, og når modemets
radio er slukket. Det er "gulvet": så lavt kan vi komme mellem målinger og
sessioner. Tallene afgør, hvilken sleep/wake-cyklus der giver mening.

## Forudsætninger
- Testappen `IoTEdge/tools/power_sleep_test` fra denne branch (ikke den
  normale firmware). Den kører faserne nedenfor i ring og viser **fase-nr.
  binært** på logikporten: D0 = bit 0, D1 = bit 1, D2 = bit 2.
- Ingen SimPLC eller Wi-Fi nødvendig (testen bruger ikke Wi-Fi).
- PPK2 i **Ampere Meter**-tilstand. Mål helst **både A, B og C**: ESP'ens søvn
  er lille i forhold til modemet, så den forsvinder i måling A.

| Fase | D2 D1 D0 | ESP32-C6 | SIM7080G | Varighed |
|---|---|---|---|---|
| (opstart) | 0 0 0 | starter, tænder modem-radio, registrerer | radio tændes | 5-60 s |
| 1 | 0 0 1 | CPU 100 % (busy loop) | registreret, ingen data | 20 s |
| 2 | 0 1 0 | idle uden light sleep | registreret | 20 s |
| 3 | 0 1 1 | **light sleep** | registreret | 20 s |
| (skift) | 0 0 0 | AT+CFUN=0 | radio slukkes | < 1 s |
| 4 | 1 0 0 | **light sleep** | **radio slukket** (CFUN=0) | 20 s |
| 5 | 1 0 1 | idle uden light sleep | radio slukket | 20 s |
| deep sleep | 0 0 0 | **deep sleep** (timer vækker) | radio slukket | 30 s |
| → genstart, fase 1 igen | | | | |

Én runde tager ca. 2,5-3 min. Opvågning fra deep sleep er en genstart:
markørerne går lave, og den starter forfra.

## Byg og flash
```powershell
west build -p always -d build-sleep -b esp32c6_devkitc/esp32c6/hpcore <repo>\IoTEdge\tools\power_sleep_test -S espressif-flash-4M
west flash -d build-sleep --esp-device COM6
```
- Hvis flash fejler ("Failed to connect"), sover ESP'en: hold **BOOT** inde,
  tryk **RESET**, slip BOOT, og flash igen.
- USB-konsollen viser kun fase 1-3; efter light sleep viser den ikke mere.
  Det er forventet. Faserne aflæses på logikporten.

## Fremgangsmåde
1. Flash, tag USB ud, kobl op som måling A og tænd.
2. Optag **mindst 2 hele runder** (ca. 6 min), **100 kS/s** (korte opvågninger
   fra light sleep er få µs-ms lange).
3. For hver fase (brug fase-nr. på D0-D2) aflæs **gennemsnit (mA)** over de
   midterste 15 s af fasen (spring overgangen over).
4. Aflæs gennemsnit i **deep sleep** (alle markører lave, mellem fase 5 og den
   næste opstart).
5. Aflæs **opstart efter deep sleep**: varighed og ladning (mC) fra
   strømmen stiger til fase 1 begynder. Det er prisen for at vågne og få
   modemet registreret igen.
6. Gentag som måling B (kun ESP) og C (kun modem).

## Mål
Strøm (mA eller µA) for:
- ESP: busy / idle / light sleep / deep sleep
- Modem: registreret idle / radio slukket
- Ladning for "vågn op fra deep sleep + modem registrerer igen"

## Succeskriterier
- Alle faser er aflæst for mindst måling A; ESP-faserne også for måling B.
- Tallene kan bruges i formlen i README'en som `I_hvile` for forskellige
  designvalg, fx: ESP light sleep + modem radio off mellem sessioner, eller
  ESP deep sleep mellem målinger.
- Bemærk hvad DevKit'ets egne LED'er/regulator bidrager med i deep sleep
  (det er der, de fylder mest); det er ikke chippens egen søvnstrøm.

## Kendte begrænsninger
- PSM (Power Saving Mode) for modemet er ikke med: det kræver at netværket
  (Onomondo/Norlys) tildeler PSM-timere, og at modemet kan vækkes. Kan
  tilføjes som en senere test.
- Wi-Fi er ikke med i testen. Wi-Fi's strøm ses i test 1-3.

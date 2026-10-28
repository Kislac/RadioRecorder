# RadioRecorder

ESP32-S3 prototípus kézi, mono WAV-felvételhez microSD-kártyára. Nyitott saját Wi-Fi hálózatán böngészőből indítható és állítható le; a lezárt felvételek visszahallgathatók és letölthetők.

A jelenlegi és tervezett modulok GPIO-kiosztása az [eszközjegyzékben](docs/device-gpio-map.md) található.

## Megvalósított funkciók

- Kézi mono WAV-felvétel a GPIO1 ADC-bemenetről microSD-re, állapot- és hiba-visszajelzéssel.
- Külön indítható INMP441-M I2S mikrofonfelvétel ugyanarra a microSD-re (22 050 Hz / 16 bites mono PCM WAV), OLED Mikrofon csempéről vagy a webes főoldalról; egyszerre csak egy forrás rögzíthet. A webes főoldalon külön kapcsolható mikrofon RMS/csúcs grafikon látható, felvétel közben is, SD-írás nélkül.
- OLED/enkóderes helyi kezelőfelület, RMS/csúcs jelmérés, hangerő/erősítés és zajszűrő-beállítás.
- ESP-hangszórós WAV-lejátszás, böngészős élő hang, valamint firmware-be épített ClearSky kezdőkép és boot/felvételi hangjelzések.
- Nyílt `RadioRecorder` AP alapmód; mentett külső Wi-Fi-hez csatlakozás AP-fallbackkel, opcionális AP+STA móddal, webes scan/connect/forget kezeléssel és Wi-Fi auto-off időzítővel.
- Beállítások NVS-mentése, OLED képernyő-timeout és ébresztés, firmware-verzió/buildidő kijelzése, valamint kétmagos ESP-IDF task watchdog.
- Böngészős felvétel- és fájlkezelés, valamint PC-s OLED nézet, soros enkóder-emuláció és offline/hardveres/Wi-Fi tesztek.
- ESP nélküli webes előnézet szimulált felvétellel, jelméréssel, hanggal és Wi-Fi-állapotokkal; a felület külön HTML/CSS/JS forrásokból szerkeszthető. Részletek: [webes fejlesztés](docs/web-development.md).
- Rádió nélküli, aktív LOW PTT-relé teszt az OLED Rádió csempéjén és a webes `/test` oldalon: legfeljebb 1 másodperces GPIO10-impulzus vagy a weben külön tartós be/ki kapcsoló, azonnali leállítással. A firmware setup első lépéseként kikapcsolt állapotba állítja a relét; a bekapcsolás előtti szintet csak megfelelő hardveres előfeszítés garantálja. Biztonságos próba: [tesztelés](docs/testing.md).

**A relé COM/NO és a rádió MIC-bemenete nincs a rádióhoz kötve; rádiós adás nincs engedélyezve.** A tesztimpulzus elektromos polaritását és a GPIO10 tényleges bekötését a konkrét panelen még ellenőrizni kell. Az akkumulátoros üzemidőhöz a kijelző és a Wi-Fi automatikus kikapcsolása van megvalósítva; az ESP32 teljes mélyalvása nincs.

## Első, rádió nélküli próba

1. Ellenőrizd a saját panelen, hogy a [próbarajzon](docs/radio-receive-adc.svg) szereplő GPIO1 valóban ADC1_CH0, és az SD-foglalat lábai CMD=GPIO38, CLK=GPIO39, DATA0=GPIO40. A panelre vonatkozó [figyelmeztetések](docs/functional-hardware.md) továbbra is érvényesek.
2. A rajz szerinti előfeszített ADC-áramkört csak ismert, kis szintű, az ESP-vel biztonságosan közös referenciájú próbaforrással építsd meg. A rádió egyik érintkezőjét se csatlakoztasd. GPIO1-en nyugalomban kb. 1,65 V-ot, jellel kb. 0,3-2,7 V közötti csúcsokat ellenőrizz; a GPIO soha ne kerüljön 0 V alá vagy 3,3 V fölé.
3. Helyezz be FAT32-formátumú microSD-t, és tápláld a panelt a saját USB-csatlakozójáról. Kamera, transzformátor és rádióbekötés ehhez a próbához nem kell.

## Fordítás és használat

PlatformIO: `pio run -e esp32-s3-devkitc-1`, majd a tényleges panel és port ellenőrzése után `pio run -e esp32-s3-devkitc-1 -t upload`. A soros monitor sebessége **115200 baud** (`monitor_speed = 115200`). Bootkor a firmware verziója/buildideje, a watchdog, az SD, ADC, felvevő, hangszóró és az aktuális Wi-Fi mód/SSID/IP jelenik meg; felvételkor a kezdés, leállítás, fájlnév és hiba is. A monitorban az `i` karakter elküldésével bármikor újra lekérhető a státusz. Mivel a monitor nem reseteli a panelt (`monitor_rts = 0`, `monitor_dtr = 0`), az indulási üzenetekhez nyitott monitor mellett nyomd meg a panel reset gombját.

A főoldal és a fájlkezelő fejlécében is a futó firmware verziója látszik. A `src/app_control.cpp` fájl `kFirmwareVersion` konstansában lévő alapverziót **minden kódváltoztatáskor kézzel növeljük**; a végére fűzött buildszámot a PlatformIO feltöltéskor automatikusan növeli. A tartós számláló a `firmware-build.txt` fájlban van, ezt ne töröld: sima fordításkor nem változik, sikertelen feltöltéskor sem lép előre. Például a 0.1.2.0 csak helyben fordított változat, az első sikeres feltöltés 0.1.2.1. A fordítás nem tölti fel automatikusan a firmware-t. A weboldalon új verzió csak a feltöltés és újracsatlakozás után jelenik meg.

Az OLED-támogatás **engedélyezve van** (`RADIORECORDER_OLED_ENABLED=1`). A 128x64-es kijelzőt a firmware SH1106-ként kezeli; 3,3 V-os I2C lába SDA=GPIO4, SCL=GPIO5. A soros monitor induláskor jelzi a 0x3C/0x3D címek I2C-próbáját és az `OLED: ready` állapotot. Ha nincs válasz, ellenőrizd az SDA/SCL felcserélését, a 3V3/GND tápot, a felhúzásokat és a panel GPIO4/5 kiosztását; az enkóder nem hajt végre rejtett gyorsparancsokat. Működő kijelzővel a forgatás csempét vagy beállítási sort választ, a rövid gombnyomás műveletet indít, a hosszú kilép. A Felvétel csempe az ADC-t, a Mikrofon csempe az I2S mikrofont rögzíti; a jelmérés, Wi-Fi mód, hangerő, erősítés és szűrő kezelése, valamint a NVS-be mentett időzítők elérhetők. A fájlok OLED-es böngészése és a felvétel alatti ESP-hangszórós lejátszás még nem. A Rádió csempén csak rádió nélkül használható PTT-tesztimpulzus van, nem rádiós adáskijelzés vagy igazolt DAC-út. A weboldalak és az OLED 13:54:22-ről induló, újraindításonként újrakezdődő **DEMO** időt mutatnak, nem RTC-időt vagy valódi felvételi időbélyeget.

A kijelző PC-s megjelenítéséhez és az automatikus hardver-/Wi-Fi-tesztekhez lásd a [tesztelési útmutatót](docs/testing.md).

A webes felület szerkesztési forrása a `web/` könyvtár. A PlatformIO a fordítás előtt automatikusan beágyazza az oldalakat és asseteket a firmware-be; az ESP-nek ehhez továbbra sem kell SD-n tárolt webfájl. A `src/web_pages.cpp` már csak a generált oldalak betöltője.

A `src/main.cpp` az inicializálás és a fő ciklus belépési pontja. A `src/recorder.cpp` kezeli az ADC-t, az I2S mikrofont, a felvételt és a jelmérést; a `src/playback.cpp` az I2S WAV-lejátszást és a firmware-be épített hangjelzéseket. A `src/live_audio.cpp` a felvevő és a WebSocket közös PCM-sorát, a `src/wav.cpp` a WAV-formátumot kezeli. A `src/web_server.cpp` birtokolja a HTTP-útvonalakat, a fájlkezelést és a WebSocketet; a `src/wifi_manager.cpp` a STA/AP+STA/AP módokat, csatlakozást, scan-t, fallbacket és Wi-Fi auto-offot. A beágyazott weboldalak a `src/web_pages.cpp` fájlban vannak, SD-kártyás webes erőforrások nélkül. Az OLED/enkóder logika a `src/display.cpp` és `src/controls.cpp` fájlokban, a tartós beállítások és a watchdog pedig az `src/app_control.cpp`, illetve `src/watchdog.cpp` modulokban találhatók.

A jelenlegi Arduino-panelprofilban a `Serial` az **UART0** kimenete (GPIO43 TX, GPIO44 RX), nem automatikusan a natív USB CDC. A monitort az UART0-hoz kapcsolódó USB-soros porton használd; ha a panel csak natív USB-csatlakozón látszik és a monitor üres, az USB CDC engedélyezése külön panel-/portbeállítást igényel. Ne csatlakoztass 5 V-os soros adaptert a 3,3 V-os GPIO-khoz.

### ROT-1AB enkóder próba

A [panel pinoutja](docs/pinout.md) alapján, **áramtalanítva**, a ROT-1AB feliratait a panelen lévő szitázással egyeztesd:

| ROT-1AB | ESP32-S3 panel |
| --- | --- |
| CLK | GPIO21 |
| DT | GPIO47 |
| SW | GPIO14 |
| + | 3V3, **nem 5V** |
| GND | GND |

Az enkóder öt vezetéke a rádiótól és a GPIO1 előfeszítő körétől különálló. A konkrét panelen ellenőrizd, hogy a GPIO14, GPIO21 és GPIO47 valóban kivezetett és szabad. Forgatással választható ki a képernyőn a Felvétel, Mikrofon, Lejátszó, Élő jel, Diagnosztika, Rádió, Wi-Fi vagy az utolsó Beállítások csempe; rövid nyomás a művelet, hosszú nyomás a visszalépés. A jelmérés az **Élő jel** csempén kapcsolható be és ki; a Wi-Fi csempe az aktuális módot, SSID-t és IP-t mutatja, rövid nyomással elfelejthető a mentett hálózat. A **Beállítások** alatt a Wi-Fi mód és az auto-off, a képernyő-timeout, valamint a hangbeállítások állíthatók. Alvó kijelzőn az első enkóderes input csak ébreszt, a következő hajtja végre a műveletet. A soros `r`/`s`/`i`/`f` parancsok és a webes vezérlés tovább működnek. Ha a forgatás iránya felcserélődik, a CLK és DT vezetékeket **áramtalanítva** cseréld meg.

Mentett külső hálózat nélkül indulás után csatlakozz a **RadioRecorder** nevű, jelszó nélküli Wi-Fi AP-hoz, majd nyisd meg a `http://192.168.4.1` címet. Külső Wi-Fi a webes Beállítások oldalon kereshető és menthető; sikeres csatlakozás után alapból STA-only módra vált, a **Keep AP host** jelölővel az AP is megtartható. A kapcsolat, IP-cím és mód a Wi-Fi OLED csempén látszik; az auto-off alapból 5 perc, a beállításokban állítható. A felvételt a weboldalon indítsd és állítsd le. Leállítás után az `REC00001.wav` jellegű fájlok megjelennek a listában. Egyszerre egy felvétel készül.

A **Wi-Fi zajteszt** előtt állítsd le az élő hallgatást; a rádió és a próbajelforrás maradjon leválasztva, a GPIO1 előfeszítése maradjon bekötve. A gomb egy WAV-ot rögzít: az első kb. 5 másodpercben a Wi-Fi AP működik, a következő kb. 10 másodpercben ki van kapcsolva. Ezalatt a telefon kapcsolata megszakad; a firmware a WAV lezárása után újraindul, és az AP visszatér. Ezután csatlakozz újra és hallgasd meg ugyanannak a fájlnak a két részét. Ne kapcsold le a tápot a felvétel alatt: a lezáratlan WAV megsérülhet. Ha a fájl nem tartalmaz mindkét szakaszból hangot, a próba nem értékelhető.

A **Fájlkezelő** linken (`http://192.168.4.1/files`) az SD-kártya mappái és fájljai 20-as lapokban böngészhetők. WAV, MP3, M4A, OGG, FLAC, AAC és WebM hangfájlokhoz lejátszó nyitható; a tényleges lejátszhatóság a telefon böngészőjétől és a fájl kódolásától is függ. Bármely fájl letölthető. Az SD olvasása felvétel alatt tiltott; az éppen készülő fájl csak lezárás után jelenik meg.

### Lejátszás az ESP hangszóróján (rádió nélkül)

Áramtalanítva csatlakoztass MAX98357A I2S erősítőt és külön **4 Ω / 3 W hangszórót**. A panel lábkiosztását és az USB-ről származó 5 V-os tápot a saját példányodon ellenőrizd:

| MAX98357A | ESP32-S3 / hangszóró |
| --- | --- |
| BCLK | GPIO42 |
| LRC / LRCLK | GPIO41 |
| DIN | GPIO11 |
| VIN | ellenőrzött USB 5 V |
| GND | ESP GND |
| SPK+ / SPK- | kizárólag a külön hangszóró két kivezetése |

A MAX98357A DIN vezetékét a korábbi GPIO48-ról **áramtalanítva át kell helyezni GPIO11-re**: a GPIO48 a panel beépített RGB LED-jéhez tartozik, az új firmware már nem ad oda hangot. A [panelképen](docs/2026-10-24_11h20_03.png) a GPIO11 kamerajelként is szerepel: a kamera szalagkábelét előbb áramtalanítva válaszd le, majd ellenőrizd a tényleges panel kivezetését és a lábon esetleg fennmaradó terhelést. A hangszóró egyik vezetékét se kösd GND-re. **A SPK+/SPK- kimenetet és a próbához használt erősítőt ne csatlakoztasd a rádió MIC-bemenetéhez; a PTT-relé csak külön tesztimpulzusra kapcsolható, rádiós adás nem működik.** Alacsony hangerővel kezdd a próbát: a firmware induláskor a WAV mintáit 25%-ra állítja. A 100% a teljes digitális jelszint, nem az erősítő vagy a hangszóró névleges teljesítményének százaléka.

A MAX98357A **GAIN** lába a chip hardveres erősítését választja ki, nem szoftveres hangerőbemenet. A [MAX98357A modul dokumentációja](https://learn.adafruit.com/adafruit-max98357-i2s-class-d-mono-amp/pinouts) szerint a szabadon hagyott GAIN alapból 9 dB; közvetlen GND-re kötve 12 dB, 100 kΩ-on keresztül GND-re kötve 15 dB. Más modul saját ellenállása módosíthatja az alapállapotot, ezért annak kapcsolását ellenőrizni kell. Egyelőre hagyd szabadon; későbbi változtatás előtt áramtalanítsd a panelt. Az erősítő tényleges kimenő teljesítményét az ellenőrzött VIN és a hangszóró is korlátozza.

A főoldali felvétellistán és a Fájlkezelő WAV-fájljainál az **ESP-n lejátszás** gomb a microSD-ről közvetlenül az I2S erősítőre küldi a hangot; a böngésző saját hangszóróján továbbra is a külön lejátszóval hallgathatod. Az ESP csak **22 050 Hz-es, 16 bites, mono PCM WAV** fájlokat játszik le, MP3-at vagy más WAV-kódolást nem. Az **ESP hangszóró hangereje** csúszka 0–100% között, lejátszás közben is állítható; alatta a nyilak 5%-kal léptetnek, az érték mindkét oldalon megjelenik. A **Lejátszási erősítés** választó ettől függetlenül, lejátszás közben 1×/2×/4× digitális szorzást ad a halk WAV-hoz; újraindítás után 25% és 1× az alapérték. A túl nagy WAV-csúcsoknál a firmware korlátozza a kimenő szintet: a hangos felvétel nem lesz korlátlanul erősíthető, és a zaj is felerősödhet. Kis hangerővel, 1×-ről indulva próbáld; torzításnál válts vissza. A böngészős élő hang hangerőcsúszkája ettől független. Egyszerre egy fájl szólhat; leállítani mindkét oldalon az **ESP-lejátszás leállítása** gombbal lehet. Felvétel és ESP-lejátszás nem indulhat egyszerre, és az élő hallgatás sem indítható ESP-lejátszás közben. A Wi-Fi kézi kikapcsolásakor a már elindult helyi lejátszás tovább fut, de a webes leállításhoz előbb vissza kell kapcsolni az AP-t.

Az I2S0 PDM-próbakimenet megszűnt: a GPIO2 nem ad lejátszási jelet. A korábbi [PDM-próbarajz](docs/radio-pdm-output.svg) csak történeti jegyzet, **ne építsd meg**. A GPIO2-re szerelt szűrőt áramtalanítva le kell választani; a rádió MIC-bemenete továbbra sincs bekötve.

A **GPIO1 jel** mérése induláskor ki van kapcsolva. A **Mérés bekapcsolása** gombbal 100 ms-os blokkokból számolt RMS- és csúcsértékek 30 másodperces grafikonja jelenik meg, továbbá a nyers DC-középérték (mindhárom 12 bites ADC-számlálási egységben). Kikapcsolva, felvétel és élő hallgatás nélkül az ADC nem mintavételez; felvétel közben a grafikon bekapcsolva a rögzítés mintáit használja. Az oldal bezárásakor vagy a lekérdezés több mint 4 másodperces megszűnése után a figyelés leáll. Ez még nem automatikus indítási küszöb. A csatlakoztatás nélkül szabadon hagyott GPIO zaja nem alkalmas küszöb beállítására: előbb a dokumentált előfeszítő áramkört és biztonságos próbaforrást teszteld.

A **Mikrofon jel** külön kapcsolható grafikonja a GND-re kötött L/R lábhoz tartozó bal I2S-csatorna 100 ms-os RMS- és csúcsértékeit mutatja 16 bites PCM-egységekben. Méréskor, felvétel nélkül az I2S-mikrofont olvassa, de nem nyit WAV-ot és nem ír SD-re; mikrofonfelvétel közben ugyanazokat a mintákat méri. A GPIO1 webes/serial mérésével és élő hallgatásával egyszerre nem indítható. Az oldal elhagyásakor vagy 4 másodperc lekérdezés nélkül kikapcsol.

Az **Élő hang** részen a **Hallgatás indítása** gomb a GPIO1 mintáit 22 050 Hz-es, 16 bites mono PCM-ként továbbítja WebSocketen (81-es port) a telefon böngészőjébe. A böngésző rövid pufferrel, állítható hangerővel játssza le; tényleges késése a Wi-Fi-től függ. A hallgatás alapból ki van kapcsolva, nem indít fájlfelvételt, és SD-kártya nélkül is működik. Felvétel közben ugyanazt az ADC-folyamot használja, egyszerre egy hallgatót enged; az oldal elhagyásakor a kapcsolat lezárul. iPhone-on a néma mód elnémíthatja az élő Web Audio-hangot akkor is, ha a mentett WAV lejátszható; néma élő hangnál kapcsold ki a néma módot, ellenőrizd a médiahangerőt, és próbáld ki a **Hangpróba** gombot. A szabadon hagyott bemeneten főleg zaj hallatszik, és ez nem teszi csatlakoztathatóvá a rádiót.

A **3,4 kHz-es zajszűrő** alapból ki van kapcsolva. A főoldali jelölőnégyzettel vagy Wi-Fi nélkül a 115200 baudos soros monitorban az `f` karakterrel kapcsolhatod. A másodrendű aluláteresztő a mentett WAV-ra és az élő hangra hat, az ADC RMS/csúcs/DC-mérésre nem. A próbazene csendes szakaszán a számított zaj-RMS kb. 65-ről 39 PCM-egységre csökkent; a magas hangok is tompulhatnak, és a Wi-Fi pattogását nem javítja. Készíts két külön, Wi-Fi nélküli felvételt ugyanazzal a jellel, egyszer kikapcsolt, egyszer bekapcsolt szűrővel; a fájl nem tárolja a szűrő beállítását. Újraindítás után ismét kikapcsolt állapotból indul.

A felvétel 22 050 Hz-es, 16 bites mono PCM WAV (a belső ADC valójában 12 bites). Nincs még hangérzékelés, előpuffer, időbélyeg vagy rádióhoz illesztett bemenet. Áramvesztés közben az éppen írt WAV sérülhet. A **nyitott Wi-Fi-n bárki a közelben** elérheti a felvételeket, a vezérlést és az élő hangot; csak ellenőrzött próbakörnyezetben használd. Valós hardveres próba még szükséges.
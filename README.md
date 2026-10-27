# RadioRecorder

ESP32-S3 prototípus kézi, mono WAV-felvételhez microSD-kártyára. Nyitott saját Wi-Fi hálózatán böngészőből indítható és állítható le; a lezárt felvételek visszahallgathatók és letölthetők.

A jelenlegi és tervezett modulok GPIO-kiosztása az [eszközjegyzékben](docs/device-gpio-map.md) található.

## Első, rádió nélküli próba

1. Ellenőrizd a saját panelen, hogy a [próbarajzon](docs/radio-receive-adc.svg) szereplő GPIO1 valóban ADC1_CH0, és az SD-foglalat lábai CMD=GPIO38, CLK=GPIO39, DATA0=GPIO40. A panelre vonatkozó [figyelmeztetések](docs/functional-hardware.md) továbbra is érvényesek.
2. A rajz szerinti előfeszített ADC-áramkört csak ismert, kis szintű, az ESP-vel biztonságosan közös referenciájú próbaforrással építsd meg. A rádió egyik érintkezőjét se csatlakoztasd. GPIO1-en nyugalomban kb. 1,65 V-ot, jellel kb. 0,3-2,7 V közötti csúcsokat ellenőrizz; a GPIO soha ne kerüljön 0 V alá vagy 3,3 V fölé.
3. Helyezz be FAT32-formátumú microSD-t, és tápláld a panelt a saját USB-csatlakozójáról. Kamera, transzformátor és rádióbekötés ehhez a próbához nem kell.

## Fordítás és használat

PlatformIO: `pio run -e esp32-s3-devkitc-1`, majd a tényleges panel és port ellenőrzése után `pio run -e esp32-s3-devkitc-1 -t upload`. A soros monitor sebessége **115200 baud** (`monitor_speed = 115200`). Bootkor a firmware verziója, az SD, ADC, felvevő és a Wi-Fi AP IP-címe jelenik meg; felvételkor a kezdés, leállítás, fájlnév és hiba is. A monitorban az `i` karakter elküldésével bármikor újra lekérhető a státusz a verzióval együtt. Mivel a monitor nem reseteli a panelt (`monitor_rts = 0`, `monitor_dtr = 0`), az indulási üzenetekhez nyitott monitor mellett nyomd meg a panel reset gombját.

A főoldal és a fájlkezelő fejlécében is a futó firmware verziója látszik. A `src/main.cpp` fájl `kFirmwareVersion` konstansában lévő alapverziót **minden kódváltoztatáskor kézzel növeljük**; a végére fűzött buildszámot a PlatformIO feltöltéskor automatikusan növeli. A tartós számláló a `firmware-build.txt` fájlban van, ezt ne töröld: sima fordításkor nem változik, sikertelen feltöltéskor sem lép előre. Például a 0.1.2.0 csak helyben fordított változat, az első sikeres feltöltés 0.1.2.1. A fordítás nem tölti fel automatikusan a firmware-t. A weboldalon új verzió csak a feltöltés és újracsatlakozás után jelenik meg.

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

Az enkóder öt vezetéke a rádiótól és a GPIO1 előfeszítő körétől különálló. A konkrét panelen ellenőrizd, hogy a GPIO14, GPIO21 és GPIO47 valóban kivezetett és szabad. Feltöltés után egy kattanás **jobbra** indítja a WAV-felvételt, a következő jobbra fordítás leállítja; egy kattanás **balra** bekapcsolja az ADC soros mérését, a következő balra fordítás kikapcsolja. Várj legalább 300 ms-ot két kapcsolás között: a gyors, többszörös tekerést a firmware egy műveletként kezeli. Wi-Fi nélkül is 0,5 másodpercenként megjelenik a 115200 baudos UART0 monitorban az RMS, csúcs és DC-közép (12 bites ADC-egység). Felvétel közben is mér; az RMS-sor elején a fájlnév és az ADC-felvétel indulása óta eltelt idő (`t=... s`) is látszik, például `REC REC00052.wav | t=1.500 s | GPIO1 RMS: 42 | Csucs: 130 | DC-kozep: 1882 (ADC)`. A balra kikapcsolt soros mérés a webes mérőkapcsolót nem érinti. Ha a két irány felcserélődik, a CLK és DT vezetékeket **áramtalanítva** cseréld meg. A SW lenyomása pergésvédelemmel kikapcsolja, következő lenyomása visszakapcsolja az AP-t. Kikapcsoláskor a weboldal és az élő hang megszakad, de a folyamatban lévő WAV-felvétel megy tovább; a felvételt Wi-Fi nélkül jobbra fordítással is leállíthatod. Visszakapcsolás után újra csatlakozni kell a RadioRecorder hálózathoz. A korábbi időzített Wi-Fi zajteszt alatt a forgatással nem lehet a felvételt kapcsolni, és a gomb nem kapcsolja a Wi-Fi-t.

Indulás után csatlakozz a **RadioRecorder** nevű, jelszó nélküli Wi-Fi hálózathoz, majd nyisd meg a `http://192.168.4.1` címet. A felvételt a weboldalon indítsd és állítsd le. Leállítás után az `REC00001.wav` jellegű fájlok megjelennek a listában. Ha egy fájl kétszer látszott, a lista egyidejű frissítéseit a firmware most kizárja; ha a hiba újrafeltöltés után is jelentkezik, ellenőrizd a kártyán lévő fájlneveket. Egyszerre egy felvétel készül.

A **Wi-Fi zajteszt** előtt állítsd le az élő hallgatást; a rádió és a próbajelforrás maradjon leválasztva, a GPIO1 előfeszítése maradjon bekötve. A gomb egy WAV-ot rögzít: az első kb. 5 másodpercben a Wi-Fi AP működik, a következő kb. 10 másodpercben ki van kapcsolva. Ezalatt a telefon kapcsolata megszakad; a firmware a WAV lezárása után újraindul, és az AP visszatér. Ezután csatlakozz újra és hallgasd meg ugyanannak a fájlnak a két részét. Ne kapcsold le a tápot a felvétel alatt: a lezáratlan WAV megsérülhet. Ha a fájl nem tartalmaz mindkét szakaszból hangot, a próba nem értékelhető.

A **Fájlkezelő** linken (`http://192.168.4.1/files`) az SD-kártya mappái és fájljai 20-as lapokban böngészhetők. WAV, MP3, M4A, OGG, FLAC, AAC és WebM hangfájlokhoz lejátszó nyitható; a tényleges lejátszhatóság a telefon böngészőjétől és a fájl kódolásától is függ. Bármely fájl letölthető. Az SD olvasása felvétel alatt tiltott; az éppen készülő fájl csak lezárás után jelenik meg.

### Lejátszás az ESP hangszóróján (rádió nélkül)

Áramtalanítva csatlakoztass MAX98357A I2S erősítőt és külön **4 Ω / 3 W hangszórót**. A panel lábkiosztását és az USB-ről származó 5 V-os tápot a saját példányodon ellenőrizd:

| MAX98357A | ESP32-S3 / hangszóró |
| --- | --- |
| BCLK | GPIO42 |
| LRC / LRCLK | GPIO41 |
| DIN | GPIO48 |
| VIN | ellenőrzött USB 5 V |
| GND | ESP GND |
| SPK+ / SPK- | kizárólag a külön hangszóró két kivezetése |

A GPIO48 a panel beépített RGB LED-jével közös: az I2S adatfolyamtól a LED világíthat vagy villoghat, de ez önmagában nem bizonyítja, hogy halkítja a hangot. Zaj vagy tápingadozás esetén a DIN később külön, szabad GPIO-ra helyezhető át a firmware lábkiosztásával együtt. A hangszóró egyik vezetékét se kösd GND-re. **A SPK+/SPK- kimenetet és a próbához használt erősítőt ne csatlakoztasd a rádió MIC-bemenetéhez; PTT-t ez a firmware nem kapcsol.** Alacsony hangerővel kezdd a próbát: a firmware induláskor a WAV mintáit 25%-ra állítja. A 100% a teljes digitális jelszint, nem az erősítő vagy a hangszóró névleges teljesítményének százaléka.

A MAX98357A **GAIN** lába a chip hardveres erősítését választja ki, nem szoftveres hangerőbemenet. A [MAX98357A modul dokumentációja](https://learn.adafruit.com/adafruit-max98357-i2s-class-d-mono-amp/pinouts) szerint a szabadon hagyott GAIN alapból 9 dB; közvetlen GND-re kötve 12 dB, 100 kΩ-on keresztül GND-re kötve 15 dB. Más modul saját ellenállása módosíthatja az alapállapotot, ezért annak kapcsolását ellenőrizni kell. Egyelőre hagyd szabadon; későbbi változtatás előtt áramtalanítsd a panelt. Az erősítő tényleges kimenő teljesítményét az ellenőrzött VIN és a hangszóró is korlátozza.

A főoldali felvétellistán és a Fájlkezelő WAV-fájljainál az **ESP-n lejátszás** gomb a microSD-ről közvetlenül az I2S erősítőre küldi a hangot; a böngésző saját hangszóróján továbbra is a külön lejátszóval hallgathatod. Az ESP csak **22 050 Hz-es, 16 bites, mono PCM WAV** fájlokat játszik le, MP3-at vagy más WAV-kódolást nem. A külön **ESP hangszóró és próbajel hangereje** csúszka 0–100% között, lejátszás közben is állítható; alatta a nyilak 5%-kal léptetnek, az érték mindkét oldalon megjelenik. A **Lejátszási erősítés** választó ettől függetlenül, lejátszás közben 1×/2×/4× digitális szorzást ad a halk WAV-hoz; újraindítás után 25% és 1× az alapérték. A túl nagy WAV-csúcsoknál a firmware korlátozza a kimenő szintet: a hangos felvétel nem lesz korlátlanul erősíthető, és a zaj is felerősödhet. Kis hangerővel, 1×-ről indulva próbáld; torzításnál válts vissza. Ezek a beállítások az ESP-hangszóróra és a külön PDM-próbakimenetre egyaránt hatnak, a böngészős élő hang hangerőcsúszkája ettől független. Egyszerre egy fájl szólhat; leállítani mindkét oldalon az **ESP-lejátszás leállítása** gombbal lehet. Felvétel és ESP-lejátszás nem indulhat egyszerre, és az élő hallgatás sem indítható ESP-lejátszás közben. A Wi-Fi kézi kikapcsolásakor a már elindult helyi lejátszás tovább fut, de a webes leállításhoz előbb vissza kell kapcsolni az AP-t.

### Szűrt próbakimenet rádió nélkül

Lejátszáskor a MAX98357A mellett az ESP32-S3 **I2S0 PCM→PDM** hardvere a WAV hangját GPIO2-re is kiadja; a hangszóró I2S1-en marad. Ez **PDM, nem PWM**: a GPIO2-n nagyfrekvenciás, 0–3,3 V-os digitális impulzusok vannak, nem analóg MIC-jel. A PDM inicializálási/írási hibáját a soros monitor jelzi, a hangszóró ettől még működhet. A GPIO2 a panelrajzon LED-hez is tartozhat; áramtalanítva azonosítsd a ténylegesen kivezetett GPIO2-t és az esetleges LED-terhelést. Ha a pin nem szabad vagy a jel nem mérhető, másik lábat csak a firmware kiosztásának módosításával használj. A PDM-ágat **ne** csatlakoztasd a MAX98357A vagy a PAM8403 hangszórókimenetére.

Az első próba az ESP saját GND-jére hivatkozó, **csak mérőműszerrel terhelt** hálózat. Két 1 kΩ-os ellenállás és két 10 nF-os kondenzátor szűri az impulzusokat (fokozatonként kb. 16 kHz); 100 nF soros kondenzátor blokkolja a kb. fél tápfeszültségnyi DC-t. A 100 kΩ / 1 kΩ osztó nagy impedanciájú mérőbemenet mellett kb. századára csökkenti a hangfrekvenciás szintet. Ezek kiinduló értékek, nem mért rádió-MIC illesztési adatok.

![GPIO2 PDM-kimenet kétfokozatú RC-szűrővel, soros csatolókondenzátorral, 100 kΩ / 1 kΩ osztóval és csak mérőműszerre kötött D ponttal; a rádió nincs bekötve](docs/radio-pdm-output.svg)

[Kapcsolási rajz megnyitása nagy méretben](docs/radio-pdm-output.svg)

Alacsony hangerőn (25%, 1×) indíts ismert, 22 050 Hz-es PCM WAV-ot. Lejátszás közben a B ponton DC-t (közel 1,65 V csendben), a D ponton közel 0 V DC-t és kis AC-hangjelet ellenőrizz. Szkóppal ellenőrizd a D ponton a beszédjel alakját, a csúcskitérést, a PDM-vivő maradékát és a leállítás/indítás tranziensét is; csak multiméterrel a nagyfrekvenciás maradék nem igazolható. A D pont terhelése és az osztó a későbbi rádió-bemenethez még újratervezendő. **Sem a GPIO2-t, sem az A/B/C/D pontokat, sem az ESP GND-t ne kösd most a rádióhoz.** A telefon korábbi 470 Ω-os vezetéke már analóg telefonkimenetet kötött be, nem helyettesíti a szűrést, a DC-leválasztást vagy a MIC-jelszint mérését. Rádiós teszthez előbb a pontos 3,5 mm-es MIC+/MIC− érintkezőket, az ottani előfeszítést és a földelés/leválasztás módját kell ellenőrizni a konkrét készüléken; a kézi PTT önmagában nem teszi biztonságossá a hangbekötést.

A **GPIO1 jel** mérése induláskor ki van kapcsolva. A **Mérés bekapcsolása** gombbal 100 ms-os blokkokból számolt RMS- és csúcsértékek 30 másodperces grafikonja jelenik meg, továbbá a nyers DC-középérték (mindhárom 12 bites ADC-számlálási egységben). Kikapcsolva, felvétel és élő hallgatás nélkül az ADC nem mintavételez; felvétel közben a grafikon bekapcsolva a rögzítés mintáit használja. Az oldal bezárásakor vagy a lekérdezés több mint 4 másodperces megszűnése után a figyelés leáll. Ez még nem automatikus indítási küszöb. A csatlakoztatás nélkül szabadon hagyott GPIO zaja nem alkalmas küszöb beállítására: előbb a dokumentált előfeszítő áramkört és biztonságos próbaforrást teszteld.

Az **Élő hang** részen a **Hallgatás indítása** gomb a GPIO1 mintáit 22 050 Hz-es, 16 bites mono PCM-ként továbbítja WebSocketen (81-es port) a telefon böngészőjébe. A böngésző rövid pufferrel, állítható hangerővel játssza le; tényleges késése a Wi-Fi-től függ. A hallgatás alapból ki van kapcsolva, nem indít fájlfelvételt, és SD-kártya nélkül is működik. Felvétel közben ugyanazt az ADC-folyamot használja, egyszerre egy hallgatót enged; az oldal elhagyásakor a kapcsolat lezárul. iPhone-on a néma mód elnémíthatja az élő Web Audio-hangot akkor is, ha a mentett WAV lejátszható; néma élő hangnál kapcsold ki a néma módot, ellenőrizd a médiahangerőt, és próbáld ki a **Hangpróba** gombot. A szabadon hagyott bemeneten főleg zaj hallatszik, és ez nem teszi csatlakoztathatóvá a rádiót.

A **3,4 kHz-es zajszűrő** alapból ki van kapcsolva. A főoldali jelölőnégyzettel vagy Wi-Fi nélkül a 115200 baudos soros monitorban az `f` karakterrel kapcsolhatod. A másodrendű aluláteresztő a mentett WAV-ra és az élő hangra hat, az ADC RMS/csúcs/DC-mérésre nem. A próbazene csendes szakaszán a számított zaj-RMS kb. 65-ről 39 PCM-egységre csökkent; a magas hangok is tompulhatnak, és a Wi-Fi pattogását nem javítja. Készíts két külön, Wi-Fi nélküli felvételt ugyanazzal a jellel, egyszer kikapcsolt, egyszer bekapcsolt szűrővel; a fájl nem tárolja a szűrő beállítását. Újraindítás után ismét kikapcsolt állapotból indul.

A felvétel 22 050 Hz-es, 16 bites mono PCM WAV (a belső ADC valójában 12 bites). Nincs még hangérzékelés, előpuffer, időbélyeg vagy rádióhoz illesztett bemenet. Áramvesztés közben az éppen írt WAV sérülhet. A **nyitott Wi-Fi-n bárki a közelben** elérheti a felvételeket, a vezérlést és az élő hangot; csak ellenőrzött próbakörnyezetben használd. Valós hardveres próba még szükséges.
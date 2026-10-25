# Hardveres megközelítések - kutatási jegyzet

Ez a dokumentum a lehetséges irányok értékelését gyűjti össze. A követelmények a [követelménytervezetben](../requirements.md) szerepelnek; a jelen jegyzet nem végleges alkatrészlista.

## Első ellenőrizendő eltérés: a meglévő panel

A [megvásárolt ESP32-S3 panel termékoldala](https://www.hestore.eu/prod_10048970.html) **ESP32-S3-N16R8-M-OV5640** modellt, OV5640 kamerát, 16 MB flash-t, 8 MB PSRAM-ot és microSD-foglalatot ír. A helyi [pinout-jegyzet](pinout.md) ezzel szemben OV2640 kamerát és arra alapozott GPIO-hozzárendelést tartalmaz. A pinoutot ezért a tényleges panel revíziójával és kapcsolási rajzával kell összevetni, mielőtt bármely GPIO-t, audio- vagy SD-buszt véglegesítünk.

## Először a teljes rendszer energiaigénye

A rádiónak a vételre **168 órán át** készen kell állnia; a 2025-ös archívum 13,85 órányi *rögzített* hangja nem 13,85 órányi rádió-üzemidőt jelent. A felvevőnél is folyamatosan kell hangot figyelni és az előpufferbe mintát venni; az ESP chip mélyalvási adatát nem lehet a működő panel fogyasztásának tekinteni. A Wi-Fi napi kétszer 10 percig kapcsol be; a rádió RX/TX, a hangút, SD, RTC, szabályozók és készenléti veszteségek ettől függetlenül fogyasztanak.

Egy [3,6 V névleges, 3400 mAh-s 18650-es cellával](https://www.hestore.eu/prod_10049687.html) szemléltetve egy cella 12,24 Wh névleges energiát jelent. **Négy cella csak példa, nem korlát: legfeljebb 4 x 4, azaz 16 cella férhet bele.** Ez fizikai maximum, nem előírt 4S4P elektromos kapcsolás. Ha *feltételesen* 80% használható a rendszer bemenetén, az alábbi hét napos keretek adódnak a **rádióra és felvevőre együtt**:

| 18650 cellák száma | Névleges energia | Feltételezett használható energia (80%) | Maximális teljes átlag 168 órához |
| ---: | ---: | ---: | ---: |
| 4 | 48,96 Wh | 39,17 Wh | 0,233 W |
| 8 | 97,92 Wh | 78,34 Wh | 0,466 W |
| 12 | 146,88 Wh | 117,50 Wh | 0,699 W |
| 16 | 195,84 Wh | 156,67 Wh | 0,933 W |

A ténylegesen kivehető energia cellaállapottól, terheléstől, lekapcsolási feszültségtől és konverziós veszteségektől függ; a 80% nem mérési eredmény. A szükséges soros/párhuzamos topológia nem következik a cellaszámból. Ha a rádió külön akkumulátoron van, a fenti keret **nem** fedi le annak energiáját: a teljes rendszerben az abban tárolt és felhasznált Wh-t is külön számolni kell.

| Példa a *teljes* akkumulátorról vett átlagteljesítményre | 168 órás energiaigény | 39,17 Wh-ból adódó idő, csak szemléltetés |
| --- | ---: | ---: |
| 0,233 W | 39,1 Wh | 168 óra |
| 0,5 W | 84 Wh | 78 óra |
| 1,0 W | 168 Wh | 39 óra |
| 1,75 W | 294 Wh | 22 óra |

Az utolsó sor a Pi Zero 2 W [hivatalos 350 mA @ 5 V tipikus aktív, *csupasz panel* adatának](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#power-supply) 1,75 W-ra váltása, nem teljes rendszerre mért adat. Ha a Pi tényleges üzemi átlaga ennyi, önmagában túllépi még a **16 cellás, 80%-os példakeretet** is. A [UV-5R termékadata](https://baofengtech.com/product/uv-5r/) 7,4 V-os, cserélhető akkumulátort és antennát ír, de nem ad hiteles, adott példányra érvényes RX-áramprofilt. Például **feltételezett** 75 mA folyamatos rádióáram már 7,4 V x 0,075 A = 0,555 W, azaz 93 Wh / 168 óra **csak a rádióra**; ez *nem mért UV-5R adat*. Ez kizárná a **négycellás** változatot, de nem a 8-16 cellás rendszert; utóbbiban a felvevőnek és a veszteségeknek is bele kell férniük.

A 8-10. nap napi 1-2 órás töltéséhez külön mérleg kell: például 1 W átlag napi 24 Wh elhasznált energia; 2 óra töltés alatt legalább 12 W *nettó akkumulátortöltés* kellene, a közben működő terhelés és veszteségek felett. USB-C csatlakozó önmagában nem garantál tetszőleges töltési teljesítményt: adapter, kábel, USB-C áram/PD-megállapodás, cellatöltési korlát és hőmérséklet alapján kell méretezni. A rádióakku külön töltése nem teljesíti az egyetlen külső USB-C csatlakozós célt.

## Három vezérlő mint teljes rendszer

Mindháromhoz kell egy, a célnak megfelelő és jogszerű rádió, hangillesztés, helyi hangszóró, RTC, tároló, akkumulátorvédelem, USB-C töltés és megszakítás nélküli terheléstáp. Ezek költsége és fogyasztása nem tűnik el az olcsó vezérlőpanel választásától.

| Szempont | ESP32-S3 | Raspberry Pi Pico 2 W | Raspberry Pi Zero 2 W |
| --- | --- | --- | --- |
| Alap | [ESP-IDF, Wi-Fi, 2 I2S és SDMMC](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html); a meglévő kamera + SD panel | [RP2350, 520 kB RAM, 4 MB flash, Wi-Fi, SPI/PIO](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html#wireless_pico2); külső SD | [Linux SBC, Wi-Fi, microSD](https://www.raspberrypi.com/products/raspberry-pi-zero-2-w/); USB/I2S hanghardver kell |
| Hangleképezés | I2S ADC/DAC kodek, esetleg külön I2S mikrofon és helyi I2S erősítő; meglévő panel GPIO/SD ütközés vizsgálandó | SPI/PIO SD és külső audio osztozik az erőforrásokon; 520 kB mellett webszerver + fájllista + puffer próbát igényel | ALSA-kompatibilis USB/I2S audio interfész, helyi erősítő; kernel/driver kompatibilitás próbálandó |
| Szoftver és megbízhatóság | ESP-IDF-ben célzott audio/SD/web feladatok; valós idejű PTT és áramvesztéses fájllezárás megoldandó | Pico SDK/lwIP mellett több integráció saját munka; kis RAM és SD/Wi-Fi konkurencia a fő kockázat | Fájlrendszer és web UI könnyebb, de bootidő, sérülékenyebb SD/OS és rendezett leállás kérdéses |
| Energia és újraépíthetőség | A panelt egészében mérni kell; végleges példányhoz dokumentált S3 modul + ismert SD-kötés előnyösebb lehet | A [gyári kapcsolási rajz](https://pip.raspberrypi.com/documents/RP-008306-DS) jó újraépítési alap; nem igazolt, hogy teljes rendszerben kevesebbet fogyaszt az S3-nál | Csupasz panel tipikus aktív 1,75 W; még 16 ilyen cella mellett is meghaladja a példakeretet, ha valóban ennyi az átlaga |
| Csak vezérlőpanel példaára | [OV5640 panel: nettó 11,235 EUR/db](https://www.hestore.eu/prod_10048970.html) | [Pico 2 család 5 USD-től](https://www.raspberrypi.com/products/raspberry-pi-pico-2/), ez **nem** feltétlenül a W modell ára | [Zero 2 W: 15 USD-től](https://www.raspberrypi.com/products/raspberry-pi-zero-2-w/); készlet, helyi adó és szállítás eltér |

Az árak külön pénznemű, tájékoztató panelárak, nem összemérhető végösszegek. Például a négy fenti cella [nettó 4 x 3,342 EUR](https://www.hestore.eu/prod_10049687.html) a listázott 1 db-os áron, ehhez még mechanika, védelem, töltő és a rádió is szükséges. Beszerzés előtt teljes, azonos követelményeket teljesítő BOM-ot kell összeírni. Az ESP32-S3 jelenleg a legjobb *első kísérleti vezérlő*, mert már rendelkezésre áll és kényelmesebb a hang + SD + kapcsolható Wi-Fi integráció; ez nem bizonyított hét napos végső választás. A Pico 2 W érdemi MCU-alternatíva, ha a mért összfogyasztás és a firmware-próba mellette szól; a Zero 2 W akkor érdekes, ha a web/szoftveres időnyereség vagy nagyobb akkumulátor fontosabb a fogyasztásnál.

## Hang-, PTT- és rádióhatár

- A rádió hangszórókimenete **nem** közvetlen MCU-mikrofonbemenet: előbb meg kell mérni a DC-eltolást, az AC csúcsot, a csatlakozó érintkezőit, a hangerőállást és a földviszonyt, majd illeszteni (osztó/csatolás, bemenetvédelem, szükség esetén leválasztás) a kodek/ADC megengedett tartományára. [TLV320AIC3204](https://www.ti.com/product/TLV320AIC3204) csak lehetséges ADC+DAC alkatrész, nem kész UV-5R illesztőmodul. Az S3 [saját ADC-je](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/adc/index.html) kipróbálható, de zaj/torzítás és Wi-Fi melletti felvétel alapján döntsünk a kodekről.
- A visszajátszás rádió mikrofonbemenetére *mikrofonszintű*, megfelelően csatolt és beállított DAC-jelet igényel, nem hangszóró-kimenetet; a helyi hangszóró külön erősítőt kap (például [MAX98357A](https://www.analog.com/en/products/max98357a.html)). A meglévő [INMP441 modul](https://www.hestore.eu/prod_10043182.html) digitális helyi mikrofon, nem a rádió analóg audio bemenete. A visszacsatolás/gerjedés, zaj és PTT alatti hangút külön próba.
- A [helyi jack/PTT-vázlat](UV5R%20audio%20interface.md) nem igazolt pinout; gyári adapter, mérés és célrádió alapján ellenőrizzük. A relé alapállapotban PTT-t elengedő legyen; áramkimaradás, reset, beragadt vezérlés és fájlvége esetére független adásidő-korlát, kézi leállítás szükséges. A rádió tényleges TOT-ját és újrabillentyűzését mérjük; az 1 másodperces rés nincs garantálva.
- Egy fél-duplex rádió a saját adása alatt nem vesz; a vételi elsőbbség itt az adás elhalasztását/megszakítását jelenti, **nem** folyamatos, hiánytalan rögzítést. Ha adás közben is teljes vétel kell, második külön vevő és annak tápja kellene. Külön helyi mikrofon/felvételi és rádióhang-útvonalat, valamint jogosulatlan automatikus újraadás elleni vezérlést tervezzünk.

## USB-C, cellák és rádióakku

A [TP4056 USB-C modul](https://www.hestore.eu/prod_10042093.html) egyetlen 4,2 V-os Li-ion cella töltésére való; nem igazolt rajta a töltés közbeni *rendszer*-power-path, és nem 2S rádióakku-töltő. A [BQ24074](https://www.ti.com/product/BQ24074) és [BQ25895](https://www.ti.com/product/BQ25895) egycellás töltés/power-path IC-jelöltek, de az IC funkciója nem igazolja a konkrét kész modul 4 cellás konfigurációját, hő- és zárlatvédelmét vagy a UV-5R kétcellás akkumulátorának töltését. A négycellás shield 1S4P/2S2P/egyéb belső kapcsolása, kiegyenlítése, védelme és terhelés melletti viselkedése ismeretlen: dokumentáció és mérés nélkül nem szabad kész táprendszerként használni vagy különböző állapotú cellákat összekötni.

Első biztonságosabb *mérési topológia*: a rádió a gyári akkumulátorán és gyári töltőjén, a felvevő külön, megfelelően védett USB-C-s tápon; így a hangkapcsolat és külön terhelések kipróbálhatók. Ez **nem teljesíti** az egyetlen csatlakozós, töltés közbeni közös üzem végső követelményt. Annak külön tervezési ága lehet megfelelő méretű, egyetlen védett akkucsomag + a választott cellakonfigurációhoz tervezett töltő/BMS + igazolt load sharing + szabályozott külön 5 V/rádió-táp; a rádió külső táplálását csak a pontos típushoz jóváhagyott megoldással és ellenőrzött feszültség-/TX-áram-tartalékkal szabad kipróbálni. Másik lehetőség két független akkucsomag egy USB-C bemenetről két **külön, hozzájuk való** töltő/power-path ággal. A rádió saját USB-C-s akkumulátorának töltés közbeni rádióüzeme sem feltételezhető: adatlap és próba kell. Mindkettőnél számolni kell USB-C forrásképességgel, PTT-csúcsárammal, hővel és töltés alatti SD-/hangkimaradás teszttel; ez nem végleges kapcsolási rajz.

## Szabályozási és adatkezelési kapu

Az aktív [ECC/DEC/(15)05 PMR446 döntés](https://docdb.cept.org/document/448) a 446,0-446,2 MHz sáv és az egyedi engedély alóli mentesítés *meghatározott műszaki feltételeit* tárgyalja; a magyar alkalmazás és a választott eszköz megfelelősége külön ellenőrizendő. A [UV-5R gyártói termékoldala](https://baofengtech.com/product/uv-5r/) amatőrrádiós, 1/4 W-os, cserélhető antennás eszközként mutatja be: a 446,03125 MHz beállíthatósága **nem** jelent PMR446-os adásjogosultságot. Ne engedélyezzünk automatikus vagy kézi adást a célhelyszínen, amíg a rádió pontos változatának PMR446-megfelelősége és a helyi szabályozás nincs tisztázva; szabályos PMR446 készülék választása is megváltoztathatja az audio/PTT interfészt. A beszélgetések rögzítésére, tárolására és újraadására vonatkozó jogi/adatkezelési feltételek a rádió műszaki megfelelőségétől **külön** vizsgálandók.

## Első mérési kapuk, döntési sorrendben

1. A pontos rádióváltozat és használati hely alapján tisztázni a PMR446 adást, felvételt és újraadást; adáspróba csak jogszerű körülmények között, szükség esetén műterheléssel/szakértővel.
2. A rádió gyári akkumulátorával mérni a Wh-t vagy feszültség+áram időprofilját: squelch-zárt készenlét, gyakori vétel, különböző hangerő, TX és TX-indítás. Ugyanígy a **teljes** ESP32 panel + SD + hang + RTC felvétel közben, Wi-Fi kikapcsolva/be, és töltéskor. Mérőeszköz a DC bemeneti oldalon, többórás naplózással; ne tévesszük össze az RF-adóteljesítményt a DC-terheléssel.
3. A mért profilból hét napra Wh/nap és napi 1-2 órás nettó feltöltési számítás 4-16 cellás jelöltekkel; ha a rádió önmagában meghaladja a 0,233 W keretet, csak a négycellás példát kell elvetni. Ha a teljes rendszer mért átlaga a 16 cellás példakeretet is meghaladja, más cella/kapacitás, rádió vagy üzemmodell kell, illetve a követelményt újra kell egyeztetni.
4. A meglévő OV5640 panel pontos kapcsolási rajza, SD-busz és kivezetések; jack-hangszintek, PTT és audio kimenet/bemenet védett méréses próbája. A pre-roll és több ezer fájlos webes böngészés párhuzamos SD-írással tesztelendő.
5. Az előírt USB-C csatlakozón működtetés töltés mellett: kihúzás/bedugás, teljes akku, merülő akku, TX-csúcs, termikus védelem, hangzaj és fájlintegritás. Csak a konkrét táprendszer és rádió jóváhagyott kapcsolása után véglegesítsünk BOM-ot és GPIO-t.
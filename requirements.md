# Rádióforgalmazás-felvevő - követelménytervezet

Ez a dokumentum a kívánt működést és a nyitott kérdéseket gyűjti össze. A felsorolt alkatrészek rendelkezésre állnak vagy felmerültek, de még nem jelentenek végleges műszaki döntést.

## 1. Cél és használat

Egy hordozható eszköz rögzítse a Baofeng UV-5R vagy hasonló rádió hangkimenetén érkező forgalmazást, és tegye elérhetővé a felvételeket helyben és telefonról. A felvett hangot lehessen a rádión keresztül újra kisugározni; helyi beszédet is lehessen rögzíteni vagy a rádión továbbítani.

A cél egy akár 10 napos airsoft LARP-rendezvény rádióforgalmazásának rögzítése az elejétől a végéig. Egy ilyen eseményen jellemzően 100-200 résztvevő van, és gyakori a rádióforgalmazás; a rögzítési és energiaigényeket ehhez kell mérni.

## 2. Eddig megfogalmazott igények

### Rögzítés és hangútvonalak

- A rádió jack csatlakozóján érkező hangot az eszköz rögzíti.
- Egy időben egy, a rádión beállított csatornát kell rögzíteni; jellemző cél a PMR446 3. csatornája (446,03125 MHz). Több csatorna párhuzamos rögzítése nem követelmény.
- A rádióból érkező hang indítson automatikus felvételt: hangszünetben ne készüljenek fölösleges felvételek. Egy elindult felvétel csak az állítható csendidő letelte után álljon le. A hang elejének megőrzése, az érzékelési küszöb és a csendidő szoftveresen állítható legyen; alapértékeiket hardveres próbák után határozzuk meg.
- A felvételek időbélyeggel, SD-kártyára kerüljenek.
- A korábbi felvételeket lehessen a rádión kézzel (egyesével vagy sorba állítva) és automatikusan is visszajátszani, a rádió adását vezérelve. Lehessen véletlenszerű időpontban adást indítani és véletlenszerű felvételt választani. A módok a webes felületen legyenek konfigurálhatók, és onnan kézzel is lehessen adást indítani.
- Az automatikus visszajátszásnál első körben legyen beállítható ismétlődő időköz (például óránként vagy kétóránként). Egy indítás egy felvétel teljes lejátszását célozza; a részletes napszakos és véletlenszerű időzítést később pontosítjuk.
- Rádiós adáskor a vezérlő relével kapcsolja a PTT-t. Ha egy fájl hosszabb a rádió beállított adásidő-korlátjánál (TOT, gyakran kb. 60 másodperc), a lejátszás szüneteljen, a PTT engedjen el, majd új PTT-indítás után folytatódjon. A cél egy másodpercnél rövidebb szünet, nem igazolt működési garancia.
- A saját mikrofon hangja csak a Wi-Fi-t kapcsoló gombtól különálló gombnyomásra kerüljön rögzítésre és/vagy élőben a rádión továbbításra; ne indítsa el a rádióhangra figyelő automatikus felvételt.
- Saját hangszórón is lehessen hangot hallgatni, ne csak a rádión keresztül.
- Egyidejű műveleteknél alapértelmezés szerint a rádióból érkező hang rögzítése élvezzen elsőbbséget; az adás és vétel ütközési szabálya a webes felületen legyen beállítható.
- A hangérzékelés, az adás/vétel prioritás és az egyéb konfigurálható működések beállításai újraindítás után is maradjanak meg EEPROM-ban; a konkrét tárolómodul még nincs kiválasztva.

### Idő és tárhely

- Az időt hálózat nélkül is elemes RTC biztosítsa; telefonról lehessen az órát beállítani vagy szinkronizálni. Felmerült modul: [DS3231+AT24C32 RTCM](https://www.hestore.hu/prod_10038042.html) (RTC + EEPROM, I2C); kiválasztása még nem végleges.
- A felvételek és a keresés időpontjai magyar helyi idő szerint jelenjenek meg (Europe/Budapest, óraátállítással).
- Első becslésekhez 32 GB-os SD-kártyával számoljunk, de ez nem kötelező kártyaméret.
- Ha az SD-kártya megtelik, weben választható működés legyen: új felvétel leállítása a meglévők megtartásával (alapértelmezés), vagy a legrégebbi felvételek törlése az újak számára. A tényleges hangfájlformátum a választott hardvertől és tesztektől függ.
- A webes fájllista több ezer felvétel mellett is legyen használható; az alábbi referencia egy napon akár 900 fájlt tartalmaz.

#### 2025-ös felvételi referencia

A rendelkezésre álló archívum 2025. július 11-19. közötti, kilenc dátummal jelölt mappájában 6122 WAV-felvétel található, összesen 2096,92 MiB méretben és 13,85 óra hangidővel. Minden fájl 22 050 Hz-es, mono, 16 bites PCM WAV; a hangidőt a WAV-fejlécekből számoltuk. Az első fájl július 11-én 12:39-kor, az utolsó július 19-én 23:54-kor indult, tehát nem kilenc teljes nap folyamatos mérése.

| Nap | Felvételek | Méret (MiB) | Hangidő (óra) |
| --- | ---: | ---: | ---: |
| Július 11. | 425 | 132,96 | 0,88 |
| Július 12. | 900 | 265,54 | 1,75 |
| Július 13. | 810 | 233,24 | 1,54 |
| Július 14. | 754 | 218,26 | 1,44 |
| Július 15. | 539 | 188,20 | 1,24 |
| Július 16. | 450 | 151,00 | 1,00 |
| Július 17. | 871 | 372,95 | 2,46 |
| Július 18. | 878 | 335,49 | 2,22 |
| Július 19. | 495 | 199,28 | 1,32 |

Kilenc naptári nap átlagával, azonos forgalom és fájlformátum mellett tíz napra durván 2330 MiB (2,28 GiB), mintegy 15,4 óra hang és 6800 fájl adódna. Ez nem garantált felső korlát: a napok forgalma eltér, az első és utolsó nap csak részben szerepel az archívumban. A minta legnagyobb napi adatmennyisége 372,95 MiB; a legnagyobb fájl 55,36 másodperces volt. A 32 GB-os kártya tárhely szempontjából bőséges kiindulás ennél a formátumnál, de a tényleges méretezéshez tartalékot kell hagyni.

### Webes elérés

- Az eszközön Wi-Fi-n elérhető webszerver fusson; a felületen látszódjon az aktuális rögzítési állapot.
- Az eszköz csatlakozhasson meglévő Wi-Fi-hálózathoz, vagy kínáljon saját hozzáférési pontot, amelyre telefon csatlakozhat.
- A Wi-Fi ne működjön folyamatosan: külön helyi gombnyomásra kapcsoljon be, alapértelmezés szerint 15 perc után automatikusan kapcsoljon ki. A bekapcsolási idő weben állítható legyen. Wi-Fi nélkül a rádiós rögzítés és a már beállított automatikus adásmódok működése folytatódjon.
- A csendidő és a hangérzékelés beállításai a webes felületen legyenek módosíthatók.
- A webes felülethez ne legyen kötelező bejelentkezés; legfeljebb opcionális jelszavas védelem szükséges. Webes konfigurálásra a Wi-Fi aktív időszakában legyen lehetőség.
- A felvételek fájllistában böngészhetők legyenek; lehessen őket legalább nap és óra szerint szűrni.
- Telefonról, böngészőben lehessen a korábbi felvételeket meghallgatni és letölteni.

### Hordozhatóság és tápellátás

- A cél legalább 7 nap akkumulátoros üzemidő a teljes csomagra (felvevő és rádió együtt). Az első tervezési használati profilban a Wi-Fi napi kétszer 10 percig aktív beállításra, ellenőrzésre vagy letöltésre; a rádióforgalmazásra a 2025-ös felvételek szolgálnak támpontként. A rögzített hangidő nem azonos a rádió készenléti vagy adási idejével.
- A tíznapos esemény hátralévő napjait töltéssel hidaljuk át: az egész csomag egy külső USB-C töltőcsatlakozón keresztül, szokásos telefontöltőről legyen tölthető. Töltés közben a rádió és a felvevő működjön tovább, a rögzítés ne szakadjon meg.
- Előzetes feltételezés, hogy naponta 1-2 óra töltési lehetőség lesz; a töltő pontos teljesítménye most nem követelmény.
- A felvevő lehetőleg kompakt, hordozható táskába kerüljön. Felmerült ház: [QBRICK System Pro Toolcase 8012](https://www.alza.hu/barkacs/qbrick-system-pro-toolcase-8012-szerszamoslada-d7865595.htm).
- Felmerült táplálási elképzelés: négy 18650-es cellát fogadó shield, telefonos töltőről tölthető kivitel, 5 V-os kimenet a vezérléshez. A konkrét shield és annak valós képességei még ellenőrzendők.
- Felmerült, hogy egy USB-C-n tölthető UV-5R-akkumulátor is kaphatna energiát a közös tápból; ennek megvalósíthatósága és üzemidőhatása nyitott kérdés.
- A vezérlőhöz rendelkezésre áll egy ESP32-S3-N16R8 CAM kártya SD-kártya-foglalattal; használata még nem végleges döntés.

### Opcionális helyi kezelés

- Ha lesz kijelző és helyi kezelőszerv, a kijelző mutathatja a rögzítési állapotot, a csendidőt és az aktuális IP-címet; a csendidőt helyben is lehessen állítani.
- Forgó enkóderrel (balra/jobbra/nyomás) helyben is lehessen fájlokat böngészni és felvételt lejátszani.

## 3. Javaslatok és ellenőrizendő feltételek

### Javasolt kiegészítő követelmények - jóváhagyásra

Ezek a megbízható használathoz fontosnak tűnnek, de még nem rögzített döntések.

- **Adásbiztonság:** a telefonon vagy a helyi hangszórón indított lejátszás önmagában ne kapcsolja adásba a rádiót. Adás csak külön indításra vagy előzetesen engedélyezett automatikus módban történjen; hiba, újraindulás vagy váratlan leállás esetén az adás álljon le.
- **Felvétel megőrzése:** tápvesztés, megtelt/hiányzó SD-kártya vagy hibás írás esetén az eszköz jelezze a hibát, és lehetőség szerint ne veszítse el a korábbi felvételeket.
- **Időkezelés:** legyen egyértelmű, mit tesz a rendszer hibás vagy lemerült RTC esetén; ilyenkor ne keltsék a felvételek azt a látszatot, hogy ismert a pontos dátum.
- **Állapotjelzés:** a rögzítésen túl az SD-kártya, a Wi-Fi, az akkumulátor és a rádiós adás állapota is legyen megismerhető.
- **Adatkezelés:** a felvételek kézi törlésének szükségességét a webes fájlkezeléshez később pontosítsuk; az SD-kártya megtelési szabálya már rögzített.

### Megvalósíthatósági ellenőrzések

- A célrádió csatlakozóit, hangszintjeit és adásvezérlését ellenőrizni kell. A [meglévő UV-5R audiointerfész-jegyzet](docs/UV5R%20audio%20interface.md) kiindulópont, nem igazolt végleges bekötés.
- A shield és a rádióakkumulátor tényleges kapacitását, a közös USB-C töltést, a töltés közbeni üzemet és a töltés biztonságát ellenőrizni kell.
- A kiválasztott rádió PMR446-os adásra való megfelelőségét külön ellenőrizni kell.
- A célrádión ellenőrizni kell a tervezett relés PTT-vezérlést, a TOT tényleges beállítását, a PTT elengedése utáni újraindítás feltételeit és a szünet hosszát; az egy másodpercnél rövidebb megszakítás jelenleg még nem igazolt. A felvétel és újraadás jogi feltételeit is ellenőrizni kell.

## 4. Nyitott kérdések az első változathoz

1. **Vételi kiesés:** elfogadható-e, hogy a saját adás alatt az egyetlen rádió nem rögzíti a beérkező forgalmazást? A rádió- és mikrofonhang külön fájlokba kerüljön-e?
2. **Adás megszakítása:** hogyan lehessen kézzel vagy vészhelyzetben leállítani a már elindult automatikus rádióadást?
3. **Érvénytelen RTC:** készülhet-e felvétel akkor is, ha nem ismert a pontos idő; ha igen, hogyan legyen megjelölve a fájl?

## 5. Később pontosítandó részletek

- A véletlenszerű adás időzítési tartománya, a választható felvételek köre, az ismétlések és az esetleges napszakos szabályok.
- A hangérzékelés és a csendidő tartományai, alapértékei, valamint a végleges hangfájlformátum a hardveres próbák után.
- Az opcionális jelszó, a kézi felvételvezérlés, a manuális fájltörlés és az eseményjelölés szükségessége.

## 6. Már meglévő vagy felmerült eszközök

Az alábbi lista készlet- és ötletjegyzék, nem kötelező alkatrészlista.

| Eszköz | Hivatkozás |
| --- | --- |
| ESP32-S3-N16R8 CAM, SD-kártya-foglalattal | [Termékoldal](https://www.hestore.eu/prod_10048970.html#) |
| ROT-1AB forgó enkóder, AB+SW, 20 impulzus | [Termékoldal](https://www.hestore.eu/prod_10035572.html) |
| MAX98357A I2S, D osztályú erősítőmodul | [Termékoldal](https://www.hestore.eu/prod_10045704.html) |
| INMP441-M I2S mikrofonmodul | [Termékoldal](https://www.hestore.eu/prod_10043182.html) |
| DS3231+AT24C32 RTCM, felmerült elemes RTC-modul | [Termékoldal](https://www.hestore.hu/prod_10038042.html) |
| D096-12864-I2C-GV-WH, 0,96 hüvelykes, 128×64-es I2C OLED, felmerült kijelző | [Termékoldal](https://www.hestore.hu/prod_10048304.html) |
| TP4056-1A-USBC Li-ion/Li-Po töltőmodul | [Termékoldal](https://www.hestore.eu/prod_10042093.html) |
| MAX4466-M elektretmikrofon-erősítő | [Keresés](https://www.hestore.eu/search.php?q=max4466-M) |
| PAM8403-M, 2 × 3 W-os erősítőmodul | [Termékoldal](https://www.hestore.eu/prod_10042006.html) |
| HW-711 / TPA3116D2 audioerősítő | [Termékoldal](https://electronicsforyou.com/product/hw-711-mini-audio-amplifier-2x25w-tpa3116d2/) |
| 18650 shield v9, négy cellához | [Termékoldal](https://www.aliexpress.com/item/1005005704037349.html) |

Általános elektronikai alkatrészek is rendelkezésre állnak. A kapcsolódó [pinout-jegyzet](docs/pinout.md) és [UV-5R audiointerfész-vázlat](docs/UV5R%20audio%20interface.md) később segíthet a megvalósíthatóság ellenőrzésében.



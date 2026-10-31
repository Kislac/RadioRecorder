# FNIRSI oszcilloszkóp-katalógus: fogalomtár

Ez a dokumentum a [FNIRSI_oszcilloszkopok.xlsx](FNIRSI_oszcilloszkopok.xlsx) oszlopait és a műszaki adatok értelmezését magyarázza. Az adatlap- és árpillanatkép a gyártó oszcilloszkóp-kollekciójából készült; a USD-ben szereplő árak változhatnak, és nem tartalmazzák feltétlenül a szállítást vagy az adókat.

## A táblázat oszlopai

| Oszlop | Jelentés |
| --- | --- |
| Fotó | A gyártó termékoldaláról letöltött és a munkafüzetbe ágyazott kép. A kép a terméksor cellájában van, így rendezéskor a sor adataival együtt mozog. |
| Modell | A készülék rövid típuskódja. Egy sor több, azonos termékoldalon árult változatot is összefoghat. |
| Termék | A gyártó termékneve. |
| Akciós ár (USD) | A kollekcióban látott aktuális, legalacsonyabb „from” ár. Ez numerikus pénzmező, tehát Excelben szám szerint rendezhető. Több csomag vagy változat esetén a magasabb árú csomag nem feltétlenül szerepel ebben a mezőben. |
| Csatornák | Hány bemeneti analóg jelalakot tud a szkóp egyszerre megjeleníteni. A csatornaszám nem azonos a logikai analizátor digitális bemeneteinek számával. |
| Sávszélesség max. (MHz) | A legmagasabb gyártó által megadott analóg sávszélesség. Ha a specifikáció egycsatornás módra vonatkozik, a megjegyzés oszlop jelzi; több aktív csatornával a sávszélesség vagy a mintavétel csökkenhet. |
| Sávszélesség több csatornával | A gyártó által közölt többcsatornás korlát, ha van ilyen. Az „nincs megadva” nem azt jelenti, hogy nincs korlát, csak azt, hogy az adatlap nem részletezi. |
| Mintavétel max. (MSa/s) | A legnagyobb mintavételi sebesség. Ez a mérés során másodpercenként vett minták száma, nem a hullámforma-frissítés gyakorisága. |
| Mintavétel több csatornával | Csatornánkénti sebesség vagy a több aktív csatornára vonatkozó gyártói megjegyzés, ahol megadták. A maximális mintavétel gyakran megoszlik az aktív csatornák között. |
| Memória / rögzítési tár | Az egy felvételbe rögzíthető minták vagy adat mélysége. A gyártók pontban, bájtban vagy bitben is megadhatják, ezért az eltérő egységeket nem szabad közvetlenül összehasonlítani. A hiányzó adatot nem következtettem ki. |
| Kijelző (hüvelyk) | A kijelző átlója. A nagyobb kijelző kényelmesebb, de önmagában nem jelent jobb mérési teljesítményt. |
| Akkumulátor (mAh) | A névleges akkumulátorkapacitás. Az üzemidő a kijelző fényerejétől, mérési módtól és a készülék állapotától függ. |
| Multiméter | Jelzi, ha a készülék digitális multiméter-funkciót is tartalmaz. Ez nem jelenti automatikusan, hogy a szkópcsatornák és a multiméter egyidejűleg használhatók. |
| Generátor max. (MHz) | A beépített jelgenerátor legmagasabb megadott kimeneti frekvenciája. Egyes modelleknél ez csak a szinuszjelre érvényes; más hullámformák alacsonyabb frekvencián működhetnek. |
| Protokolldekódolás | Az adatlapban név szerint feltüntetett digitális buszdekódolás. Az „I²C/SPI/UART/CAN/LIN” azt jelenti, hogy a készülék meg tudja jeleníteni a rögzített jelekből a protokoll adatmezőit; nem minden listázott szkóp tudja ezt. |
| PC-kapcsolat | Rövid minősítés: nincs dokumentálva, fájlexport, Bluetooth-adatkapcsolat korlátozott funkcióval, vagy kifejezetten dokumentált PC-s távkezelés. |
| PC-s kapcsolat részlete | Kifejti, hogy az USB/vezeték nélküli kapcsolat képernyőképet vagy hullámformát exportál-e, illetve lehet-e élőben vezérelni és mérni. Az USB-C töltőcsatlakozó önmagában nem jelent PC-s szkópkapcsolatot. |
| Gyártói termékoldal | Kattintható link az adott FNIRSI-termékoldalra. |

## Műszaki fogalmak

### Analóg sávszélesség

A szkóp bemeneti analóg fokozatának frekvenciatartománya. A megadott határnál a jel amplitúdója már csökkenni kezd; a sávszélesség ezért nem azonos azzal, hogy a készülék minden ilyen frekvenciájú négyszögjelet pontosan megmutat.

Egy négyszögjel élei sok magasabb harmonikust tartalmaznak. Ha a jel frekvenciája közel van a szkóp sávszélességéhez, az élek lekerekednek és a mért amplitúdó is eltérhet. Hobbicélra általános ökölszabály, hogy a vizsgált jel leggyorsabb éléhez legyen bőven tartalék sávszélesség; a szükséges érték a jelalak pontosságától függ.

### Mintavételi sebesség

A szkóp ADC-je másodpercenként ennyi mintát vesz a bemeneti jelből. Nagyobb sebességnél finomabb időbeli részletesség érhető el, de csak a megfelelő analóg sávszélesség, trigger, memória és aktív csatornaszám mellett. A magas mintavételi adat nem garantálja önmagában a jó mérési minőséget.

Az MSa/s és az Mpts két külön fogalom: előbbi a minták időbeli sűrűsége, utóbbi a rögzített minták száma. A mintavétel jellemzően megoszlik a csatornák között; mindig az aktív csatornákra vonatkozó adatlapértéket kell keresni.

### Memóriamélység

A rögzítési memória határozza meg, mennyi időt tud a szkóp nagy mintavételi sebesség mellett eltárolni. Kis memória esetén gyors jelből csak rövid szakasz rögzíthető. Nagyobb memóriával hosszabb esemény vizsgálható, illetve nagyításkor több részlet marad meg.

Például 1 Mpts memória és 1 MSa/s mintavétel ideális esetben körülbelül egy másodpercnyi mintát jelent; a kijelzés és egyéb korlátok miatt ez csak közelítés. A `kB`, `Kpts`, `kbit` és `Mpts` eltérő egységek, átváltásukhoz tudni kell, hogyan tárolja az adatot a műszer.

### Analóg csatornák

Minden analóg csatorna külön mérőbemenet. Két csatornával két pont feszültségét egyszerre lehet összevetni, például egy I²C órajelet és adatot. A csatornák földcsipeszei sok hagyományos szkópnál közös potenciálon vannak; az akkumulátoros táp sem garantál galvanikusan leválasztott csatornákat.

SPI-nél az órajel, MOSI, MISO és chip select egyidejű megfigyelése négy analóg csatornát igényel, ha szkóppal akarod mindet látni. Kétcsatornás szkóppal kiválaszthatsz két vezetéket, de nem kapod meg a teljes buszállapotot.

### Trigger

A trigger határozza meg, milyen jelenségre induljon vagy álljon meg a rögzítés, például egy jel emelkedő élére. Jó triggerrel stabilan megjeleníthető ismétlődő jel, illetve egy esemény környéke. Az éltrigger alapvető méréshez elég; a ritka, összetettebb hibákhoz impulzusszélesség-, logikai vagy protokolltrigger lehet hasznos.

### Hullámforma-frissítési sebesség

A teljes hullámformák másodpercenkénti rögzítési/frissítési száma. Nem azonos az ADC mintavételi sebességével. Nagyobb frissítési sebességgel nagyobb eséllyel kap el a szkóp ritka, rövid ideig tartó hibákat, mert kevesebb idő telik el a mérések között.

### Protokolldekódolás

A dekóder a jelalakból bitfolyamot, majd protokollmezőket állít elő. I²C-hez legalább SDA és SCL kell. UART általában egy adatvezetékből dekódolható irányonként. SPI-nél a dekóder igényétől függően órajel, adat és chip select kell; a teljes MOSI/MISO forgalomhoz több csatorna szükséges. CAN differenciális fizikai rétegű; a dekódolás bemeneti és adapterigénye modellfüggő.

A táblázatban csak a gyártó által név szerint felsorolt dekódereket jelöltem. FFT vagy spektrumanalizátor nem buszdekóder: frekvenciatartalmat mutat, nem I²C/SPI/UART adatcsomagokat.

### PC-s kapcsolat és USB

Négy szintet érdemes különválasztani:

1. **USB-töltés:** csak a készüléket táplálja.
2. **Fájl- vagy képexport:** a szkópon rögzített képernyőképet/adatot később ki lehet másolni; nem élő mérés.
3. **Vezeték nélküli adatkapcsolat:** a TMP-610S-nél Bluetooth-appot ír a gyártó, de kifejezetten a multiméter adataihoz.
4. **Élő PC-s vezérlés/mérés:** a PC állíthatja és olvashatja a műszert. A kollekcióból a DPOF1204 adatlapja sorol fel USB/LAN interfészt, SCPI-t és WebControlt.

## ESP-projekthez

PWM, hang és általános analóg hibakereséshez a 10–50 MHz-es, kétcsatornás kézi modellek is használhatók, ha megfelelő a trigger és a memória. SPI/I²S több vonalának egyszerre követésére a négycsatornás DPOF1204 a táblázat egyetlen biztosan megfelelő FNIRSI-je, de lényegesen drágább. Olcsó protokollvizsgálatra gyakran jobb kiegészítő egy USB-s logikai analizátor: több digitális vonalat tud egyszerre rögzíteni és dekódolni, de analóg feszültség/zaj mérésére nem alkalmas.

**Biztonság:** a termékoldalon feltüntetett maximális feszültség nem helyettesíti a megfelelő kategóriájú mérőfejet és biztonsági besorolást. Hálózati feszültséget ezekkel a hobbiműszerekkel ne mérj; kisfeszültségű ESP-áramkörnél is ügyelj a közös földre és a túlfeszültségre.
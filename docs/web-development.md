# Webes fejlesztés ESP nélkül

## Mobilos arculati koncepciók

A [terepi terminál](web-concepts/field.html), a [Clear Sky-hangulatú állomás](web-concepts/sky.html) és a [jelmonitor](web-concepts/signal.html) külön megnyitható, telefonon is összehasonlítható statikus látványtervek. A koncepciók között a felső fülekkel válthatsz; mobilon az alsó navigáció a mintanézet részeihez ugrik. Az adatok illusztrációk, a rögzítőgombok szándékosan nem működnek; egyik oldal sem csatlakozik az ESP-hez és nem tartozik a firmware-be ágyazott oldalak közé. Az opcionális webfontok offline módban tartalék betűkészletre esnek vissza.

A következő körben a kiválasztott vizuális irányt kell a tényleges állapotokra (készenlét, rögzítés, SD-/kapcsolathiba), a fájlkezelésre és a beállításokra átvezetni; addig az aktuális webes kezelőfelület változatlan.

Második kör: a Terepi terminál szövegére és felépítésére épülő [Határállomás](web-concepts/outpost.html) a hangszintet emeli ki, a [Kék archívum](web-concepts/archive.html) a Clear Sky-jelvényt hangsúlyozza, a [Jelőr](web-concepts/watch.html) pedig grafikonra és műszeres színekre épít. A `pics/` alatti helyi képek a koncepciók részei; a meglévő három lap változatlan. Az itt látható hullámok és RMS/peak értékek nem élő adatok. Az éles verzióban az egyszerű hangszintoszlopok a meglévő forrásonkénti RMS/csúcs-mérésből frissíthetők; a pontos hullámforma külön PCM-mintafolyamot és annak terhelési felülvizsgálatát igényelné. A logók forrását és felhasználhatóságát éles beágyazás előtt ellenőrizni kell.

A [7. Jelőr + Wi-Fi](web-concepts/watch-sta.html) az AP+STA kapcsolatra mutat külön külső hálózati név/IP mintát; a 6. Jelőr az AP-only változat. Mind a hét oldalon ugyanaz a görgethető felső koncepcióválasztó van, az alsó részmenü pedig asztali és mobilnézetben is látható. A megjelenített RTC, AP és STA adatok csak illusztrációk, nem a firmware-hez kapcsolódnak.

## Helyi előnézet

VS Code: **Terminal > Run Task > RadioRecorder: Web Preview (offline)**. A `--scenario` értékével válassz állapotot:

| Állapot | Példa |
| --- | --- |
| `ready` | Működő készülék, két mintafelvétel |
| `missing-sd` | Hiányzó SD, tiltott felvétel és fájlolvasás |
| `recording` | Már folyamatban lévő felvétel |
| `wifi-failure` | Sikertelen külső Wi-Fi-csatlakozás, visszatérés AP-ba |

Parancssorból a workspace gyökerében: `python -m scripts.preview_web --scenario ready`.
Alapcím: `http://127.0.0.1:8080`. Ha foglalt, a program a következő szabad portot választja, és kiírja a tényleges címet. Leállítás: Ctrl+C vagy a VS Code task megszakítása.

A főoldal, a `/files` fájlkezelő és a `/settings` beállítások ugyanazokat a webes forrásokat használják, mint a firmware. A böngészőfül `[Offline]` előtagja jelzi az előnézetet. Fájlmódosítás után frissítsd az oldalt; a szervert nem kell újraindítani.

A főoldal felső sávja az ESP helyi dátumát és idejét mutatja `2026.11.01. 16:10:02` formában, az időforrással együtt (NTP, RTC, KÉZI, MENTETT vagy ALAP). Másodpercenként frissül, a dátum éjfélkor továbblép; nem a PC óráját vagy időzónáját használja. Ismeretlen időnél `IDŐ NINCS` jelenik meg.

A Beállítások → Dátum és idő rész az üzemidőt is mutatja `YY:MM:DD HH:MM:SS` formában, másodpercenként frissítve. A `/api/time` `uptimeSeconds` értéke a készülék aktuális bekapcsolása óta eltelt idő; az NTP/kézi óraállítás nem változtatja meg. A felbontás fix 30 napos hónapokat és 360 napos éveket használ, nem naptári dátumot. Újraindításkor nullázódik; az offline előnézet ugyanígy, a saját indulásától számolja.

A fájlkezelő előbb a mappákat, majd a fájlokat adja vissza, legfeljebb 20 elemet kérésenként; görgetéskor automatikusan lekéri a következő oldalt, hibánál az Újrapróbálás gombbal folytatható.

A `/radio` Radio Programming oldal is elérhető: megerősítés után kétmásodperces szimulált kiolvasást és két mintacsatornát mutat. Valódi UART-ot nem használ; a bináris letöltés `preview-only.bin` jelölőszöveg, nem rádiókép. A JSON szintén kizárólag mintabeállításokat tartalmaz. A készülékhez tartozó bekötést és próbalépéseket a [tesztelési útmutató](testing.md#uv-5r-konfiguráció-kiolvasása) ismerteti.

## Többnyelvűség

A katalógusbetöltő a JSON-objektumokon belüli ismétlődő kulcsokat is elutasítja, azonos értéknél is; így nem veszhet el fordítás csendes felülírással.

Az OLED és mind a hat weboldal közös, NVS-ben mentett nyelvet használ; alapértelmezés magyar (`hu`), választható angol (`en`). Az OLED **Beállítások → Nyelv** során rövid nyomás a következő nyelvre vált; a webes `/settings` oldalon legördülő választó van. Az OLED azonnal frissül, a nyitott weboldalak legfeljebb öt másodpercen belül követik a másik felületen történt váltást. Újraindítás után a mentett nyelv marad. A felhasználói fájl-/mappa-/hálózatneveket és a protokoll mezőit nem fordítjuk.

A [locales/hu.json](../locales/hu.json) és [locales/en.json](../locales/en.json) fájlok `name`, `oled` és `web` részeket tartalmaznak. Minden programforrásbeli felirat és katalóguskulcs angol; az angol katalógusban a kulcs és az érték azonos, a magyar szövegek kizárólag a magyar katalógus értékei. Például `Settings` → `Beallitasok` az OLED-en, illetve `Settings` → `Beállítások` a weben. A build nem angol forráskulcsnál hibázik. A magyar alapértelmezés és a mentett nyelvválasztás változatlan. A webes `{name}` helyőrzők és az OLED `%s`/`%u` formázási jelölői változatlanul szükségesek. Az OLED szövegek ASCII-t használnak a jelenlegi betűkészlethez, legfeljebb 25 karakterrel; a 6 képpontos főcímek legfeljebb 21 karakteresek legyenek. A webes fordítások UTF-8-asak.

Új nyelvhez másold a teljes katalógust például `locales/de.json` néven, állítsd be a megjelenített nevét, és fordítsd le ugyanazokat a kulcsokat. A nyelvlista, az OLED választás, a HTTP-validálás és a webes választó a katalógusokból származik. A nyelvkód két kisbetű (opcionális régióval, például `pt-BR`). A build hiányzó kulcsnál vagy eltérő helyőrzőnél hibázik. A PlatformIO a fordítás előtt generálja a gitignored `.pio/generated/language.generated.h` táblát és a webes oldalakba ágyazza a katalógusokat; nincs SD-/internet-/külső asset-függőség. A közös böngészős fordító a `web/i18n.js` fájlban van, dinamikus feliratokra, címekre, hozzáférhetőségi címkékre és megerősítésekre is alkalmazva. Felhasználói adatot tartalmazó DOM-elemeket `data-i18n-ignore` jelöl.

HTTP: `GET /api/settings` tartalmazza a `language` kódot; `POST /api/language?language=en` ment és frissít. Nem támogatott nyelv 400, NVS-mentési hiba 500. Az `i` soros státusz `Language: hu` vagy `Language: en` sort is ad. Offline teszt: `python -m unittest test.test_language test.test_web_preview -v`.

## Szerkeszthető fájlok

| Oldal | HTML | Stílus | Viselkedés |
| --- | --- | --- | --- |
| Főoldal | `web/index.html` | `web/index.css` | `web/index.js` |
| Fájlkezelő | `web/files.html` | `web/files.css` | `web/files.js` |
| Beállítások | `web/settings.html` | `web/settings.css` | `web/settings.js` |
| Radio Programming | `web/radio.html` | `web/settings.css` + `web/radio.css` | `web/radio.js` |

A fájlok formázott, különálló források; nem kell HTML-t C++ karakterláncban szerkeszteni. A meglévő megjelenés és működés megmaradt. Az oldalanként eltérő CSS külön maradt, így nincs véletlen stílusütközés.

## Szimulált funkciók

A `/radio` oldalon az írási séma és a módosításlista is kipróbálható. A kiolvasott mintabeállítások módosítása, a külön írási megerősítés, a revision-ellenőrzés és az eredmény a PC memóriájában szimulált; nem küld rádióparancsot. A friss Python-kódhoz az előnézeti szervert újra kell indítani. Valódi USB-s és Wi-Fi-s használat: [rádióprogramozás](radio-programming.md).

A PC-s szerver memóriában kezeli a felvétel indítását/leállítását, a dátummappás fájllistát, az időzítőket, hangerőt, erősítést, szűrőt, órát és a Wi-Fi beállításokat. Frissen az óra 2000-01-01-ről indul; a `GET /api/time`, `POST /api/time?local=YYYY-MM-DDTHH:MM` és `POST /api/time/zone?zone=0..28` API-k az időt szimulálják. A 0-s időzóna Budapest, az 1-es UTC, a 2–28-as UTC−12…UTC+14. A szimulált újraindítás a legutóbb mentett UTC-pillanattal és időzónával tesztelhető, valós flash vagy NTP nélkül. A `GET /api/files?dir=/...`, `GET /api/files/download?path=/...` ZIP-et ad; `POST /api/files/delete?path=/...&confirm=1` fájlt vagy mappát töröl. Van változó RMS/csúcs/DC jel, példa hálózatkeresés, sikeres vagy hibás csatlakozás, valamint hálózatelfelejtés. A mintafelvételek letölthető és böngészőből hallgatható szinuszhangok. Az élő hang ugyancsak szimulált PCM hangfolyam; az „ESP-n lejátszás” csak a szimulált készülék állapotát változtatja.

Sem a felvétel, sem a kézi DAC-próbahang nem nyúl valódi SD-hez vagy DAC-hoz. A jelszómezőbe csak tetszőleges próbaszöveget írj, nem szükséges valódi jelszó. A szimulátor nem kapcsolódik külső hálózatra, nem nyit COM-portot, és nem ír firmware-t. A beállítások/felvételek a szerver újraindításakor elvesznek, több böngészőablak pedig ugyanazt a memóriabeli készüléket látja.

A szimulált Wi-Fi módváltás szándékosan nem bontja le a localhost-előnézetet. Nem modellezi a valós időzítést, RF-jelet, watchdogot, NVS-megbízhatóságot, hangminőséget vagy energiafogyasztást; ezekhez hardveres teszt kell.

A Hardver teszt oldali ADC-grafikon az előnézetben csak generált 12 bites mintákat rajzol a GPIO1/2/3/9 választáshoz; nem ad visszajelzést a valódi előfeszítésről vagy rádiós hangról.

## Firmware-be ágyazás

A PlatformIO előfeldolgozó hook (`scripts/embed_web.py`) a `scripts/web_assets.py` csomagolóval összefűzi a HTML/CSS/JS fájlokat. A generált `.pio/generated/web_pages.generated.h` Git által ignorált; kézzel nem szerkesztendő. A firmware ugyanazokat a `kPage`, `kFilesPage`, `kSettingsPage` szimbólumokat szolgálja ki, teljesen beágyazva, SD és külön asset-route-ok nélkül.

A generálás önmagában is ellenőrizhető ESP és firmware-build nélkül: `python -m scripts.web_assets`.
Az offline tesztek: `python -m unittest discover -s test -p 'test_*.py' -v`. Ezek a csomagolót és localhost HTTP-kérésekkel a szimulált API-t is vizsgálják.
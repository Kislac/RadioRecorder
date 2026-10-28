# Webes fejlesztés ESP nélkül

## Helyi előnézet

VS Code: **Terminal > Run Task > RadioRecorder: Web Preview (offline)**. Indításkor válassz állapotot:

| Állapot | Példa |
| --- | --- |
| `ready` | Működő készülék, két mintafelvétel |
| `missing-sd` | Hiányzó SD, tiltott felvétel és fájlolvasás |
| `recording` | Már folyamatban lévő felvétel |
| `wifi-failure` | Sikertelen külső Wi-Fi-csatlakozás, visszatérés AP-ba |

Parancssorból a workspace gyökerében: `python -m scripts.preview_web --scenario ready`.
Alapcím: `http://127.0.0.1:8080`. Ha foglalt, a program a következő szabad portot választja, és kiírja a tényleges címet. Leállítás: Ctrl+C vagy a VS Code task megszakítása.

A főoldal, a `/files` fájlkezelő és a `/settings` beállítások ugyanazokat a webes forrásokat használják, mint a firmware. A böngészőfül `[Offline]` előtagja jelzi az előnézetet. Fájlmódosítás után frissítsd az oldalt; a szervert nem kell újraindítani.

## Szerkeszthető fájlok

| Oldal | HTML | Stílus | Viselkedés |
| --- | --- | --- | --- |
| Főoldal | `web/index.html` | `web/index.css` | `web/index.js` |
| Fájlkezelő | `web/files.html` | `web/files.css` | `web/files.js` |
| Beállítások | `web/settings.html` | `web/settings.css` | `web/settings.js` |

A fájlok formázott, különálló források; nem kell HTML-t C++ karakterláncban szerkeszteni. A meglévő megjelenés és működés megmaradt. Az oldalanként eltérő CSS külön maradt, így nincs véletlen stílusütközés.

## Szimulált funkciók

A PC-s szerver memóriában kezeli a felvétel indítását/leállítását, a fájllistát, az időzítőket, hangerőt, erősítést, szűrőt és a Wi-Fi beállításokat. Van változó RMS/csúcs/DC jel, példa hálózatkeresés, sikeres vagy hibás csatlakozás, valamint hálózatelfelejtés. A mintafelvételek letölthető és böngészőből hallgatható szinuszhangok. Az élő hang ugyancsak szimulált PCM hangfolyam; az „ESP-n lejátszás” csak a szimulált készülék állapotát változtatja.

Sem a felvétel, sem a Wi-Fi-zajteszt nem nyúl valódi SD-hez vagy hálózathoz. A jelszómezőbe csak tetszőleges próbaszöveget írj, nem szükséges valódi jelszó. A szimulátor nem kapcsolódik külső hálózatra, nem nyit COM-portot, és nem ír firmware-t. A beállítások/felvételek a szerver újraindításakor elvesznek, több böngészőablak pedig ugyanazt a memóriabeli készüléket látja.

A szimulált Wi-Fi módváltás szándékosan nem bontja le a localhost-előnézetet. Nem modellezi a valós időzítést, RF-jelet, watchdogot, NVS-megbízhatóságot, hangminőséget vagy energiafogyasztást; ezekhez hardveres teszt kell.

## Firmware-be ágyazás

A PlatformIO előfeldolgozó hook (`scripts/embed_web.py`) a `scripts/web_assets.py` csomagolóval összefűzi a HTML/CSS/JS fájlokat. A generált `.pio/generated/web_pages.generated.h` Git által ignorált; kézzel nem szerkesztendő. A firmware ugyanazokat a `kPage`, `kFilesPage`, `kSettingsPage` szimbólumokat szolgálja ki, teljesen beágyazva, SD és külön asset-route-ok nélkül.

A generálás önmagában is ellenőrizhető ESP és firmware-build nélkül: `python -m scripts.web_assets`.
Az offline tesztek: `python -m unittest discover -s test -p 'test_*.py' -v`. Ezek a csomagolót és localhost HTTP-kérésekkel a szimulált API-t is vizsgálják.
# Radio Programming USB-n és Wi-Fi-n

A **0.1.47** firmware-től ugyanaz az ESP oldali olvasó/író motor érhető el a `/radio` weboldalon és a `scripts.radio_programmer` Python-klienssel. A rádió továbbra is az ESP **GPIO17 TX -> 3,5 mm sleeve (rádió RX)** és **GPIO16 RX <- 2,5 mm ring (rádió TX)** pontjaira csatlakozik; az ellenőrzött adat-GND a 2,5 mm sleeve. A PC csak az ESP meglévő USB-soros portját használja 115200 baudon, DTR/RTS reset nélkül. Ez nem átlátszó CHIRP-programozókábel: a Python az ESP keretezett API-ját használja.

## Hardveres feltételek

A [bekötési és jelszint-ellenőrzés](testing.md#uv-5r-konfiguráció-kiolvasása) változatlanul kötelező. Az ESP nem 5 V-tűrő; kamera és külső PTT/MIC/DAC/audio ág leválasztva, a rádió saját akkumulátorról működjön. A 3,5 mm sleeve földre zárása adást indíthat. Feltöltés előtt válaszd le a rádiót, és csak külön engedéllyel tölts firmware-t. USB-kliens, soros monitor, OLED viewer és más COM-teszt nem futhat egyszerre ugyanazon a porton.

Az írás első támogatott célja a már kiolvasott **N5R2407BFB297** firmware-azonosító. Más változat továbbra is olvasható, de az írás tiltott. A program nem indít RF-adást; a frekvenciák jogszerű használatáért és a rádió hardverkorlátaiért a kezelő felel.

## PC / VS Code

Telepítés: `python -m pip install -r scripts/requirements-test.txt` (pyserial). A taskok a projekt Python-parancsát használják:

- **RadioRecorder: Radio Status (USB, read-only)**: csak az ESP állapotát kérdezi, a rádió UART-ját nem aktiválja.
- **RadioRecorder: Radio Read (USB)**: COM-port (`auto` vagy például `COM3`), majd `READ UV5R` megerősítés. Mentés a `.test-logs/radio-backup-<időbélyeg>/` könyvtárba: `radio.img`, `radio.json` és `patch.json` sablon.
- **RadioRecorder: Radio Write (USB, opt-in)**: a módosított `patch.json` elérési útja, COM-port és `WRITE UV5R` megerősítés kell. Az alapválasztás CANCEL. A kliens újraolvassa a rádiót, külön eredeti mentést készít, és csak az egyező kiinduló revision alapján ír. A sikeresen visszaellenőrzött eredményt külön `-verified` könyvtárba menti.

Parancssorból:

```powershell
python -m scripts.radio_programmer --port COM3 status
python -m scripts.radio_programmer --port COM3 schema
python -m scripts.radio_programmer --port COM3 read --confirm-read "READ UV5R"
python -m scripts.radio_programmer --port COM3 write --patch ".test-logs/radio-backup-.../patch.json" --confirm-write "WRITE UV5R"
```

A sablonban hagyd meg a kiolvasott `revision` értéket, és csak a kívánt változásokat add meg. Példa (a revision helyére a saját mentés értéke kerül):

```json
{
  "revision": "12345678",
  "changes": [
    {"group": "settings", "field": "squelch", "value": 4},
    {"group": "settings", "field": "beep", "value": 0},
    {"group": "vfoA", "field": "frequencyHz", "value": 145500000},
    {"group": "channel", "index": 3, "field": "name", "value": "LOCAL"},
    {"group": "channel", "index": 3, "field": "txHz", "value": null}
  ]
}
```

Legfeljebb 32 változás, 4096 bájt/patch. Az értékek egész számok, nem logikai JSON-értékek: kapcsolók 0/1, frekvenciák Hz-ben, VFO-offset 1000 Hz-es egységre illesztve. A `schema` listázza az engedélyezett csoportokat, mezőket és maximumokat; a firmware ezen felül a frekvenciasávot, tónuskódot, szövegkaraktereket és a meglévő csatornát is ellenőrzi. Csatorna-TX `null` értéke tiltja az adást. A `bandwidthRaw` VFO-nál 0=Wide/1=Narrow, csatornánál 0=Narrow/1=Wide. Az S-CODE nyers indexe 0–14; CTCSS például 885=88,5 Hz, DCS 1–105 normál és 106–210 fordított kódindex, 0/65535 kikapcsolt. Csatornanév legfeljebb hét támogatott nagybetűs ASCII karakter.

A kliens nem írja felül a meglévő mentéskönyvtárat, és minden futás PASS/FAIL/SKIP összesítést és időbélyeges `.test-logs/radio-programmer-*.log` naplót készít. A sablon üres változáslistájával írás nem indul. Eltérő revision esetén készíts friss mentést és vizsgáld felül a változásokat; a régi revision egyszerű lecserélése nem biztonságos felülvizsgálat.

## Web / PC nélkül

Nyisd meg az ESP `/radio` oldalát. Bekötési megerősítés után olvasd ki a rádiót, töltsd le az eredeti `.img` és JSON mentést. Az engedélyezett rádióváltozatnál megjelenik a **Módosítások** rész. Válassz csoportot, csatornát/mezőt és értéket, majd **Hozzáadás**. A listából elemek eltávolíthatók, vagy az egész lista elvethető. A módosítások nem íródnak ki automatikusan.

Az eredeti mentés letöltésének és az írás engedélyezésének külön jelölése, a bekötési jelölő és az **Írás a rádióba** megerősítése szükséges. A folyamat előzetes összehasonlítást, írást és visszaellenőrzést jelez. Új snapshot esetén a régi szerkesztési lista elvész. A web és USB nem indíthat egyszerre rádióműveletet; a PTT és a Wi-Fi zajteszt is tiltott aktív rádióművelet alatt.

## Írási védelem és korlátok

Az ESP a kiinduló és a kívánt képet RAM-ban tartja. Írás előtt teljesen újraolvassa a rádiót; bármely eltérés vagy más firmware esetén **egyetlen írási blokkot sem küld**. Csak az engedélyezett mezők változott, 16 bájtos főmemória-blokkjait írja, a többi bitet megőrzi. Ezután ismét kiolvassa és összehasonlítja az eredményt. Csak sikeres összehasonlítás után lesz `writeVerified=true`.

A kalibráció, frekvenciahatárok, azonosító, kiegészítő memória/indítási szövegek, a két kihagyandó főmemória-blokk és ismeretlen mezők nem írhatók. Ebben a változatban csak meglévő csatorna szerkeszthető, új csatorna, törlés, MEM-CH és RESET nem hajtható végre. Nyers `.img` feltöltés nincs.

Írás közben ne kapcsold ki a tápot és ne húzd ki a vezetéket. Hiányzó ACK vagy hibás visszaellenőrzés után **részleges módosítás maradhat**; nincs automatikus újrapróbálkozás vagy automatikus visszaállítás. Őrizd meg az eredeti mentést. A firmware legfeljebb 120 másodpercet enged az írási műveletre; a 10 másodperces task watchdog végig engedélyezett, a főciklus együttműködve fut.

A revision egy konzisztenciaazonosító, nem hitelesítés. A nyílt AP-n nincs felhasználói jogosultságkezelés; csak megbízható próbahálózaton használd. A webes írás azonos eredetű `application/json` kérést és külön megerősítést vár, de ez nem véd a hálózat jogosult kliensétől.

## API és ellenőrzés

HTTP: `GET /api/radio`, `GET /api/radio/schema`, `GET /api/radio/image`, `POST /api/radio/read?confirm=1`, valamint JSON patch törzzsel `POST /api/radio/write?confirm=WRITE%20UV5R`. A hibás vagy elavult patch nem indít műveletet. Az írási fázisokat a `stage`, `bytesWritten`, `writeBytes`, `writeVerified` jelzi.

USB-kérés: `@RR {"id":42,"command":"status"}` és newline; válasz: `RR_REPLY 42 <JSON>`. Parancsok: `hello`, `status`, `settings`, `schema`, `image` (`offset`, `count` legfeljebb 128), `read` (`confirm=true`), `write` (`confirm="WRITE UV5R"`, `patch` objektum). A JSON-válaszokat a Python a háttérlogok közül, azonosító szerint választja ki. Legfeljebb 4607 karakteres keret, 5 másodperces bemeneti időtúllépés; hibánál a keret maradékát newline-ig eldobja, nem hajtja végre régi soros parancsként. A korábbi OLED-keret és egybetűs parancsok változatlanok.

Offline ellenőrzés: `python -m unittest discover -s test -p 'test_*.py' -v`, majd `pio run -e esp32-s3-devkitc-1`. A natív teszt a tényleges C++ állapotgépet és mezőkódolót szimulált UART-tal futtatja, a Python-kliens tesztje darabolt COM-válaszokat használ. A localhost webes előnézetben az írás is csak szimuláció; a letölthető bináris továbbra sem rádiókép. Valódi írás, USB-COM és Wi-Fi AP teszt csak külön opt-innel és időbélyeges naplóval történjen; a normál read-only smoke tesztek nem programozzák a rádiót.
# Automatikus rádiós visszajátszás - tervezet

**Állapot:** tervezés, nem megvalósított funkció. A javaslatok jóváhagyásig nem módosítják a firmware működését, és nem jogosítanak fel rádiós adásra.

## Cél és határok

Az eszköz Wi-Fi nélkül is, előre beállított ismétlődő időközönként egy korábban lezárt SD-felvételt játszana végig a rádió hangbemenetére, kizárólag ehhez a művelethez kapcsolt PTT-vel. A kézi böngészős vagy hangszórós lejátszás **nem** indíthat adást. A webes kézi adás külön, megerősített művelet lenne; nem azonos a meglévő Fájlkezelő „Rádió DAC” gombjával.

Az első változatban csak az ismétlődő időköz, egy kiválasztott WAV és az engedélyezés szerepelne. Egy időzítési esemény egy teljes fájlt céloz. Sor, véletlen fájl/időpont, napszakos szabály és több párhuzamos adás későbbi kör. A hosszú fájlok TOT-kezelése az adás engedélyezésének feltétele, nem elfogadható egyszerűen a rádió saját TOT-jára hagyatkozni.

## Jelenlegi alapok és hiányzó láncszemek

- Van 22 050 Hz / 16 bites mono PCM WAV-olvasás és PCM5102A DAC-kimenet; a Fájlkezelő és a soros próba ezt **PTT nélkül** indítja. A `PlaybackStatus` bájtokban jelzi a haladást, van stop/szünet/seek; a DAC-ra írt bájtok nem bizonyítják a rádióban hallható vagy kisugárzott hangot.
- A GPIO10 aktív LOW reléje jelenleg külön PTT-teszt, weben és OLED-en tartósan reteszelhető. Nincs közös tulajdonlás a DAC-os lejátszással; az egy másodperces próbapulzus nem TOT-védelem. A rádió bekötése még nincs jóváhagyva.
- A felvevő, a DAC, a fájlműveletek és az automatikus jelfigyelés erőforrásokon osztoznak. A lejátszás most szünetelteti az automatikus figyelést, de nem kapcsolja ki annak élesítését. Rádióadás alatt az egyetlen rádió nem tudja garantálni a vételi forgalom rögzítését.
- Az óra RTC/NTP/kézi/telefon forrásból működhet, de a 2000-es alapidő és az áramtalanítás alatt nem járó NVS-tartalék nem megbízható valódi dátum. Az intervallumhoz nincs szükség helyi faliórára.

## Javasolt első működés

1. **Beállítás:** weben az adminisztrátor egy már lezárt, létező és támogatott PCM WAV-ot választ ki az SD-ről, valamint 1 vagy 2 órás időközt állít be; a pontos megengedett tartományt jóvá kell hagyni. A konfiguráció NVS-ben marad, az „Automatikus adás engedélyezve” kapcsoló **nem** marad aktív újraindítás után (az automatikus felvétel élesítéséhez hasonlóan). Az élesítés csak külön megerősítéssel, az adáskörnyezet ellenőrzése után történhet.
2. **Ütemezés:** élesítéskor indul az első teljes intervallum; nincs azonnali adás. A határidőt monoton eltelt idővel kell számolni, így óraállítás, NTP-szinkron vagy téli/nyári átállás nem vált ki adást. Sikeres vagy kihagyott időpont után a következő határidő az előző ütemezett határidő + intervallum; csúszáskor az elmúlt időpontokat át kell ugrani, soha nem bepótolni. Nincs két egymásra halmozott adás.
3. **Indítási kapu:** élesítés és minden ütemezett indítás előtt ellenőrzendő az SD, a kiválasztott fájl és formátuma, DAC, PTT-vezérlés biztonságos alapállapota, a felvétel/lejátszás/fájlművelet/radio-programozás foglaltsága és a kézi PTT-próba hiánya. Hiányzó, törölt, átnevezett vagy csonka fájlnál nincs helyettesítő fájl és nincs PTT; az ok látható, az élesítés leáll. Foglalt eszköznél az adott alkalom kimarad, nincs késői, meglepetésszerű adás.
4. **Vételi prioritás - javasolt biztonságos alap:** aktív kézi/automatikus felvétel mindig kizárja az adást. Amíg az automatikus rádiófigyelés élesítve van, az első változat **ne** indítson rádiós visszajátszást: nem tudja előre bizonyítani, hogy éppen nincs bejövő forgalom, és a DAC-lejátszás ma szünetelteti a figyelést. A kezelő ilyenkor külön kapcsolja ki a rádiós auto-felvételt, vagy az időpont kimarad. Ennek feloldása külön jóváhagyandó ütközési szabály és hardveres próba; adás alatt a vételi kiesés akkor is elkerülhetetlen.
5. **Adási munkamenet:** kizárólag egy új központi adásvezérlő kaphat PTT-tulajdont. Először fájl megnyitása/formátumellenőrzés és a DAC előkészítése, csak sikeres készenlét után PTT-be; rövid, kimért beállási idő után PCM-kiadás. Szünet/stop/hiba esetén azonnal némítás és PTT-elengedés; normál végén a DAC kiürítése után PTT ki. Másik kérés nem veheti át a relét egy aktív munkamenet alatt. A jelenlegi külön PTT-tesztet ezzel kölcsönösen kizáróvá kell tenni, és az automatikus adás bekapcsolásakor tiltani kell a reteszelt tesztállapotot.
6. **Leállítás:** külön webes „Adás leállítása” parancs az aktuális munkamenetet szakítja meg; külön „Automatikus adás kikapcsolása” a jövőbeli indításokat is tiltja. A helyi kezelőszerven is kell azonnali, képernyőtől független vészleállítás, amely minden szoftveresen vezérelt PTT-t elenged és letiltja az automatikát. Áramszünet, reset, watchdog, SD/DAC-hiba és szoftveres feladatleállás esetén alapállapot a **nyitott COM-NO**; ehhez a szoftver mellett igazolt hardveres előfeszítés és hibabiztos relébekötés kell. Nyílt, hitelesítetlen AP-n rádióadást indító API nem engedhető éles használatra hozzáférés-védelem nélkül.

### TOT és folytatás

Az UV-5R tényleges TOT-ja (gyakran kb. 60 s) rádió- és beállításfüggő; az adásvezérlőn külön konfigurálható, a rádióénál **kisebb**, mért maximális folyamatos PTT-idő szükséges. A határ előtt a PCM-adat folyamát meg kell állítani, a DAC-on már sorban álló minták kiürülését vagy eldobását ellenőrizni, PTT-t elengedni, a rádió szükséges újraélesítési idejét kivárni, majd új PTT-vel a következő mintától folytatni. A WAV-pozíció csak ténylegesen kiadott minták után léphet; a határnál nem lehet bájtvesztés, kettőzés vagy titkos seek. Minden szegmensnek saját maximális PTT-ideje van, és a teljes fájlra külön felső biztonsági korlát, hogy hibás fájl ne tartson fenn végtelen adást.

Az egy másodpercnél rövidebb megszakítás **cél, nem igazolt ígéret**. Az elengedés, relé, rádió TOT-visszaállás, DAC-puffer és újraindítás idejét mérni kell. Amíg a szegmentálás nem tesztelt, a megengedett egybefüggő keretnél hosszabb fájlokat el kell utasítani PTT nélkül; csendben levágni nem szabad. Az első adásképes kiadás csak akkor kész, ha a teljes fájl vagy biztonságos, folytatható szegmensekkel megy végig, vagy az elutasítást egyértelműen jelzi.

## Állapot, felület, tárolás

Javasolt állapotok: `disabled`, `waiting`, `skipped`, `preparing`, `keying`, `playing`, `rekeying`, `stopping`, `error`. A `skipped` csak az adott alkalomra vonatkozik; `error` biztonsági vagy tartós hibánál letiltja az automatikát. A kijelzett állapot különítse el a DAC-ra kiadott hangot a PTT tényleges parancsolt állapotától (a fizikai relé/radio állapotát visszacsatolás nélkül nem ismerjük).

A webes beállításban látszódjon a kiválasztott fájl, időköz, élesítettség, következő esedékesség/eltelt idő, aktuális fájl és bájthoz kötött haladás, PTT-állapot, legutóbbi indítás/kihagyás/hiba, és mindig elérhető stop. Az OLED-en a jelenlegi **Beállítások** marad az utolsó főcsempe; új főcsempe esetén elé kell beszúrni, és a PC-s kijelzőteszt sorrendjét frissíteni. Az első megvalósítás lehet webes vezérlés + helyi vészstop, külön új csempe nélkül. A tervezett API csak külön `GET` állapot, konfigurációmentés, megerősített élesítés/kézi adás és idempotens stop műveleteket kapjon; ne módosítsa a meglévő, PTT nélküli `/api/play` jelentését.

NVS-ben csak az időköz, a kiválasztott abszolút SD-útvonal és az igazolt adási korlát menthető; élesítés, aktuális határidő, PTT-állapot, hiba utáni újraengedélyezés nem. Ha átnevezés/törlés után a kijelölt útvonal nem létezik, a rendszer letilt, nem választ automatikusan másik felvételt. Időzítéshez az eszköz üzemideje kell; az érvénytelen RTC csak a felületen megjelenő faliórás időpontot érinti, az intervallumot nem. Wi-Fi-kikapcsolás nem állíthatja le a már jóváhagyott ütemezést, viszont a helyi stopnak ekkor is működnie kell.

## Megvalósítási és ellenőrzési kapuk

1. **Csak offline:** determinisztikus ütemező- és állapotgép-próbák ESP/radio nélkül: első intervallum, időátfordulás, óraugrás, lejárt időpontok kihagyása, reboot, hiányzó WAV, foglaltság, újraélesítés, stop és hiba. A fájl kiválasztását több ezer tételes könyvtár mellett se oldjuk meg teljes lista RAM-ba töltésével.
2. **Rádió nélküli integráció:** DAC + műterhelés, relé COM-NO csak mérőműszerre kötve; tesztelni a sorrendet, némítást, pontos szegmenshatárokat, resetet, feladatmegakadást, watchdogot, SD-hibát és fizikai vészstopot. A read-only smoke tesztek nem kapcsolhatnak PTT-t. Külön opt-in PC-s hardverteszt PASS/FAIL/SKIP összesítéssel és időbélyeges `.test-logs/` naplóval.
3. **Rádióval csak jóváhagyás után:** ellenőrzött audio/relé bekötés, szint, TOT és jogszerű adási környezet; oszcilloszkóppal/hallgatással igazolni a relé tényleges elengedését, az első és utolsó mintát, a szegmensváltás hosszát és a vételi kiesést. Ismert, rövid teszt-WAV-val indulni; a teszt kizárólag saját létrehozott és ellenőrzött fájlját törölheti. Firmware-változáskor fókuszált regressziós teszt, teljes offline csomag és PlatformIO build, szabad COM esetén feltöltés és teljes offline/online csomag a [tesztelési rend](testing.md) szerint.

## Jóváhagyandó döntések

1. Elég-e az első körben **egy kijelölt fájl ismétlése**, és milyen intervallumtartomány kell az 1/2 órán túl? Ha több fájl kell, milyen sorrend és ismétlési szabály legyen?
2. Elfogadható-e, hogy az automatikus rádiófelvétel élesítése alatt minden ütemezett adás kimarad, vagy szükséges a felvétel és az adás közti, kifejezetten választott prioritási mód már az első kiadásban? A saját adás alatti vételi kiesés elfogadható-e?
3. Milyen fizikai kezelőszerv legyen a vészleállítás, és kell-e külön hardveres PTT-tiltó kapcsoló? Milyen hitelesítés vagy csak helyi, védett hálózati hozzáférés fogadható el adást indító webes parancshoz?
4. Mennyi a célrádió **mért** TOT-ja és a biztonságos szegmenshossz/újrakezdési idő? Melyik jogszerű tesztkörnyezetben igazoljuk a valódi adást?

Kapcsolódó kiindulópontok: [követelmények](../requirements.md), [automatikus felvétel](automatic-recording.md), [fájlkezelés](file-management.md), [hardverbekötési feltételek](functional-hardware.md).
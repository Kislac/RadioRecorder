# Eszközök és GPIO-kiosztás

Ez a jegyzék a jelenlegi firmware ([src/main.cpp](../src/main.cpp)) lábkiosztását és a tervezett modulokat különíti el. A firmware-ben használt GPIO nem minden esetben jelent igazolt, fizikailag bekötött eszközt. A panel pontos revízióját és a kivezetéseket bekötés előtt ellenőrizni kell; a rádió most nincs az ESP-hez csatlakoztatva. **A lent tervezettként jelölt lábak bekötése nem jelenti azt, hogy a firmware már kezeli a modulokat.**

## Jelenlegi kiosztás

| Eszköz / jel | ESP32-S3 GPIO | Másik oldal, táp / állapot |
| --- | --- | --- |
| ESP32-S3 CAM panel + beépített microSD-foglalat | GPIO38 = SD CMD, GPIO39 = SD CLK, GPIO40 = SD DATA0 | A firmware 1 bites SD_MMC módot használ; nincs külön bekötendő SD-modul. |
| Rádióhang ADC-próbakör | GPIO1 = ADC1_CH0 | A helyi előfeszítő/próbaáramkör bemenete; a rádió hangkimenete **nincs ide kötve**. |
| MAX98357A I2S erősítő | GPIO42 = BCLK, GPIO41 = LRC/WS, **GPIO11 = DIN** | I2S1 TX; VIN = ellenőrzött USB 5 V, GND = ESP GND. A DIN-t a korábbi GPIO48-ról át kell kötni, a kamera leválasztása és a GPIO11 ellenőrzése után. |
| Külön 4 Ω / 3 W hangszóró | nincs GPIO | Csak a MAX98357A SPK+ és SPK- pontjai közé; egyik kivezetés sem GND. |
| ROT-1AB forgó enkóder | GPIO21 = CLK, GPIO47 = DT, GPIO14 = SW | + = 3V3, GND = ESP GND; SW a kijelző menügombja. |
| Soros monitor | GPIO43 = UART0 TX, GPIO44 = UART0 RX | Panelen lévő USB-soros kapcsolat; nem új külső modul. |
| Beépített Wi-Fi | nincs külső GPIO | ESP32-S3 saját perifériája. |
| RELC-1CH-5V-UNI teszt-PTT relé | GPIO10 = kimenet, LOW nyugalom / HIGH legfeljebb 1 másodperc | 5 V-os modultáp; a modul jumperének aktív magas állását, a 3,3 V-os vezérlőjel elfogadását és a GPIO10 tényleges bekötését a konkrét példányon ellenőrizni kell. COM/NO nincs rádióra kötve. |
| INMP441-M I2S mikrofon | GPIO6 = BCLK, GPIO7 = WS, GPIO8 = SD (I2S0 RX) | L/R = GND (bal csatorna), VDD = 3V3, közös GND. A firmware 22 050 Hz-es sztereó 32 bites I2S-keret bal csatornájából ment 16 bites mono WAV-ot és méri a webes szintet; a tényleges hangút még nincs hardveresen igazolva. |

## Tervezett modulok, firmware-támogatás nélkül

**Feltétel:** a [helyi panelképen](2026-10-24_11h20_03.png) a GPIO4–11 kamerajelként is szerepelnek. A megvett panel OV5640-es, a kép régebbi kamerakiosztást is tartalmazhat: a kamera szalagkábelét áramtalanítva le kell választani, majd a tényleges panelen kimérni a kivezetéseket és azt, hogy nincs más meghajtó vagy nem megfelelő felhúzás rajtuk. GPIO11 már a hangszóró új DIN-je; GPIO4-en egy korábbi ADC-próba eltérő feszültséget mutatott, ezt a GPIO4/5 I2C-bekötés előtt külön ellenőrizni kell. Az alábbi GPIO-k **jelöltek**, nem kimért végleges kapcsolás.

| Modul | Modul jele -> ESP32-S3 jelölt GPIO | Táp / egyéb vezeték |
| --- | --- | --- |
| [D096-12864-I2C-GV-WH OLED](https://www.hestore.hu/prod_10048304.html) | SDA -> **GPIO4**, SCL -> **GPIO5** | VCC = 3V3, GND = ESP GND; az RTC-vel közös I2C-busz. |
| [DS3231+AT24C32 RTCM](https://www.hestore.hu/prod_10038042.html) | SDA -> **GPIO4**, SCL -> **GPIO5** | VCC = 3V3, GND = ESP GND; ugyanaz a két jel szolgálja ki az RTC-t és a 32 Kbit (4 KiB) EEPROM-ot, nem kell hozzá külön adat-GPIO. SQW/32K nincs bekötve. |
| PCM5102A I2S DAC a rádió mikrofonhangjához | BCK -> **GPIO6**, LRCK/WS -> **GPIO7**, DIN -> **GPIO9** (I2S0 TX) | A megvett modul tápját, SCK/MCLK igényét és kimeneti szintjét ellenőrizni kell. A DAC analóg kimenete **nem** közvetlen rádió-MIC bekötés. |

Az OLED és RTC közös I2C-busza 3,3 V-os legyen: a modulok felhúzóellenállásai és a busz nyugalmi szintje ne vigye 3,3 V fölé az ESP GPIO-jait. Címütközést I2C-szkenneléssel kell kizárni (a DS3231 szokásos címe 0x68; az OLED és az EEPROM tényleges címét a modulon kell ellenőrizni). Az RTC elemtartójába csak a konkrét panel tartalék-elem/esetleges töltőáramkörének ellenőrzése után kerüljön megfelelő elem: a forgalmazó CR2032-t ajánl, de a termékoldal nem igazolja, hogy a panel nem próbálja tölteni a nem tölthető elemet.

Az OLED-et a firmware SH1106-ként kezeli U8g2-vel, de a konkrét modul vezérlőtípusa **még nincs hardveresen igazolva**. A firmware induláskor 0x3C és 0x3D I2C-címet vizsgál, a soros monitor jelzi a választ vagy a hibát. A kijelző hiánya esetén a régi enkóderes gyorsparancsok nem futnak. Az RTC továbbra sincs implementálva.

Az I2S1-en a MAX98357A BCLK/WS a meglévő GPIO42/41-en marad, a DIN már GPIO11 (GPIO48 szabad a firmware-ben, a panel saját RGB LED-je ettől még oda van kötve). A GPIO2 PDM-kimenetét eltávolítottuk a firmware-ből; az esetleges külső RC-szűrőt áramtalanítva le kell választani. Az I2S0 mikrofon RX GPIO6/7 órajelet és GPIO8 adatbemenetet használ. A tervezett DAC TX a közös GPIO6/7 órajelhez külön GPIO9 adatvezetéket igényelne, de **nincs implementálva**; a későbbi közös RX/TX keretidőzítést és a DAC mintavételét külön igazolni kell. A közös órajel nem jelenti az adatvezetékek összekötését; [ESP-IDF I2S leírás](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html#full-duplex).

Először a szabad GPIO-kat és a 3,3/5 V-os tápokat kell rádió nélkül igazolni. A relénél a kiválasztott vezérlési polaritással ellenőrizni kell, hogy tápfelépüléskor, resetkor és hibakor COM–NO nyitva marad; szoftveres leállítás és önálló adásidő-korlát is kell. A relé NO-COM érintkezőjét és a DAC hangkimenetét sem szabad a rádióhoz kötni, amíg a konkrét jack-kiosztás, PTT és MIC-előfeszítés nincs megmérve. A [meglévő próbábekötések](../README.md) és a [rádióinterfész-jegyzet](UV5R%20audio%20interface.md) további részleteket tartalmaznak.
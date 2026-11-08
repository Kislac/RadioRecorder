
Baofeng side:
2,5mm Audio jack (TRS)
1. Sleeve: SPK - PPT
2. Ring: TX data
3. Tip: SPK+

3,5mm Audio jack (TRS)
1. Sleeve: Mic-
2. Ring: Mic+
3. Tip: +V


Mérések, bekapcsolt rádióval:
3,5mm Audio jack (TRS) -- Elengedett PPT
1. Sleeve: Mic- <--> 2. Ring: Mic+ ==> 10kohm
1. Sleeve: Mic- <--> 3. Tip: +V ==> 17kohm
2. Ring: Mic+ <--> 3. Tip: +V ==> 13.5khom
3,5mm Audio jack (TRS) -- Megnyomott PPT
1. Sleeve: Mic- <--> 2. Ring: Mic+ ==> 3.2v
1. Sleeve: Mic- <--> 3. Tip: +V ==> 3.2v
2. Ring: Mic+ <--> 3. Tip: +V ==> 13.5khom

Mérések, bekapcsolt rádióval:
2,5mm Audio jack (TRS) -- Elengedett PPT
1. Sleeve: SPK - PPT <--> 2. Ring: TX data ==> 3.3v
1. Sleeve: SPK - PPT <--> 3. Tip: SPK+==> 11-12Mohm
2. Ring: TX data <--> 3. Tip: SPK+ ==> -2.5v (GND on Ring)
2,5mm Audio jack (TRS) -- Megnyomott PPT
1. Sleeve: SPK - PPT <--> 2. Ring: TX data ==> 1.1Mhom
1. Sleeve: SPK - PPT <--> 3. Tip: SPK+==> 7.5Mohm
2. Ring: TX data <--> 3. Tip: SPK+ ==> 10Mhom

Baofeng UV-5R ↔ Smartphone Audio Interface

Figyelem: ez a telefonos kábel bekötési jegyzete, nem az ESP32 ADC-bemenetének vagy a megszüntetett PDM-próbának a bekötése. A PDM-kimenet már nincs a firmware-ben. A 2,5 mm-es tip/sleeve pár között vételkor mért 3,5-3,6 V DC miatt az itt szereplő sleeve->GND összekötést ne másold át az ESP-re. A [jelenlegi izolált vételi bekötés](wiring/radio-receive-isolated.svg) nem helyettesíti a rádió nélküli ADC-próbát. A telefonos 470 Ω-os bekötés nem szűri az ESP digitális kimenetét, és a 3,5 mm-es MIC érintkezőinek kiosztása/előfeszítése a konkrét rádión nincs ellenőrizve.

Phone / Recorder side:
1. Sleeve: Mic
2. Ring2: Gnd
3. Ring1: R
4. Tip: L

Wiring
1. [Baofeng side] 2,5mm Tip: (SPK+)   <---> 470Ohm <---> [Recorder side] Sleeve: Mic
2. [Baofeng side] 3,5mm Ring: (Mic+)   <---> 470Ohm <---> [Recorder side] Tip: L
3. [Baofeng side] 2,5mm Sleeve: (SPK - PPT) <--->  [Recorder side] Ring2: gnd
4. [Baofeng side] 2,5mm Sleeve: (SPK - PPT) <---> PPT (switch) <---> [Baofeng side] Sleeve: Mic-
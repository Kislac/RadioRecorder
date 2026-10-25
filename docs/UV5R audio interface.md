Baofeng UV-5R ↔ Smartphone Audio Interface

Figyelem: ez a telefonos kábel bekötési jegyzete, nem az ESP32 ADC-bemenetének rajza. A 2,5 mm-es tip/sleeve pár között vételkor mért 3,5-3,6 V DC miatt az itt szereplő sleeve->GND összekötést ne másold át az ESP-re. A [jelenlegi ADC-próba](radio-receive-adc.svg) kizárólag rádió nélkül használható.



Baofeng side:
2,5mm Audio jack
1. Sleeve: SPK - PPT
2. Ring: TX data
3. Tip: SPK+

3,5mm Audio jack
1. Sleeve: Mic-
2. Ring: Mic+
3. Tip: +V

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
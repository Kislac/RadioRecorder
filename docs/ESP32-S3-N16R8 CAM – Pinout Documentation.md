# ESP32-S3-N16R8 CAM – Pinout Documentation

## 1. Overview

**Board:** ESP32-S3-WROOM-1-N16R8 CAM Development Board  
**Microcontroller:** ESP32-S3  
**Flash:** 16 MB  
**PSRAM:** 8 MB  
**Camera:** OV2640  
**Wireless:** Wi-Fi 2.4 GHz, Bluetooth LE  
**Logic voltage:** 3.3 V

This document describes the GPIO pin assignments and peripheral connections based on the supplied ESP32-S3 pinout diagram.

> **Important:** ESP32-S3 CAM boards may differ in PCB layout and peripheral wiring. Verify the actual board revision before connecting external hardware.

## 2. Power and Control Pins

| Pin | Function | Description |
|---|---|---|
| 5V | Power | 5 V power input/USB power rail, depending on board design |
| 3V3 | Power | 3.3 V power rail |
| GND | Ground | Common electrical ground |
| RST | Reset | Resets the ESP32-S3 |
| GPIO0 | Boot | Boot-mode strapping pin |

**Electrical requirements:**
- ESP32-S3 GPIOs use 3.3 V logic and are not 5 V tolerant.
- Never apply 5 V directly to a GPIO.
- Connect the grounds of the ESP32 and external peripherals.
- Use an appropriate regulated power supply.

## 3. OV2640 Camera Pin Mapping

The following mapping is taken from the supplied pinout diagram.

| Camera Signal | ESP32-S3 GPIO | Description |
|---|---:|---|
| SIOD / SDA | GPIO4 | Camera SCCB data |
| SIOC / SCL | GPIO5 | Camera SCCB clock |
| VSYNC | GPIO6 | Frame synchronization |
| HREF | GPIO7 | Horizontal line synchronization |
| Y2 / D0 | GPIO11 | Camera data bit |
| Y3 / D1 | GPIO9 | Camera data bit |
| Y4 / D2 | GPIO8 | Camera data bit |
| Y5 / D3 | GPIO10 | Camera data bit |
| Y6 / D4 | GPIO12 | Camera data bit |
| Y7 / D5 | GPIO18 | Camera data bit |
| Y8 / D6 | GPIO17 | Camera data bit |
| Y9 / D7 | GPIO16 | Camera data bit |
| PCLK | GPIO13 | Pixel clock |
| XCLK | GPIO15 | Camera input clock |
| PWDN | Not specified | Verify board wiring |
| RESET | Not specified | Verify board wiring |

**Notes:**
- The camera GPIOs should be considered reserved while the camera is in use.
- PWDN and RESET are not assigned to GPIOs in the supplied diagram.
- The camera driver configuration must match the physical wiring.

## 4. SD Card Interface

The supplied diagram identifies the following SD-related signals:

| Signal | ESP32-S3 GPIO | Description |
|---|---:|---|
| SD_CMD | GPIO38 | SD command |
| SD_CLK | GPIO39 | SD clock |
| SD_DATA | GPIO40 | SD data |

The diagram does not establish the complete SD interface mode or all possible data-line connections. Verify the board schematic before configuring SD_MMC or SPI mode.

## 5. PSRAM

The board uses an ESP32-S3-WROOM-1-N16R8 module with 8 MB of PSRAM.

| GPIO | Assignment |
|---|---|
| GPIO35 | PSRAM |
| GPIO36 | PSRAM |
| GPIO37 | PSRAM |

These pins are marked as PSRAM-related in the supplied diagram. Do not use them as general-purpose GPIOs without verifying the module and board documentation.

## 6. Built-in LEDs

| GPIO | Function | Notes |
|---|---|---|
| GPIO2 | LED ON | Shown as an LED-related signal in the diagram |
| GPIO48 | WS2812 RGB LED | Addressable RGB LED signal |

The LED polarity and active level should be verified on the actual board.

GPIO48 can be used with a WS2812-compatible library if the onboard LED is wired as shown.

## 7. USB Interface

| Signal | ESP32-S3 GPIO | Description |
|---|---:|---|
| USB D− | GPIO19 | Native USB data minus |
| USB D+ | GPIO20 | Native USB data plus |

Avoid using GPIO19 and GPIO20 for other functions when the native USB interface is required.

The board may provide USB connectors for native USB, USB-to-UART programming, or both. Verify the actual board implementation.

## 8. UART and Debugging

| Signal | ESP32-S3 GPIO | Description |
|---|---:|---|
| U0TXD | GPIO43 | UART0 transmit |
| U0RXD | GPIO44 | UART0 receive |
| GPIO42 | JTAG MTMS | JTAG debugging |
| GPIO41 | JTAG MTDI | JTAG debugging |
| GPIO40 | JTAG MTDO / SD_DATA | Multiplexed function in the supplied diagram |
| GPIO39 | JTAG MTCK / SD_CLK | Multiplexed function in the supplied diagram |

GPIO43 and GPIO44 may be used for serial logging and programming, depending on the board's USB-to-UART circuitry.

The supplied diagram shows overlapping alternate functions for some pins. Avoid assigning conflicting functions simultaneously.

## 9. Additional GPIO Reference

The following table summarizes other pins shown in the supplied diagram.

| GPIO | Alternate Function / Notes |
|---|---|
| GPIO1 | ADC1_CH0 |
| GPIO2 | ADC1_CH1, LED ON |
| GPIO3 | ADC1_CH2, JTAG EN / strapping considerations |
| GPIO4 | ADC1_CH3, camera SIOD |
| GPIO5 | ADC1_CH4, camera SIOC |
| GPIO6 | ADC1_CH5, camera VSYNC |
| GPIO7 | ADC1_CH6, camera HREF |
| GPIO8 | ADC1_CH7, camera Y4 |
| GPIO9 | ADC1_CH8, camera Y3 |
| GPIO10 | ADC1_CH9, camera Y5 |
| GPIO11 | ADC2_CH0, camera Y2 |
| GPIO12 | ADC2_CH1, camera Y6 |
| GPIO13 | ADC2_CH2, camera PCLK |
| GPIO14 | ADC2_CH3 |
| GPIO15 | ADC2_CH4, camera XCLK |
| GPIO16 | ADC2_CH5, camera Y9 |
| GPIO17 | ADC2_CH6, camera Y8 |
| GPIO18 | ADC2_CH7, camera Y7 |
| GPIO19 | Native USB D+ / ADC2_CH8 |
| GPIO20 | Native USB D− / ADC2_CH9 |
| GPIO21 | General-purpose GPIO |
| GPIO38 | SD_CMD |
| GPIO39 | SD_CLK / JTAG MTCK |
| GPIO40 | SD_DATA / JTAG MTDO |
| GPIO41 | JTAG MTDI |
| GPIO42 | JTAG MTMS |
| GPIO43 | UART0 TX |
| GPIO44 | UART0 RX |
| GPIO45 | Strapping pin / alternate function |
| GPIO46 | Boot-related strapping pin |
| GPIO47 | General-purpose GPIO |
| GPIO48 | WS2812 RGB LED |

**Important:** Alternate functions do not mean that a pin is physically available. Camera, USB, SD, PSRAM, UART, onboard LEDs, and boot configuration may reserve pins.

## 10. Boot and Strapping Pins

Take special care with the following pins:

- **GPIO0:** affects boot and download mode.
- **GPIO3:** has alternate functions and strapping considerations.
- **GPIO45 and GPIO46:** strapping-related pins.

External circuits must not force these pins to inappropriate levels during startup.

## 11. Arduino Configuration Example

The following definitions provide a starting point for an OV2640 camera configuration based on the supplied diagram.

```cpp
// OV2640 camera pin mapping
#define CAM_PIN_SIOD   4
#define CAM_PIN_SIOC   5

#define CAM_PIN_VSYNC  6
#define CAM_PIN_HREF   7
#define CAM_PIN_PCLK   13
#define CAM_PIN_XCLK   15

#define CAM_PIN_Y2     11
#define CAM_PIN_Y3      9
#define CAM_PIN_Y4      8
#define CAM_PIN_Y5     10
#define CAM_PIN_Y6     12
#define CAM_PIN_Y7     18
#define CAM_PIN_Y8     17
#define CAM_PIN_Y9     16

// SD interface: verify the actual board configuration
#define SD_CMD_GPIO    38
#define SD_CLK_GPIO    39
#define SD_DATA0_GPIO  40

// Onboard LEDs: verify actual wiring
#define STATUS_LED_GPIO  2
#define RGB_LED_GPIO    48
```

These definitions are not a complete camera initialization routine. The camera driver also requires configuration for image format, frame size, clock frequency, framebuffer allocation, and any board-specific power/reset pins.

## 12. Design Guidelines

1. **Voltage:** Use 3.3 V logic for all GPIO connections.
2. **Power:** Ensure the supply can handle the ESP32-S3, camera, and Wi-Fi current peaks.
3. **Camera:** Reserve all camera GPIOs while the camera is active.
4. **PSRAM:** Do not repurpose memory-related pins.
5. **USB:** Reserve GPIO19 and GPIO20 if native USB is used.
6. **Boot:** Avoid external circuits that interfere with strapping pins.
7. **Outputs:** Use a transistor or MOSFET driver for motors, relays, and other high-current loads.
8. **ADC:** Check ADC channel restrictions when using Wi-Fi concurrently.
9. **Conflicts:** A GPIO must not be assigned to two active peripherals at the same time.
10. **Verification:** Confirm the pin mapping against the actual PCB schematic before finalizing hardware.

## 13. References

- Supplied ESP32-S3 pinout diagram.
- [ESP32-S3-WROOM-1 / WROOM-1U Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
- [ESP32-S3 Datasheet](https://documentation.espressif.com/esp32_s3_datasheet_en.pdf)

The module datasheet describes the ESP32-S3 module, but it does not necessarily document every connection on a third-party CAM development board. The actual board schematic takes precedence.

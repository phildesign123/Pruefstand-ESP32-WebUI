# ESP32-WROVER-E — Pin-Belegung

## Übersicht

| Bus / Interface | Peripherie         | GPIOs                          |
| --------------- | ------------------ | ------------------------------ |
| LEDC PWM        | 2× MOSFET          | 25, 26                         |
| VSPI (shared)   | MAX31865 + SD-Karte | 23, 19, 18, 5, 4, 36          |
| I2C             | NAU7802 + ADS1115  | 21, 22, 39                     |
| RMT + UART      | TMC2208            | 27, 14, 13, 33, 32            |

---

## PWM-MOSFETs

| Funktion   | GPIO | Anmerkung       |
| ---------- | ---- | --------------- |
| MOSFET 1   | 25   | Hitzepille für Hotend |
| MOSFET 2   | 26   | Lüfter |

---

## MAX31865 — PT100/PT1000 (VSPI)

| Funktion | GPIO | Anmerkung                        |
| -------- | ---- | -------------------------------- |
| MOSI     | 23   | VSPI, shared mit SD-Karte        |
| MISO     | 19   | VSPI, shared mit SD-Karte        |
| SCLK     | 18   | VSPI, shared mit SD-Karte        |
| CS       | 5    | Strapping Pin, nach Boot unkritisch |
| DRDY     | 36   | Input-only, Interrupt            |

---

## SD-Karte (VSPI, shared mit MAX31865)

| Funktion | GPIO | Anmerkung                 |
| -------- | ---- | ------------------------- |
| MOSI     | 23   | Shared mit MAX31865       |
| MISO     | 19   | Shared mit MAX31865       |
| SCLK     | 18   | Shared mit MAX31865       |
| CS       | 4    | Eigener Chip-Select       |

> **Hinweis:** MAX31865 und SD-Karte teilen sich den VSPI-Bus.  
> Zugriffe müssen serialisiert werden (Mutex oder ESP-IDF `spi_bus_add_device`).

---

## NAU7802 — 24-Bit ADC / Wägezelle (I2C)

| Funktion | GPIO | Anmerkung                   |
| -------- | ---- | --------------------------- |
| SDA      | 21   | I2C Default, shared         |
| SCL      | 22   | I2C Default, shared         |
| DRDY     | 39   | Input-only, Interrupt       |

- I2C-Adresse: **0x2A** (fest)
- Abtastrate: **80 SPS** (Register CTRL2)
- DRDY-Interrupt empfohlen für jitterffreies Sampling

---

## ADS1115 — 16-Bit ADC (I2C)

| Funktion | GPIO | Anmerkung                   |
| -------- | ---- | --------------------------- |
| SDA      | 21   | Shared mit NAU7802          |
| SCL      | 22   | Shared mit NAU7802          |
| ALRT/RDY | 34   | Optional, Input-only        |

- I2C-Adresse: **0x48** (ADDR → GND, konfigurierbar)
- Kein Adresskonflikt mit NAU7802

> **I2C Pull-ups:** 4,7 kΩ nach 3,3 V auf SDA und SCL.  
> Bei Leitungslängen > 30 cm auf 2,2 kΩ reduzieren.

---

## TMC2208 — Stepper-Treiber (RMT + UART)

| Funktion | GPIO | Anmerkung                      |
| -------- | ---- | ------------------------------ |
| STEP     | 27   | RMT Channel 0                  |
| DIR      | 14   | Richtung                       |
| EN       | 13   | Enable, Active Low             |
| UART TX  | 33   | Software-UART zum TMC2208      |
| UART RX  | 32   | Software-UART vom TMC2208      |

> **Tipp:** TX und RX können über 1 kΩ Widerstand auf eine Leitung  
> zusammengelegt werden (Single-Wire UART), spart einen GPIO.

---

## Freie GPIOs

| GPIO | Typ         | Hinweis                          |
| ---- | ----------- | -------------------------------- |
| 2    | I/O         | Strapping Pin, LED auf manchen Boards |
| 12   | I/O         | Strapping — muss beim Boot LOW sein  |
| 15   | I/O         | Strapping Pin                    |
| 34   | Input-only  | Frei (oder ALRT/RDY für ADS1115) |
| 35   | Input-only  | Frei                             |

---

## Nicht verwendbare GPIOs (WROVER-E)

| GPIO    | Grund                              |
| ------- | ---------------------------------- |
| 6–11   | Intern: SPI-Flash                  |
| 16, 17  | Intern: PSRAM                      |
| 0       | Strapping: Boot-Modus              |
| 1, 3    | UART0 TX/RX (USB/Serial)          |

---

## Schematische Bus-Übersicht

```
                    ┌─────────────────────────┐
    MOSFET 1 ←──── │ GPIO 25          GPIO 21 │ ────→ SDA (I2C)
    MOSFET 2 ←──── │ GPIO 26          GPIO 22 │ ────→ SCL (I2C)
                    │                  GPIO 39 │ ────→ DRDY (NAU7802)
   MOSI (VSPI) ←── │ GPIO 23          GPIO 34 │ ────→ ALRT (ADS1115)
   MISO (VSPI) ←── │ GPIO 19                  │
   SCLK (VSPI) ←── │ GPIO 18  ESP32-WROVER-E  │
   CS MAX31865 ←── │ GPIO 5           GPIO 27 │ ────→ STEP (RMT)
   CS SD-Karte ←── │ GPIO 4           GPIO 14 │ ────→ DIR
   DRDY MAX ←───── │ GPIO 36          GPIO 13 │ ────→ EN
                    │                  GPIO 33 │ ────→ UART TX (TMC)
                    │                  GPIO 32 │ ────→ UART RX (TMC)
                    └─────────────────────────┘
```

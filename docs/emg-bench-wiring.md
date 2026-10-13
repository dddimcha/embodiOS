# M0 EMG Bench Wiring — MCP3008 + Feetech STS3215 on Raspberry Pi 4/5

embodiOS M0 prosthesis bench loop: EMG electrodes → analog front end →
MCP3008 ADC (SPI0) → 1 kHz DSP/classify → Feetech STS3215 serial-bus servo
(aux UART). Board-gated in `kernel/drivers/adc/` and
`kernel/drivers/actuator/` (WS-DRV, branch `m0-drv`).

## ⚠ POWER SAFETY — READ FIRST ⚠

**NEVER power the servo from the Pi's 5 V or 3V3 pins.** The STS3215 draws
>1 A stalled; the Pi's rail will brown out or the polyfuse/PCB trace will
cook. The servo gets its own 6 V supply (rated 6–7.4 V); only **GND is
shared** with the Pi. Double-check polarity before connecting — reversed
servo power destroys the servo and can back-feed the Pi.

Likewise the MCP3008 here runs at **3.3 V only** (Pi GPIOs are not 5 V
tolerant; on Pi 5 the RP1 pins tolerate 5 V only while RP1 is powered —
do not rely on that).

## SPI0 — MCP3008 (EMG acquisition)

| Pi header pin | Signal | BCM GPIO | MCP3008 pin (DIP-16) |
|---|---|---|---|
| 19 | SPI0 MOSI | GPIO10 | 11 (DIN) |
| 21 | SPI0 MISO | GPIO9 | 12 (DOUT) |
| 23 | SPI0 SCLK | GPIO11 | 13 (CLK) |
| 24 | SPI0 CE0 | GPIO8 | 10 (CS/SHDN) |
| 1 or 17 | 3V3 | — | 16 (VDD), 15 (VREF), 14 (AGND→GND)* |
| 6/9/14/20/25 | GND | — | 9 (DGND), 14 (AGND) |

\* AGND and DGND both to Pi GND (star point at the ADC if you have a
noisy bench supply). VREF = 3V3 → 1 LSB ≈ 3.22 mV, range 0..1023 counts.

MCP3008 pin-by-pin (notch up, pins numbered 1..8 down the left, 16..9 up
the right):

| Pin | Name | Connect to |
|---|---|---|
| 1 | CH0 | EMG channel 0 (front-end output, 0..3V3) |
| 2 | CH1 | EMG channel 1 (M0 uses 2 channels) |
| 3–8 | CH2–CH7 | spare / GND (never leave floating in a noisy rig) |
| 9 | DGND | Pi GND |
| 10 | CS/SHDN | Pi pin 24 (CE0) |
| 11 | DIN | Pi pin 19 (MOSI) |
| 12 | DOUT | Pi pin 21 (MISO) |
| 13 | CLK | Pi pin 23 (SCLK) |
| 14 | AGND | Pi GND |
| 15 | VREF | Pi 3V3 (pin 1/17) |
| 16 | VDD | Pi 3V3 (pin 1/17) |

SPI settings (driver does this): mode 0 (CPOL=0, CPHA=0), 1 MHz, CS0,
MSB first, 3-byte single-ended transaction per sample.

## Aux UART — Feetech STS3215 servo bus

Console UART0 (pins 8/10) stays free for the shell; the servo bus uses
the aux UART on pins 27/28 — same physical pins on both boards:

| Pi header pin | Signal | BCM GPIO | Pi 4 function | Pi 5 function |
|---|---|---|---|---|
| 27 | bus TX | GPIO0 | UART2 TXD2 (ALT4) | RP1 UART1 TXD1 (a2) |
| 28 | bus RX | GPIO1 | UART2 RXD2 (ALT4) | RP1 UART1 RXD1 (a2) |
| 39 (or any GND) | GND | — | — | — |

**Note:** pins 27/28 are officially the HAT ID EEPROM (ID_SD/ID_SC).
Using them for the servo bus means no HAT EEPROM on the bench rig —
that is the documented trade-off (dtoverlay=uart2 on Pi 4 /
dtoverlay=uart1-pi5 on Pi 5 make the same choice under Linux).

### Half-duplex on a 2-wire TTL bus (the 1 kΩ trick)

The STS3215 has a single DATA line (time-shared TX/RX, TTL 3.3 V
levels — do not connect RS-232 levels). Wire it like this:

```
Pi pin 27 (TXD) ──[ 1 kΩ ]──┬────────── STS3215 DATA
Pi pin 28 (RXD) ────────────┘
Pi GND ─────────────────────┴────────── STS3215 GND
```

- Pi **TX through the 1 kΩ resistor**, Pi **RX directly** on the bus.
- When the Pi transmits, TX drives the bus through the resistor (servo
  input is high-Z) — and RX hears an **echo** of every byte. The driver
  discards exactly as many RX bytes as it transmitted after each packet.
- When the servo replies, it drives the bus; the resistor prevents the
  Pi's TX (idle high) from fighting the servo, and RX reads cleanly.

Servo connector: 3 wires — DATA (yellow/white), VCC (red), GND
(black/brown). VCC goes to the **6 V bench supply**, not the Pi.

## Servo power

- Separate 6 V supply, ≥ 2 A headroom. Common GND with the Pi
  (without common ground the TTL data levels are meaningless).
- Factory baud rate is **1000000**; M0 uses **115200**. Set it once with
  the Feetech FD software / URT-1 adapter or any FTDI at 1 Mbaud: write
  register 6 (baud) then power-cycle. Servo ID must be **1**.
- Driver-set safety limits at `actuator_init()`: max torque 500/1000,
  acceleration 50, goal speed 500, goal range clamped to 100..900 counts
  (mechanical mid-range), slew limit 40 counts per 1 kHz tick.

## First power-on — expected behavior

1. Boot embodiOS on the Pi (HDMI console + UART0 shell).
2. `emg` should print hardware source active (on pi4/pi5 with
   `BOARD_HAS_EMG_HW=1` the strong overrides replace the weak synthetic
   stubs): `mcp3008: SPI0 up (CS0, mode 0, 1 MHz) on pi4|pi5`.
3. With the servo unpowered/absent, `actuator_init()` (via `m0demo`)
   prints `feetech: servo id 1 not answering on <board> aux UART` —
   that is correct; check wiring/baud/ID if the servo IS attached.
4. With the servo wired and powered at 6 V, `m0demo` prints
   `feetech: STS3215 id 1 online, torque capped at 500` and the servo
   horn holds its current position (torque on). Contracting the muscle
   (or injecting a signal at CH0) sweeps the horn between the 100..900
   count range.
5. `shutdown`/estop disables torque — the horn goes limp. The estop
   latches until the next `m0demo` run.
6. Sanity without muscles: short MCP3008 CH0 to GND → envelope ~0;
   short to 3V3 → raw counts saturate at 1023 and the DSP absurd-input
   guard trips the estop (expected — a stuck rail is treated as a fault).

## References

- Feetech SMS/STS protocol manual (doc 191218):
  https://files.waveshare.com/upload/2/27/Communication_Protocol_User_Manual-EN%28191218-0923%29.pdf
- STS3215 control table summary:
  https://www.kevsrobots.com/resources/how_it_works/bus-servos.html
- MCP3008 datasheet (Microchip DS21295).
- RP1 peripherals datasheet (RP-008370-DS-1) for Pi 5 SPI0/UART1
  register blocks and GPIO function tables.

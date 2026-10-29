# Rover-Bot wiring

ESP32-S3 module must be an **N16R8 or N8R8** (octal PSRAM) with an OV2640 camera on the
common ESP32-S3-CAM pinout (Freenove / GOOUUU style: XCLK=15, SIOD=4, SIOC=5, VSYNC=6,
HREF=7, PCLK=13, D0..D7=11,9,8,10,12,18,17,16). Camera pins are already wired on such boards.

## Pin table (all in `main/boards/rover-bot/config.h`)

| Part | Module pin | ESP32-S3 GPIO |
|---|---|---|
| INMP441 mic | SCK | 2 |
| | WS | 1 |
| | SD | 42 |
| | L/R | GND (left channel) |
| | VDD / GND | 3V3 / GND |
| MAX98357A amp | BCLK | 2 (shared with mic SCK) |
| | LRC | 1 (shared with mic WS) |
| | DIN | 41 |
| | SD | 3V3 (full-volume left channel; leave open = quieter) |
| | VIN / GND | 5V / GND, speaker on + / - |
| TFT 2.4 inch SPI (ILI9341) | VCC | 3V3 |
| | GND | GND |
| | SCK (CLK) | 21 |
| | SDI (MOSI) | 47 |
| | DC (RS) | 43 (TX0 pin) |
| | CS | **GND** (always selected) |
| | RESET | **3V3** |
| | LED (backlight) | **3V3** (through the module's own resistor) |
| | SDO (MISO), T_* touch, SD card pins | not connected |
| Touch sensor (TTP223, optional) | OUT | 45 |
| | VCC / GND | 3V3 / GND |
| L298N | IN1 | 38 |
| | IN2 | 39 |
| | IN3 | 40 |
| | IN4 | 14 |
| | ENA | 19  (**remove the ENA jumper cap first!**) |
| | ENB | 20  (**remove the ENB jumper cap first!**) |
| Servo head | signal | 48 |
| Servo left arm | signal | 46 |
| Servo right arm | signal | 3 |

## TFT screen (2.4 inch SPI, ILI9341)
The ESP32-S3 N16R8 has no spare GPIO left, so the screen is wired with only **3 signal wires** (SCK, MOSI, DC)
and the other pins are tied on the module itself (CS to GND, RESET and LED to 3V3).
* DC is on **GPIO43 (TX0)**, the serial-log pin: the firmware log is no longer printed, flashing still works.
* Want proper CS/RESET/backlight control? Free a pin (e.g. no touch sensor: `ROVER_TOUCH_GPIO -1` frees GPIO45)
  and set `DISPLAY_CS_PIN` / `DISPLAY_RST_PIN` / `DISPLAY_BL_PIN` in `config.h`.
* Module is a 5 V board with an AMS1117 regulator? Power VCC from **3V3** anyway (the logic pins are 3.3 V).
* Picture shifted, noisy or streaky -> set `DISPLAY_SPI_HZ` to `20 * 1000 * 1000`.
* Do not run long thin wires for SCK/MOSI (keep them under ~15 cm).

## Touch sensor
Any TTP223-style capacitive touch module (output HIGH when touched, LOW when idle). Mount it where a hand
can reach it (head or back). **Do not touch it while the robot powers up** (GPIO45 is a boot-strap pin).
No sensor? Set `ROVER_TOUCH_GPIO -1` in `config.h`.

## L298N checklist - if the motors do not run
1. **Pull the jumper caps off ENA and ENB.** With the caps on, 5 V from the L298N is wired to the
   ESP32 pins GPIO19/20 (it can damage the ESP32, and the motors cannot be controlled).
2. ENA -> GPIO19, ENB -> GPIO20, IN1..IN4 -> GPIO38, 39, 40, 14, battery - to L298N GND **and** ESP32 GND.
3. Motor supply to the L298N **12V** terminal must be at least **7 V** (2S Li-ion 7.4 V). The L298N loses
   about 2 V, so a single 3.7 V cell gives the motors almost nothing.
4. Motors on OUT1/OUT2 (left) and OUT3/OUT4 (right). Wrong spin direction -> swap the two wires or use
   the Setup box / voice calibration `motor_left_invert`.
5. Test from the control web page: drag a tank slider up. The L298N input LEDs (if any) should light.

## Power (most common cause of problems)

```
2S Li-ion 7.4 V ──┬── L298N 12V in  (motors get ~5.4 V after its drop: right for 6 V N20s)
                  └── 5 V buck converter (3 A) ──┬── servos (red wires)
                                                 ├── MAX98357A VIN
                                                 └── ESP32-S3 5V pin
ALL GNDs joined: battery, L298N, buck, ESP32, servos.
```
* Do **not** power servos from the ESP32 3V3 pin. Add a 470-1000 uF capacitor across the servo 5 V rail.
* Solder a 100 nF ceramic capacitor across each N20 motor's terminals (stops noise resets).
* Do not use the L298N's own 5 V output for the ESP32 + servos.

## Notes
* GPIO19/20 (native USB pins) now drive ENA/ENB, so **flash through the UART port** (the USB-serial
  connector), or hold BOOT + tap RESET before flashing on the USB-OTG port.
* Mount the camera at the front, looking slightly down, on the body (not on the head).
* Arms move in one plane (shoulder pitch). 0 = hanging down, 90 = forward, 180 = up.
  Mirror-mounted arms: right arm is inverted by default; fix by voice (see guide).

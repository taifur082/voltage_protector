# Voltage Protector

ESP32 DevKitC V4 firmware. The simulation runs in [Wokwi](https://wokwi.com). Potentiometers stand in for the mains sensors.

## Layout

```text
voltage-protector/
├── src/voltage_protector.ino          firmware
├── project_files/diagram.json         Wokwi circuit
├── project_files/libraries.txt
├── test_evidence/                     
└── readme.md
```

## Assumptions

| Item | Value |
|---|---|
| Nominal grid | 230 V |
| Over-voltage trip | 250 V, adjustable 150–290 |
| Over-current trip | 10 A, adjustable 1–25 |
| Voltage pot | GPIO34, 0–4095 counts = 0–300 V |
| Current pot | GPIO35, 0–4095 counts = 0–30 A |
| Trip confirm | 5 samples × 20 ms = 100 ms |
| Release gap | 8 V under the voltage limit, 0.5 A under the current limit |
| Reconnect | 5 s after the readings are inside that gap, adjustable 1–30 s |
| Computer link | USB serial, 115200 baud |

## Run it in Wokwi

1. Open the ESP32 template: https://wokwi.com/projects/new/esp32
2. Replace the contents of `diagram.json` with `project_files/diagram.json`.
3. Replace the contents of `sketch.ino` with `src/voltage_protector.ino`.
4. Add a file named `libraries.txt` and paste in `project_files/libraries.txt`:
   - Adafruit GFX Library
   - Adafruit SSD1306
   - Adafruit BusIO
5. Press **Save**, then **Start Simulation**.
6. Open the **Serial Monitor** at the bottom. It is already wired. Baud in the sketch is 115200.
my project link: https://wokwi.com/projects/477505593298122753

### What a healthy start looks like

The green **Appliance** LED turns on after a short boot. The OLED shows about **216V** and **3.0A**, state `NORMAL`, load `ON`. Serial prints `[NORMAL] boot` and `[RELAY] CLOSED load on`.

If the OLED instead shows about **57V** and **0.7A**, Wokwi is returning 10-bit counts. In the sketch, change `ADC_SPAN` from `4095` to `1023`, copy the sketch into both firmware files, and start again.

### Keyboard

Click a potentiometer first. Left and Right move it a little. Page Up and Page Down move it a lot.

Click the diagram background, then:

| Key | Button |
|---|---|
| M | Menu, next setting |
| U | Up, or reset when you are on the status screen |
| D | Down |

## Operate

| Goal | What to do |
|---|---|
| Trip on surge | Raise the **left** potentiometer until the OLED is above 250 V and hold it. Within about 100 ms the green LED goes off, the red LED blinks, and serial prints `OVERVOLTAGE`. |
| Clear a surge | Lower that potentiometer until the OLED is **below 242 V**, not just under 250. The 8 V gap is there so the relay does not chatter. Wait 5 s. The green LED comes back. |
| Trip on over-current | Raise the **lower** potentiometer until current is above 10 A. |
| Clear over-current | Lower it below 9.5 A, then wait 5 s. |
| Change a limit from the PC | In the serial monitor type `SET OV 240` and press Enter. `GET` prints the live values. |
| Change a limit on the device | Press M until `>` sits on OV, OC, DLY, or AUTO. Press U or D. |
| Latch until a person resets | `SET AUTO OFF`. After a trip, return the potentiometer to a safe reading and press U on the status screen, or type `RESET`. |
| Refuse a reset during a fault | Press U or type `RESET` while the reading is still past the limit. Serial prints `refused`. |

### Serial commands

```text
HELP
GET
SET OV 250
SET OC 10
SET DELAY 5
SET AUTO ON
SET AUTO OFF
RESET
```

A rejected number is ignored and the previous setting stays.

## Behaviour

- Fail-safe relay. The appliance LED is on the normally-open contact. The coil is released (GPIO26 high) to disconnect. Boot and reset start with the load off.
- The main loop has no `delay`. Sampling, buttons, serial, alarm, and the OLED all run from `millis()`.
- Over-voltage is checked before over-current when both are true.
- Auto-reconnect waits until the reading is inside the release gap, then waits the delay. A new fault cancels the wait.
- The buttons and the serial port write the same four variables. The next 20 ms sample uses the new value.

## Pins

| ESP32 | Function |
|---|---|
| GPIO34 | Voltage potentiometer signal (input only) |
| GPIO35 | Current potentiometer signal (input only) |
| GPIO26 | Relay IN. Low = load on, high = load off |
| GPIO21 / GPIO22 | OLED SDA / SCL, address 0x3C |
| GPIO18 | Fault LED, through 220 ohm |
| GPIO25 | Buzzer |
| GPIO16 / GPIO17 / GPIO4 | Menu / Up / Down, to GND, `INPUT_PULLUP` |



## Not in this build

- Wi-Fi. Serial is the computer link.
- Settings are not saved across reset.
- The potentiometers are a DC stand-in, not AC RMS.


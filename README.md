# ESP32-REED-SWITCH-DOOR-SENSOR-DEEP-SLEEP — wake an ESP32 from deep sleep on a reed switch and publish door open/close events over MQTT

IoT example by [Fluidwire](https://fluidwire.com/).

A magnetic reed switch wakes the ESP32 from deep sleep the moment a door or window moves. The board connects to WiFi, publishes one MQTT event (`open` or `closed`) with the battery voltage and a running event counter, and is back asleep within about two seconds. A timer wake sends an hourly heartbeat so your dashboard can tell "door has not moved" from "sensor is dead".

## Why this matters

A door sensor spends 99.9% of its life doing nothing. Polling the switch from a running CPU wastes the whole battery on waiting. The ESP32's EXT0 wake watches a single RTC GPIO from the always-on RTC domain while everything else — CPU, RAM, WiFi radio — is powered down. A bare ESP32-WROOM module idles at roughly **10 µA** in that state, which is months on an 18650 if the door only moves a few dozen times a day.

The event itself is the expensive part: one WiFi connect plus one MQTT publish is ~2 s at ~100 mA. That is why the sketch does the minimum on each wake, keeps timeouts short so a dead router cannot drain the cell, and keeps all its counters in RTC memory instead of flash.

This is the gate/door/window/mailbox/cabinet building block for a thesis prototype or a small home-security dashboard. Swap the reed switch for a PIR, a tilt switch or a float switch and the code does not change.

## What it does

- Sleeps with EXT0 wake armed on GPIO33, pulled up internally, so the next door movement in *either* direction wakes the chip
- Debounces the reed contact (majority of 5 reads over 50 ms) so a slammed door gives one clean event
- Publishes a JSON event to `fluidwire/demo/door/event` and a retained `open`/`closed` to `fluidwire/demo/door/state`
- Keeps `bootCount`, `eventCount` and the last reported state in RTC memory across sleeps
- Sends a heartbeat on a timer (default hourly) with the current state and battery voltage
- If a transition was missed (publish failed, or the door moved while WiFi was connecting), reports it on the next wake instead of losing it
- Reads battery voltage through a 100k/100k divider on GPIO34 using the factory-calibrated `analogReadMilliVolts()`
- Bounded timeouts: 10 s WiFi, 5 s MQTT — on failure it sleeps anyway rather than hanging on a dead network

## Required hardware

- 1x ESP32 dev board (classic ESP32-WROOM; S2/S3 also work if you pick an RTC-capable pin)
- 1x magnetic reed switch (the two-part door-sensor type from any hardware or electronics store works)
- 2x 100 kΩ resistors (battery divider)
- 1x 18650 cell or 1S LiPo + a 3.3 V LDO, or just USB while you test
- Optional: 1x 100 kΩ pull-up from GPIO33 to 3V3 for long cable runs (see Wiring)

## Required libraries

Install via **Sketch → Include Library → Manage Libraries…**

- **PubSubClient** by Nick O'Leary

`WiFi.h`, `esp_sleep` and the RTC GPIO driver ship with the ESP32 Arduino core (Boards Manager → `esp32` by Espressif Systems).

## Wiring

| Part | ESP32 |
|------|-------|
| Reed switch, leg 1 | GPIO33 |
| Reed switch, leg 2 | GND |
| Battery + | 100 kΩ → GPIO34 |
| GPIO34 | 100 kΩ → GND |
| Battery − | GND |

The sketch enables the ESP32's internal pull-up on GPIO33 and keeps it alive during deep sleep, so the two-wire reed switch is the entire input circuit on the bench. For a switch on the end of a few metres of cable, add an external **100 kΩ** from GPIO33 to 3V3 — the internal pull-up is weak (~45 kΩ) and long wires pick up enough noise to cause false wakes.

Pin rules that matter here:

- The reed pin **must be an RTC GPIO**: GPIO0, 2, 4, 12–15, 25–27, 32–39. GPIO33 is a safe default. Avoid GPIO0/2/12/15 (strapping pins) and GPIO34–39 (input-only, no pull-up).
- The battery pin **must be ADC1** (GPIO32–39) because ADC2 is unusable while WiFi is on. GPIO34 is input-only, which is exactly what you want for a sense line.

Magnet placement: the reed closes when the magnet is within ~10–15 mm. Mount the switch on the frame and the magnet on the door so they line up when closed.

## How to use

1. Copy `config.h.example` to `config.h`.
2. Edit `config.h`: WiFi credentials, MQTT broker, topics. Leave the rest at defaults for a first run.
3. Open `esp32-reed-switch-door-sensor-deep-sleep.ino` in the Arduino IDE.
4. **Tools → Board → ESP32 Dev Module**, pick the COM port, click **Upload**.
5. Open **Tools → Serial Monitor**, baud **115200**.
6. Bring the magnet to the reed switch, take it away, bring it back. Each move should produce one wake and one publish.

Watch the events from any machine with `mosquitto_sub`:

```
mosquitto_sub -h broker.hivemq.com -t "fluidwire/demo/door/#" -v
```

## Expected output

```
Boot #1, wake reason: power-on / reset
Door is CLOSED  (event: closed, events so far: 1)
Battery: 3.98 V
WiFi: connected, IP 192.168.1.137  RSSI -58
MQTT: connected
Published: {"event":"closed","door":"closed","count":1,"boot":1,"batt_v":3.98,"wake":"power-on / reset","awake_ms":2140}
Sleeping - wake on GPIO33 -> HIGH or heartbeat in 3600 s  (awake 2318 ms)

Boot #2, wake reason: ext0 (reed switch)
Door is OPEN  (event: open, events so far: 2)
Battery: 3.97 V
WiFi: connected, IP 192.168.1.137  RSSI -60
MQTT: connected
Published: {"event":"open","door":"open","count":2,"boot":2,"batt_v":3.97,"wake":"ext0 (reed switch)","awake_ms":1930}
Sleeping - wake on GPIO33 -> LOW or heartbeat in 3600 s  (awake 2102 ms)

Boot #3, wake reason: timer (heartbeat)
Door is OPEN  (event: heartbeat, events so far: 2)
Battery: 3.97 V
...
```

The retained state topic always holds the latest `open`/`closed`, so a Node-RED or Home Assistant dashboard started an hour later shows the right state immediately.

## How the wake logic works

EXT0 wakes on a *level*, not an edge: you tell the chip "wake when GPIO33 is HIGH" or "wake when it is LOW". The sketch reads the pin right before sleeping and arms the opposite level, so the next movement — open or close — fires the wake. On wake it reads the pin again (debounced) and reports whatever it finds.

Two edge cases are handled explicitly:

- **Publish failed** (no WiFi, broker down): `lastState` is not updated, so the next wake — heartbeat or door — sees the state still differs from what was last reported and sends it as a real event rather than a heartbeat.
- **Door moved during the ~2 s awake window**: the level the sketch was about to arm against is stale, so it restarts instead of sleeping. RTC memory survives a software reset, and the fresh boot reports the new state.

## Battery notes

- The ESP32 dev board's USB-serial chip and LDO draw ~3 mA even in deep sleep. For real battery life use a bare WROOM module, or a board designed for low power (FireBeetle, TinyPICO, LOLIN D32 with its "sleep-friendly" LDO).
- Feed a 1S LiPo through a low-quiescent 3.3 V LDO (HT7333, MCP1700, XC6220) into 3V3. Do not feed 4.2 V into 3V3 directly.
- The 100k/100k divider draws ~20 µA continuously at 4 V. That is comparable to the sleep current itself; use 1 MΩ/1 MΩ with a 100 nF capacitor across the bottom resistor if you want to squeeze it, and bump `BATT_SAMPLES` up.
- `HEARTBEAT_SECONDS` is the biggest knob. Hourly heartbeats cost ~24 WiFi connects a day; a door that moves 20 times a day costs 20 more. Daily heartbeats roughly halve that.

## Troubleshooting

- **Wakes immediately in a loop** — the pin is floating. Confirm the reed switch is between GPIO33 and GND and that you are on a classic ESP32 (the internal RTC pull-up is enabled by the sketch). Add the external 100 kΩ pull-up.
- **Reads inverted (OPEN when the door is closed)** — you have a normally-closed reed switch. Set `DOOR_CLOSED_LEVEL` to `HIGH` in `config.h`.
- **Never wakes on the door** — the magnet is too far from the switch, or you moved the switch to a non-RTC pin. Test the switch with a multimeter in continuity mode first.
- **Battery reads 0.00 V** — nothing on GPIO34, or the divider is wired to an ADC2 pin. It must be GPIO32–39.
- **Battery reads ~2x too high or too low** — `BATT_DIVIDER_RATIO` does not match your resistors. Ratio = (top + bottom) / bottom.
- **`WiFi: timeout` every wake** — wrong credentials, or a 5 GHz-only SSID. The ESP32 is 2.4 GHz only.
- **`MQTT: timeout, rc=-2`** — broker unreachable. Check `MQTT_HOST`/`MQTT_PORT` and that the broker allows anonymous connections or your credentials are right.
- **Serial Monitor blank after a wake** — some USB-serial chips drop the port on reset. The 200 ms delay at the top of `setup()` covers most boards; raise it to 500 ms if yours is slow.
- **Upload fails** — hold **BOOT** while the IDE says "Connecting…". If the board is asleep when you click Upload it will not respond; press **EN** first.

## Security note

This sketch sends WiFi credentials and MQTT traffic in the clear, which is fine on your own LAN with a local Mosquitto. For a public broker or anything across the internet, switch to `WiFiClientSecure` on port 8883 — see [esp32-mqtt-tls-secure](https://github.com/fluidwire/esp32-mqtt-tls-secure). Never commit a real `config.h`; it is in `.gitignore` for a reason.

## License

MIT — see [LICENSE](LICENSE).

## Got stuck?

Drop by [fluidwire.com](https://fluidwire.com/) — we ship IoT building blocks and are happy to help.

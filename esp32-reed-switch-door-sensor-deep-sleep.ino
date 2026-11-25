/*
 * ESP32 Battery Door Sensor - Reed Switch + Deep Sleep + MQTT
 * ------------------------------------------------------------
 * A magnetic reed switch on an RTC-capable GPIO wakes the ESP32 from deep
 * sleep the instant a door or window moves. The board connects to WiFi,
 * publishes ONE MQTT event ("open" or "closed") plus battery voltage and a
 * running event counter, then goes straight back to sleep - typically within
 * ~2 s of the door moving. A timer wake sends a periodic heartbeat so your
 * dashboard notices when the sensor dies or the battery runs flat.
 *
 * Why it works on a battery: in deep sleep only the RTC domain is powered,
 * and EXT0 wake watches the pin from there. A bare ESP32-WROOM module idles
 * at ~10 uA, so an 18650 lasts months when the door only moves a few dozen
 * times a day. (Most dev boards burn ~3 mA in their USB-serial chip and LDO
 * regardless - measure at the module's 3V3/GND pins for the real number.)
 *
 * Required libraries (install via Library Manager):
 *   - "PubSubClient" by Nick O'Leary
 *   WiFi, esp_sleep and the RTC GPIO driver ship with the ESP32 Arduino core.
 *
 * Board: ESP32 Dev Module (classic ESP32-WROOM). The EXT0 wake + RTC GPIO
 *        calls used here are also available on ESP32-S2/S3; just pick an
 *        RTC-capable pin for your variant.
 *
 * Wiring:
 *   Reed switch  : one leg -> GPIO33, other leg -> GND (internal pull-up used,
 *                  an external 100k to 3V3 is more robust in the field)
 *   Battery sense: BATT+ -> 100k -> GPIO34 -> 100k -> GND  (ratio 2.0)
 *   Power        : 18650 / LiPo -> 3.3 V LDO or the dev board's 5V/VIN pin
 *
 * Setup:
 *   1. Copy config.h.example to config.h
 *   2. Edit config.h: WiFi, MQTT broker, topics, heartbeat, divider ratio
 *   3. Flash. Serial Monitor at 115200. Wave a magnet past the reed switch.
 *
 * Sample MQTT message on fluidwire/demo/door/event:
 *   {"event":"open","door":"open","count":7,"boot":12,"batt_v":3.91,
 *    "wake":"ext0 (reed switch)","awake_ms":1930}
 *
 * (c) 2026 Fluidwire. Released under the MIT License.
 * https://github.com/fluidwire
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include "driver/rtc_io.h"
#include "config.h"

// --- Pins -------------------------------------------------------------------
// The reed pin MUST be an RTC GPIO for EXT0 wake. On the classic ESP32 that
// means GPIO0, 2, 4, 12-15, 25-27, 32-39. GPIO33 is a safe default: it is not
// a strapping pin and has nothing else on it on most dev boards.
const gpio_num_t REED_GPIO  = GPIO_NUM_33;
const int        REED_PIN   = 33;

// Battery divider midpoint. GPIO34 is input-only, ADC1 - exactly what we want.
// (ADC2 pins do not work while WiFi is on.)
const int        BATT_PIN   = 34;
const uint8_t    BATT_SAMPLES = 8;

// --- Timing -----------------------------------------------------------------
const uint32_t WIFI_TIMEOUT_MS  = 10000;
const uint32_t MQTT_TIMEOUT_MS  = 5000;
const uint32_t DEBOUNCE_MS      = 50;
const uint64_t US_PER_SECOND    = 1000000ULL;

// --- State that survives deep sleep -----------------------------------------
// RTC slow memory is kept during deep sleep and software resets; it is only
// cleared by a power cycle or brownout.
RTC_DATA_ATTR uint32_t bootCount  = 0;
RTC_DATA_ATTR uint32_t eventCount = 0;
RTC_DATA_ATTR uint8_t  lastState  = 0xFF;   // 0xFF = unknown (first boot)

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
uint32_t     wakeMs;

// ---------------------------------------------------------------------------
const char *wakeReasonStr(esp_sleep_wakeup_cause_t r) {
  switch (r) {
    case ESP_SLEEP_WAKEUP_EXT0:  return "ext0 (reed switch)";
    case ESP_SLEEP_WAKEUP_EXT1:  return "ext1 (multi-pin)";
    case ESP_SLEEP_WAKEUP_TIMER: return "timer (heartbeat)";
    default:                     return "power-on / reset";
  }
}

// Read the reed pin a few times over DEBOUNCE_MS and take the majority, so a
// bouncing contact or a slammed door does not give us a half-way reading.
int readDoorLevel() {
  int highs = 0;
  const int samples = 5;
  for (int i = 0; i < samples; i++) {
    if (digitalRead(REED_PIN) == HIGH) highs++;
    delay(DEBOUNCE_MS / samples);
  }
  return (highs * 2 > samples) ? HIGH : LOW;
}

bool isDoorOpen(int level) {
  // DOOR_CLOSED_LEVEL comes from config.h: LOW for a normally-open reed
  // switch (closes when the magnet is near), HIGH for a normally-closed one.
  return level != DOOR_CLOSED_LEVEL;
}

float readBatteryVolts() {
  // analogReadMilliVolts() applies the chip's factory ADC calibration, which
  // makes a big difference on the ESP32's famously non-linear ADC.
  uint32_t sum = 0;
  for (uint8_t i = 0; i < BATT_SAMPLES; i++) {
    sum += analogReadMilliVolts(BATT_PIN);
    delay(2);
  }
  float pinVolts = (sum / (float)BATT_SAMPLES) / 1000.0f;
  return pinVolts * BATT_DIVIDER_RATIO;
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);               // faster association, we are awake for ~2 s anyway
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > WIFI_TIMEOUT_MS) {
      Serial.println("WiFi: timeout");
      return false;
    }
    delay(50);
  }
  Serial.print("WiFi: connected, IP ");
  Serial.print(WiFi.localIP());
  Serial.print("  RSSI ");
  Serial.println(WiFi.RSSI());
  return true;
}

bool connectMqtt() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(MQTT_TIMEOUT_MS / 1000);
  uint32_t t0 = millis();
  while (!mqtt.connected()) {
    if (millis() - t0 > MQTT_TIMEOUT_MS) {
      Serial.print("MQTT: timeout, rc=");
      Serial.println(mqtt.state());
      return false;
    }
    bool ok;
    if (strlen(MQTT_USER) > 0) ok = mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD);
    else                       ok = mqtt.connect(MQTT_CLIENT_ID);
    if (!ok) delay(250);
  }
  Serial.println("MQTT: connected");
  return true;
}

void goToSleep(int armedLevel) {
  // Keep the internal pull-up alive in deep sleep, otherwise the pin floats
  // and the reed switch can never pull it anywhere meaningful.
  rtc_gpio_init(REED_GPIO);
  rtc_gpio_set_direction(REED_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pulldown_dis(REED_GPIO);
  rtc_gpio_pullup_en(REED_GPIO);

  // Wake when the pin goes to the OPPOSITE of its current level, i.e. on the
  // next door movement, whichever direction that is.
  int wakeLevel = (armedLevel == HIGH) ? 0 : 1;
  esp_sleep_enable_ext0_wakeup(REED_GPIO, wakeLevel);

  if (HEARTBEAT_SECONDS > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)HEARTBEAT_SECONDS * US_PER_SECOND);
  }

  Serial.print("Sleeping - wake on GPIO33 -> ");
  Serial.print(wakeLevel ? "HIGH" : "LOW");
  Serial.print(" or heartbeat in ");
  Serial.print(HEARTBEAT_SECONDS);
  Serial.print(" s  (awake ");
  Serial.print(millis() - wakeMs);
  Serial.println(" ms)");
  Serial.flush();
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------------------
void setup() {
  wakeMs = millis();
  Serial.begin(115200);
  delay(200);                          // let the USB-serial chip settle after reset

  bootCount++;
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  pinMode(REED_PIN, INPUT_PULLUP);
  analogReadResolution(12);

  int  level   = readDoorLevel();
  bool open    = isDoorOpen(level);
  bool changed = (lastState == 0xFF) || (lastState != (uint8_t)open);

  // Decide what this wake means. A pin wake is always a door event. A timer
  // wake is a heartbeat - unless the door state differs from what we last
  // reported, in which case we missed a transition and report it now.
  const char *event;
  if (cause == ESP_SLEEP_WAKEUP_EXT0 || changed) {
    event = open ? "open" : "closed";
    eventCount++;
  } else {
    event = "heartbeat";
  }

  Serial.println();
  Serial.print("Boot #");
  Serial.print(bootCount);
  Serial.print(", wake reason: ");
  Serial.println(wakeReasonStr(cause));
  Serial.print("Door is ");
  Serial.print(open ? "OPEN" : "CLOSED");
  Serial.print("  (event: ");
  Serial.print(event);
  Serial.print(", events so far: ");
  Serial.print(eventCount);
  Serial.println(")");

  float battV = readBatteryVolts();
  Serial.print("Battery: ");
  Serial.print(battV, 2);
  Serial.println(" V");

  if (connectWiFi() && connectMqtt()) {
    char payload[224];
    snprintf(payload, sizeof(payload),
             "{\"event\":\"%s\",\"door\":\"%s\",\"count\":%lu,\"boot\":%lu,"
             "\"batt_v\":%.2f,\"wake\":\"%s\",\"awake_ms\":%lu}",
             event, open ? "open" : "closed",
             (unsigned long)eventCount, (unsigned long)bootCount,
             battV, wakeReasonStr(cause), (unsigned long)(millis() - wakeMs));

    // Event topic: not retained, one message per wake.
    // State topic: retained, so a dashboard that starts later still knows
    // whether the door is open right now.
    bool ok1 = mqtt.publish(MQTT_TOPIC_EVENT, payload, false);
    bool ok2 = mqtt.publish(MQTT_TOPIC_STATE, open ? "open" : "closed", true);
    Serial.print("Published: ");
    Serial.println(payload);
    if (!ok1 || !ok2) Serial.println("MQTT: publish failed (payload too big for buffer?)");

    // Give the TCP stack a moment to actually send before we cut the radio.
    uint32_t t0 = millis();
    while (millis() - t0 < 150) { mqtt.loop(); delay(10); }
    mqtt.disconnect();
    lastState = open ? 1 : 0;        // only mark reported once it really went out
  } else {
    Serial.println("Could not publish - will retry on the next wake");
    // Leave lastState untouched so the next wake re-reports this transition.
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // If the door moved while we were busy on WiFi, the level we are about to
  // arm against is stale. Restart and let the next boot report it - RTC
  // memory (counters, lastState) survives a software reset.
  int levelNow = readDoorLevel();
  if (levelNow != level) {
    Serial.println("Door moved while we were awake - restarting to report it");
    Serial.flush();
    ESP.restart();
  }

  goToSleep(levelNow);
}

void loop() {
  // Never reached: setup() ends in esp_deep_sleep_start().
}

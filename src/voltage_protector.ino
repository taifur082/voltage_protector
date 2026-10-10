#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

const uint8_t PIN_VOLT  = 34;
const uint8_t PIN_AMP   = 35;
const uint8_t PIN_RELAY = 26;
const uint8_t PIN_FAULT = 18;
const uint8_t PIN_BUZZ  = 25;
const uint8_t PIN_MENU  = 16;
const uint8_t PIN_UP    = 17;
const uint8_t PIN_DOWN  = 4;
const uint8_t PIN_SDA   = 21;
const uint8_t PIN_SCL   = 22;

const uint8_t RELAY_CONNECT    = LOW;
const uint8_t RELAY_DISCONNECT = HIGH;


// Scales the potentiometer across the ESP32 12-bit ADC.
const float ADC_SPAN   = 4095.0f;
const float VOLT_FULL  = 300.0f;   
const float AMP_FULL   = 30.0f;    
const float HYST_VOLT  = 8.0f;     
const float HYST_AMP   = 0.5f;

const unsigned long SAMPLE_MS   = 20;
const uint8_t       TRIP_SAMPLES = 5;    
const unsigned long BOOT_MS     = 300;
const unsigned long DEBOUNCE_MS = 25;
const unsigned long OLED_MS     = 200;

// Settings (shared by the buttons and the serial port)
const float OV_MIN = 150.0f;
const float OV_MAX = 290.0f;
const float OC_MIN = 1.0f;
const float OC_MAX = 25.0f;
const unsigned long DELAY_MIN_S = 1;
const unsigned long DELAY_MAX_S = 30;

float         ovLimitV      = 250.0f;
float         ocLimitA      = 10.0f;
unsigned long reconnectMs   = 5000;
bool          autoReconnect = true;

// Runtime
enum State : uint8_t {
  ST_BOOT = 0,
  ST_NORMAL,
  ST_TRIP_OV,
  ST_TRIP_OC,
  ST_RECONNECT
};

State         state       = ST_BOOT;
bool          loadOn      = false;
bool          displayOk   = false;
float         voltage     = 0.0f;
float         current     = 0.0f;
int           rawVolt     = 0;
int           rawAmp      = 0;
uint8_t       ovCount     = 0;
uint8_t       ocCount     = 0;
uint8_t       editField   = 0;     
unsigned long sampleMs    = 0;
unsigned long bootReadyMs = 0;
unsigned long reconnectAt = 0;
unsigned long nextBeepMs  = 0;
unsigned long oledMs      = 0;
bool          oledDirty   = true;

Adafruit_SSD1306 display(128, 64, &Wire, -1);

struct Button {
  uint8_t       pin;
  bool          lastRaw;
  bool          stable;
  unsigned long changeMs;
};

Button btnMenu = { PIN_MENU, HIGH, HIGH, 0 };
Button btnUp   = { PIN_UP,   HIGH, HIGH, 0 };
Button btnDown = { PIN_DOWN, HIGH, HIGH, 0 };

const uint8_t RX_MAX = 48;
char          rxBuf[RX_MAX];
uint8_t       rxLen = 0;

// Small helpers
const char* stateName(State s) {
  switch (s) {
    case ST_BOOT:      return "BOOT";
    case ST_NORMAL:    return "NORMAL";
    case ST_TRIP_OV:   return "TRIP OV";
    case ST_TRIP_OC:   return "TRIP OC";
    case ST_RECONNECT: return "RECONN";
  }
  return "?";
}

bool readingsAreClear() {
  return voltage < (ovLimitV - HYST_VOLT) && current < (ocLimitA - HYST_AMP);
}

void setLoad(bool connected) {
  digitalWrite(PIN_RELAY, connected ? RELAY_CONNECT : RELAY_DISCONNECT);
  if (connected == loadOn) return;
  loadOn = connected;
  Serial.printf("[RELAY] %s\n",
                connected ? "CLOSED load on" : "OPEN load off");
  oledDirty = true;
}

void startAlarm() {
  digitalWrite(PIN_FAULT, HIGH);
  tone(PIN_BUZZ, 1200, 250);
  nextBeepMs = millis() + 900;
}

void stopAlarm() {
  digitalWrite(PIN_FAULT, LOW);
  noTone(PIN_BUZZ);
}

void serviceAlarm() {
  digitalWrite(PIN_FAULT, ((millis() / 200) & 1) ? HIGH : LOW);
  if ((long)(millis() - nextBeepMs) >= 0) {
    tone(PIN_BUZZ, 1000, 70);
    nextBeepMs = millis() + 900;
  }
}

void enterNormal(const char* why) {
  state = ST_NORMAL;
  stopAlarm();
  setLoad(true);
  Serial.printf("[NORMAL] %s  V=%.0f I=%.1f\n", why, voltage, current);
  oledDirty = true;
}

void enterTrip(State cause) {
  state = cause;
  setLoad(false);
  startAlarm();
  Serial.printf("[TRIP] t=%lu ms %s  V=%.0f/%.0f  I=%.1f/%.1f\n",
                millis(),
                cause == ST_TRIP_OV ? "OVERVOLTAGE" : "OVERCURRENT",
                voltage, ovLimitV, current, ocLimitA);
  oledDirty = true;
}

void tryReset(const char* source) {
  if (state == ST_BOOT) {
    Serial.printf("[RESET] %s ignored during boot\n", source);
    return;
  }
  if (state == ST_NORMAL) {
    Serial.printf("[RESET] %s ignored, already normal\n", source);
    return;
  }
  if (!readingsAreClear()) {
    Serial.printf("[RESET] %s refused, fault still present  V=%.0f I=%.1f\n",
                  source, voltage, current);
    return;
  }
  enterNormal(source);
}

void printStatus() {
  Serial.printf("[STATUS] %s  load=%s  V=%.0f limit=%.0f  I=%.1f limit=%.1f\n",
                stateName(state), loadOn ? "ON" : "OFF",
                voltage, ovLimitV, current, ocLimitA);
  Serial.printf("[STATUS] delay=%lu s  auto=%s  hyst=%.0fV/%.1fA  trip=%lu ms  rawV=%d rawI=%d\n",
                reconnectMs / 1000UL,
                autoReconnect ? "ON" : "OFF",
                HYST_VOLT, HYST_AMP,
                (unsigned long)TRIP_SAMPLES * SAMPLE_MS,
                rawVolt, rawAmp);
}

void printHelp() {
  Serial.println("[HELP] GET");
  Serial.println("[HELP] SET OV 250");
  Serial.println("[HELP] SET OC 10");
  Serial.println("[HELP] SET DELAY 5");
  Serial.println("[HELP] SET AUTO ON");
  Serial.println("[HELP] SET AUTO OFF");
  Serial.println("[HELP] RESET");
}

void applyOv(float value, const char* source) {
  value = (float)((int)(value + 0.5f));
  if (value < OV_MIN || value > OV_MAX) {
    Serial.printf("[SET] OV refused, use %.0f to %.0f\n", OV_MIN, OV_MAX);
    return;
  }
  ovLimitV = value;
  Serial.printf("[SET] %s OV %.0f V\n", source, ovLimitV);
  oledDirty = true;
}

void applyOc(float value, const char* source) {
  if (value < OC_MIN || value > OC_MAX) {
    Serial.printf("[SET] OC refused, use %.1f to %.1f\n", OC_MIN, OC_MAX);
    return;
  }
  ocLimitA = value;
  Serial.printf("[SET] %s OC %.1f A\n", source, ocLimitA);
  oledDirty = true;
}

void applyDelay(unsigned long seconds, const char* source) {
  if (seconds < DELAY_MIN_S || seconds > DELAY_MAX_S) {
    Serial.printf("[SET] DELAY refused, use %lu to %lu s\n", DELAY_MIN_S, DELAY_MAX_S);
    return;
  }
  reconnectMs = seconds * 1000UL;
  Serial.printf("[SET] %s DELAY %lu s\n", source, seconds);
  oledDirty = true;
}

void applyAuto(bool enabled, const char* source) {
  autoReconnect = enabled;
  Serial.printf("[SET] %s AUTO %s\n", source, enabled ? "ON" : "OFF");
  oledDirty = true;
}

void upperInPlace(char* s) {
  for (; *s; ++s) {
    if (*s >= 'a' && *s <= 'z') *s = *s - 'a' + 'A';
  }
}

void handleLine(char* line) {
  while (*line == ' ') ++line;
  if (*line == '\0') return;
  upperInPlace(line);

  if (strcmp(line, "HELP") == 0) { printHelp(); return; }
  if (strcmp(line, "GET") == 0)  { printStatus(); return; }
  if (strcmp(line, "RESET") == 0) { tryReset("SERIAL"); return; }

  float value = 0.0f;
  if (sscanf(line, "SET OV %f", &value) == 1) { applyOv(value, "SERIAL"); return; }
  if (sscanf(line, "SET OC %f", &value) == 1) { applyOc(value, "SERIAL"); return; }
  if (sscanf(line, "SET DELAY %f", &value) == 1) {
    applyDelay((unsigned long)(value + 0.5f), "SERIAL");
    return;
  }
  if (strcmp(line, "SET AUTO ON") == 0)  { applyAuto(true, "SERIAL"); return; }
  if (strcmp(line, "SET AUTO OFF") == 0) { applyAuto(false, "SERIAL"); return; }

  Serial.println("[ERR] unknown command, type HELP");
}

void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (rxLen > 0) {
        rxBuf[rxLen] = '\0';
        handleLine(rxBuf);
        rxLen = 0;
      }
    } else if (rxLen < RX_MAX - 1) {
      rxBuf[rxLen++] = c;
    }
  }
}

void onPress(uint8_t pin) {
  if (pin == PIN_MENU) {
    editField = (editField + 1) % 5;
    oledDirty = true;
    return;
  }

  if (editField == 0) {
    if (pin == PIN_UP) tryReset("BUTTON");
    return;
  }

  int dir = (pin == PIN_UP) ? 1 : -1;
  if (editField == 1) applyOv(ovLimitV + dir * 5.0f, "BUTTON");
  else if (editField == 2) applyOc(ocLimitA + dir * 0.5f, "BUTTON");
  else if (editField == 3) {
    long seconds = (long)(reconnectMs / 1000UL) + dir;
    if (seconds < 0) seconds = 0;
    applyDelay((unsigned long)seconds, "BUTTON");
  } else if (editField == 4 && dir != 0) {
    applyAuto(!autoReconnect, "BUTTON");
  }
}

void pollButton(Button& btn) {
  bool raw = digitalRead(btn.pin);
  if (raw != btn.lastRaw) {
    btn.lastRaw = raw;
    btn.changeMs = millis();
  }
  if ((millis() - btn.changeMs) < DEBOUNCE_MS) return;
  if (raw == btn.stable) return;
  btn.stable = raw;
  if (btn.stable == LOW) onPress(btn.pin);
}

void serviceProtection() {
  if ((millis() - sampleMs) < SAMPLE_MS) return;
  sampleMs = millis();

  rawVolt = analogRead(PIN_VOLT);
  rawAmp  = analogRead(PIN_AMP);
  if (rawVolt < 0) rawVolt = 0;
  if (rawAmp < 0) rawAmp = 0;
  voltage = (rawVolt / ADC_SPAN) * VOLT_FULL;
  current = (rawAmp / ADC_SPAN) * AMP_FULL;

  if (voltage > ovLimitV) {
    if (ovCount < 255) ovCount++;
  } else {
    ovCount = 0;
  }
  if (current > ocLimitA) {
    if (ocCount < 255) ocCount++;
  } else {
    ocCount = 0;
  }

  bool ovTrip = ovCount >= TRIP_SAMPLES;
  bool ocTrip = ocCount >= TRIP_SAMPLES;
  bool clear  = readingsAreClear();

  switch (state) {
    case ST_BOOT:
      if ((long)(millis() - bootReadyMs) < 0) break;
      if (ovTrip) enterTrip(ST_TRIP_OV);
      else if (ocTrip) enterTrip(ST_TRIP_OC);
      else enterNormal("boot");
      break;

    case ST_NORMAL:
      if (ovTrip) enterTrip(ST_TRIP_OV);
      else if (ocTrip) enterTrip(ST_TRIP_OC);
      break;

    case ST_TRIP_OV:
    case ST_TRIP_OC:
      serviceAlarm();
      if (clear && autoReconnect) {
        state = ST_RECONNECT;
        reconnectAt = millis() + reconnectMs;
        Serial.printf("[CLEAR] timer %lu s\n", reconnectMs / 1000UL);
        oledDirty = true;
      }
      break;

    case ST_RECONNECT:
      serviceAlarm();
      if (ovTrip) { enterTrip(ST_TRIP_OV); break; }
      if (ocTrip) { enterTrip(ST_TRIP_OC); break; }
      if (clear && (long)(millis() - reconnectAt) >= 0) enterNormal("timer");
      break;
  }
}

void drawScreen() {
  if (!displayOk) return;
  if (!oledDirty && (millis() - oledMs) < OLED_MS) return;
  oledMs = millis();
  oledDirty = false;

  char line[24];
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(0, 0);
  snprintf(line, sizeof line, "%.0fV %.1fA", voltage, current);
  display.print(line);

  display.setTextSize(1);
  display.setCursor(0, 20);
  if (state == ST_RECONNECT) {
    unsigned long left = 0;
    if (reconnectAt > millis()) left = (reconnectAt - millis() + 999UL) / 1000UL;
    snprintf(line, sizeof line, "RECONN %lus LOAD OFF", left);
  } else if (state == ST_TRIP_OV || state == ST_TRIP_OC) {
    snprintf(line, sizeof line, "%s LOAD OFF", stateName(state));
  } else {
    snprintf(line, sizeof line, "%s LOAD %s", stateName(state), loadOn ? "ON" : "OFF");
  }
  display.print(line);

  display.setCursor(0, 32);
  snprintf(line, sizeof line, "%cOV %.0fV", editField == 1 ? '>' : ' ', ovLimitV);
  display.print(line);
  display.setCursor(70, 32);
  snprintf(line, sizeof line, "%cOC %.1fA", editField == 2 ? '>' : ' ', ocLimitA);
  display.print(line);

  display.setCursor(0, 44);
  snprintf(line, sizeof line, "%cDLY %lus", editField == 3 ? '>' : ' ', reconnectMs / 1000UL);
  display.print(line);
  display.setCursor(70, 44);
  snprintf(line, sizeof line, "%cAUTO %s", editField == 4 ? '>' : ' ', autoReconnect ? "ON" : "OFF");
  display.print(line);

  display.setCursor(0, 56);
  if (editField == 0 && state != ST_NORMAL && state != ST_BOOT && readingsAreClear()) {
    display.print("U = reset   M = field");
  } else if (editField == 0) {
    display.print("M = field   U = reset");
  } else {
    display.print("M = field   U/D change");
  }

  display.display();
}

void setup() {
  // Disconnect the load before anything else.
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, RELAY_DISCONNECT);
  pinMode(PIN_FAULT, OUTPUT);
  digitalWrite(PIN_FAULT, LOW);
  pinMode(PIN_BUZZ, OUTPUT);

  pinMode(PIN_MENU, INPUT_PULLUP);
  pinMode(PIN_UP, INPUT_PULLUP);
  pinMode(PIN_DOWN, INPUT_PULLUP);

  analogReadResolution(12);

  Serial.begin(115200);
  Serial.println();
  Serial.println("[BOOT] ESP32 voltage protector");
  Serial.println("[BOOT] stand-in sensors only, no mains");
  Serial.printf("[BOOT] OV %.0f V  OC %.1f A  delay %lu s  auto ON\n",
                ovLimitV, ocLimitA, reconnectMs / 1000UL);
  Serial.println("[BOOT] type HELP");

  Wire.begin(PIN_SDA, PIN_SCL);
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!displayOk) {
    Serial.println("[BOOT] OLED missing, protection still runs");
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 24);
    display.println("Voltage protector");
    display.display();
  }

  bootReadyMs = millis() + BOOT_MS;
  sampleMs = millis();
}

void loop() {
  pollSerial();
  pollButton(btnMenu);
  pollButton(btnUp);
  pollButton(btnDown);
  serviceProtection();
  drawScreen();
}

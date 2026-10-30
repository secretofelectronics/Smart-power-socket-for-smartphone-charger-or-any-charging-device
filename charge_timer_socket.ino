/*
 * Charging Timer Socket - ESP32 + Active-HIGH relay + SSD1306 OLED + 3 buttons
 *
 * Wiring:
 *   OLED SDA -> GPIO21, SCL -> GPIO22, VCC -> 3.3V, GND -> GND
 *   Relay IN -> GPIO26 (HIGH = ON)
 *   UP -> GPIO25, DOWN -> GPIO33, OK -> GPIO32 (each button to GND)
 *
 * Libraries: Adafruit SSD1306, Adafruit GFX
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// ---------- Pins ----------
const uint8_t PIN_RELAY = 26;
const uint8_t PIN_UP    = 25;
const uint8_t PIN_DOWN  = 33;
const uint8_t PIN_OK    = 32;

// ---------- Display ----------
#define SCREEN_W 128
#define SCREEN_H 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ---------- Settings ----------
const uint32_t MIN_MINUTES   = 5;
const uint32_t MAX_MINUTES   = 720;      // 12 hours
const uint32_t STEP_SHORT    = 5;        // minutes per press
const uint32_t STEP_LONG     = 15;       // minutes per repeat while holding
const uint32_t CANCEL_HOLD   = 1000;     // ms to hold OK to cancel
const uint32_t SLEEP_SET_MS  = 120000;   // screen off after idle in set mode
const uint32_t SLEEP_RUN_MS  = 20000;    // screen off after idle while running
const uint32_t SLEEP_DONE_MS = 10000;    // screen off after finishing

Preferences prefs;

// ---------- State ----------
enum State { SETTING, RUNNING, FINISHED };
State state = SETTING;

uint32_t setMinutes = 60;
uint32_t durationMs = 0;
uint32_t startMs    = 0;
uint32_t finishedAt = 0;
uint32_t lastActivity = 0;
bool     screenOn = true;
bool     okCancelHandled = false;

// ---------- Button handling ----------
struct Button {
  uint8_t  pin;
  bool     down = false;
  bool     lastRaw = false;
  uint32_t lastChange = 0;
  uint32_t pressedAt = 0;
  uint32_t lastRepeat = 0;
  bool     pressEvent = false;   // fired once on press
  bool     repeatEvent = false;  // fired repeatedly while held
};

Button bUp   {PIN_UP};
Button bDown {PIN_DOWN};
Button bOk   {PIN_OK};

void updateButton(Button &b) {
  b.pressEvent = false;
  b.repeatEvent = false;
  bool raw = (digitalRead(b.pin) == LOW);
  uint32_t now = millis();
  if (raw != b.lastRaw) { b.lastRaw = raw; b.lastChange = now; }
  if (now - b.lastChange > 25 && raw != b.down) {       // debounced change
    b.down = raw;
    if (b.down) { b.pressEvent = true; b.pressedAt = now; b.lastRepeat = now; }
  }
  if (b.down && now - b.pressedAt > 500 && now - b.lastRepeat > 250) {
    b.repeatEvent = true;
    b.lastRepeat = now;
  }
}

// ---------- Relay ----------
void relayOn()  { digitalWrite(PIN_RELAY, HIGH); }
void relayOff() { digitalWrite(PIN_RELAY, LOW);  }

// ---------- Screen helpers ----------
void screenWake() {
  if (!screenOn) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    screenOn = true;
  }
}
void screenSleep() {
  if (screenOn) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    screenOn = false;
  }
}

void drawCentered(const char *txt, int y, uint8_t size) {
  int16_t x1, y1; uint16_t w, h;
  display.setTextSize(size);
  display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_W - w) / 2, y);
  display.print(txt);
}

// small plug icon
void drawPlug(int x, int y) {
  display.drawRoundRect(x, y + 3, 10, 8, 2, SSD1306_WHITE);
  display.drawLine(x + 3, y, x + 3, y + 3, SSD1306_WHITE);
  display.drawLine(x + 6, y, x + 6, y + 3, SSD1306_WHITE);
  display.drawLine(x + 5, y + 11, x + 5, y + 14, SSD1306_WHITE);
}

void drawHeader(const char *label, bool on) {
  drawPlug(2, 1);
  display.setTextSize(1);
  display.setCursor(18, 4);
  display.print(label);
  // status pill on the right
  display.drawRoundRect(94, 2, 32, 12, 6, SSD1306_WHITE);
  if (on) {
    display.fillRoundRect(94, 2, 32, 12, 6, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(103, 5);
    display.print("ON");
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.setCursor(100, 5);
    display.print("OFF");
  }
  display.drawFastHLine(0, 17, SCREEN_W, SSD1306_WHITE);
}

// ---------- Screens ----------
void drawSetting() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  drawHeader("SET TIMER", false);

  char buf[8];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(setMinutes / 60), (unsigned)(setMinutes % 60));
  drawCentered(buf, 24, 3);

  display.setTextSize(1);
  display.setCursor(26, 52);  display.print("hours");
  display.setCursor(76, 52);  display.print("min");

  // hint arrows
  display.fillTriangle(4, 34, 12, 34, 8, 28, SSD1306_WHITE);
  display.fillTriangle(4, 42, 12, 42, 8, 48, SSD1306_WHITE);
  display.display();
}

void drawRunning(uint32_t remainingMs) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  drawHeader("CHARGING", true);

  uint32_t secs = (remainingMs + 999) / 1000;
  char buf[12];
  bool colon = (millis() / 500) % 2 == 0;
  snprintf(buf, sizeof(buf), colon ? "%02u:%02u:%02u" : "%02u %02u %02u",
           (unsigned)(secs / 3600), (unsigned)((secs / 60) % 60), (unsigned)(secs % 60));
  drawCentered(buf, 22, 2);

  // progress bar (fills as time elapses)
  float done = 1.0f - (float)remainingMs / (float)durationMs;
  if (done < 0) done = 0; if (done > 1) done = 1;
  display.drawRoundRect(4, 44, 120, 8, 3, SSD1306_WHITE);
  int fillW = (int)(done * 116);
  if (fillW > 0) display.fillRoundRect(6, 46, fillW, 4, 1, SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(4, 55);
  display.print("+/- adjust");
  display.setCursor(74, 55);
  display.print("hold OK=stop");
  display.display();
}

void drawFinished() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  drawHeader("DONE", false);

  // check mark
  display.drawLine(52, 34, 60, 42, SSD1306_WHITE);
  display.drawLine(53, 34, 61, 42, SSD1306_WHITE);
  display.drawLine(60, 42, 76, 24, SSD1306_WHITE);
  display.drawLine(61, 42, 77, 24, SSD1306_WHITE);

  drawCentered("Power cut. Sleep well", 50, 1);
  display.display();
}

// ---------- Actions ----------
void startTimer() {
  prefs.putUInt("mins", setMinutes);
  durationMs = setMinutes * 60000UL;
  startMs = millis();
  relayOn();
  state = RUNNING;
}

void stopTimer(bool finished) {
  relayOff();
  if (finished) { state = FINISHED; finishedAt = millis(); }
  else          { state = SETTING; }
}

// ---------- Arduino ----------
void setup() {
  pinMode(PIN_RELAY, OUTPUT);
  relayOff();                                   // always start OFF

  pinMode(PIN_UP,   INPUT_PULLUP);
  pinMode(PIN_DOWN, INPUT_PULLUP);
  pinMode(PIN_OK,   INPUT_PULLUP);

  Wire.begin(21, 22);
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);

  prefs.begin("chargetimer", false);
  setMinutes = prefs.getUInt("mins", 60);
  if (setMinutes < MIN_MINUTES) setMinutes = MIN_MINUTES;
  if (setMinutes > MAX_MINUTES) setMinutes = MAX_MINUTES;

  // splash
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  drawCentered("CHARGE", 12, 2);
  drawCentered("TIMER", 32, 2);
  display.display();
  delay(1200);

  lastActivity = millis();
}

void loop() {
  uint32_t now = millis();
  updateButton(bUp);
  updateButton(bDown);
  updateButton(bOk);

  bool anyPress = bUp.pressEvent || bDown.pressEvent || bOk.pressEvent;
  bool wasOff = !screenOn;

  if (anyPress || bUp.repeatEvent || bDown.repeatEvent) {
    lastActivity = now;
    screenWake();
  }

  // First press only wakes the screen (prevents accidental actions in the dark)
  bool swallow = wasOff && anyPress;

  if (!swallow) {
    switch (state) {

      case SETTING:
        if (bUp.pressEvent || bUp.repeatEvent) {
          uint32_t step = bUp.repeatEvent ? STEP_LONG : STEP_SHORT;
          setMinutes = min(MAX_MINUTES, setMinutes + step);
        }
        if (bDown.pressEvent || bDown.repeatEvent) {
          uint32_t step = bDown.repeatEvent ? STEP_LONG : STEP_SHORT;
          setMinutes = (setMinutes > MIN_MINUTES + step) ? setMinutes - step : MIN_MINUTES;
        }
        if (bOk.pressEvent) startTimer();
        break;

      case RUNNING: {
        uint32_t elapsed = now - startMs;
        // extend / reduce running time
        if (bUp.pressEvent) {
          if (durationMs + STEP_SHORT * 60000UL <= MAX_MINUTES * 60000UL)
            durationMs += STEP_SHORT * 60000UL;
        }
        if (bDown.pressEvent) {
          if (durationMs > elapsed + (STEP_SHORT + 1) * 60000UL)
            durationMs -= STEP_SHORT * 60000UL;
        }
        // hold OK to cancel
        if (bOk.pressEvent) okCancelHandled = false;
        if (bOk.down && !okCancelHandled && now - bOk.pressedAt >= CANCEL_HOLD) {
          okCancelHandled = true;
          stopTimer(false);
          lastActivity = now;
        }
        break;
      }

      case FINISHED:
        if (anyPress) { state = SETTING; lastActivity = now; }
        break;
    }
  }

  // timer completion
  if (state == RUNNING && (now - startMs) >= durationMs) {
    stopTimer(true);
    screenWake();
    lastActivity = now;
  }

  // auto screen off
  uint32_t timeout = (state == SETTING) ? SLEEP_SET_MS :
                     (state == RUNNING) ? SLEEP_RUN_MS : SLEEP_DONE_MS;
  if (now - lastActivity > timeout) screenSleep();

  // draw
  static uint32_t lastDraw = 0;
  if (screenOn && (now - lastDraw > 100 || anyPress)) {
    lastDraw = now;
    if (state == SETTING)       drawSetting();
    else if (state == RUNNING)  drawRunning(durationMs - (now - startMs));
    else                        drawFinished();
  }
}

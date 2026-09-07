// Author: James Balolong
// Version 1.0
// Smart Peg Board – ESP32 (NodeMCU-32S) scaffold
// Sensor strategy: 2× CD74HC4067 (32 channels) + 4 direct GPIO (holes 33–36)

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Compile-time sizes
// ---------------------------------------------------------------------------
static const uint8_t GRID_SIZE = 6;
static const uint8_t HOLE_COUNT = 36;
static const uint8_t LEVEL_COUNT = 10;
static const uint8_t MUX_CHANNEL_COUNT = 16;

// ---------------------------------------------------------------------------
// GPIO pin map
// Tentative mapping from pegboard_with_2mux.md — validate against the schematic.
// Mux EN pins are defined for software control (active LOW). Tie them to GND
// on the board if those GPIOs are needed for LEDs/buzzer instead.
// ---------------------------------------------------------------------------

// --- SPI TFT (ILI9341) ---
const int PIN_TFT_SCK = 18;   // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_TFT_MOSI = 23;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_TFT_DC = 2;     // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_TFT_CS = 5;     // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_TFT_RST = 4;    // CHANGE GPIO HERE if pin assignments differ on hardware

// --- Mux #1 CD74HC4067 (holes 1–16, indices 0–15) ---
const int PIN_MUX1_S0 = 13;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_MUX1_S1 = 12;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_MUX1_S2 = 14;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_MUX1_S3 = 27;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_MUX1_SIG = 34; // CHANGE GPIO HERE if pin assignments differ on hardware (input-only OK)
const int PIN_MUX1_EN = 15;  // CHANGE GPIO HERE if pin assignments differ on hardware (active LOW)
                             // Default: tie EN to GND on the PCB and set MUX_EN_TIED_TO_GND below.

// --- Mux #2 CD74HC4067 (holes 17–32, indices 16–31) ---
const int PIN_MUX2_S0 = 26;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_MUX2_S1 = 25;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_MUX2_S2 = 33;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_MUX2_S3 = 32;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_MUX2_SIG = 35; // CHANGE GPIO HERE if pin assignments differ on hardware (input-only OK)
const int PIN_MUX2_EN = 16;  // CHANGE GPIO HERE if pin assignments differ on hardware (active LOW)
                             // Default: tie EN to GND on the PCB and set MUX_EN_TIED_TO_GND below.

// Both muxes have their own SIG pin, so they may stay enabled together.
// When true, EN is assumed wired to GND; GPIO 15/16 are used for LEDs instead.
const bool MUX_EN_TIED_TO_GND = true;

// --- Direct GPIO sensors (holes 33–36, indices 32–35) ---
const int PIN_SENSOR_33 = 21;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_SENSOR_34 = 22;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_SENSOR_35 = 19;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_SENSOR_36 = 17;  // CHANGE GPIO HERE if pin assignments differ on hardware

const int DIRECT_SENSOR_PINS[4] = {
    PIN_SENSOR_33,
    PIN_SENSOR_34,
    PIN_SENSOR_35,
    PIN_SENSOR_36};

// --- Buttons (active LOW, internal pull-up where supported) ---
const int PIN_BTN_GREEN = 0;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_BTN_RED = 36;   // CHANGE GPIO HERE if pin assignments differ on hardware (input-only; no pull-up)

// --- Status LEDs (available when mux EN is tied to GND) ---
const int PIN_LED_GREEN = 15;  // CHANGE GPIO HERE if pin assignments differ on hardware
const int PIN_LED_RED = 16;    // CHANGE GPIO HERE if pin assignments differ on hardware

// --- Buzzer ---
const int PIN_BUZZER = 1;  // CHANGE GPIO HERE if pin assignments differ on hardware
                           // Placeholder only: GPIO 1 is UART0 TX. Remap once a free pin exists on the PCB.

// ---------------------------------------------------------------------------
// Timing / debounce
// ---------------------------------------------------------------------------
static const uint32_t SENSOR_DEBOUNCE_MS = 20;
static const uint32_t BUTTON_SHORT_MAX_MS = 3000;
static const uint32_t BUTTON_LONG_MS = 5000;
static const uint32_t MUX_SETTLE_US = 5;

// Set true if the LM393 module drives LOW when a peg is present.
static const bool SENSOR_ACTIVE_LOW = false;

// ---------------------------------------------------------------------------
// Game state machine
// ---------------------------------------------------------------------------
enum GameState {
  IDLE,
  LEVEL_ACTIVE,
  LEVEL_COMPLETE,
  GAME_COMPLETE
};

const char *gameStateName(GameState state) {
  switch (state) {
    case IDLE:           return "IDLE";
    case LEVEL_ACTIVE:   return "LEVEL_ACTIVE";
    case LEVEL_COMPLETE: return "LEVEL_COMPLETE";
    case GAME_COMPLETE:  return "GAME_COMPLETE";
    default:             return "UNKNOWN";
  }
}

volatile GameState currentState = IDLE;
uint8_t currentLevel = 0;  // 0–9

bool currentPegState[GRID_SIZE][GRID_SIZE] = {};
bool lastStablePegState[GRID_SIZE][GRID_SIZE] = {};
bool debounceCandidate[GRID_SIZE][GRID_SIZE] = {};
uint32_t debounceStartMs[GRID_SIZE][GRID_SIZE] = {};

uint32_t levelStartMs = 0;
uint32_t elapsedTimeMs[LEVEL_COUNT] = {};
uint16_t errorCount[LEVEL_COUNT] = {};

// Button ISR timestamps (press = falling edge, release handled in loop)
volatile uint32_t greenPressMs = 0;
volatile uint32_t redPressMs = 0;
volatile bool greenIrqPending = false;
volatile bool redIrqPending = false;
bool greenHeld = false;
bool redHeld = false;
uint32_t greenDownMs = 0;
uint32_t redDownMs = 0;

// ---------------------------------------------------------------------------
// Level patterns: 1 = peg required, 0 = empty
// Stored in flash (PROGMEM). Edit placeholders for levels 3–10 as needed.
// ---------------------------------------------------------------------------
const uint8_t levelPatterns[10][6][6] PROGMEM = {
    // Level 1
    {
        {1, 1, 1, 1, 1, 1},
        {0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0}},
    // Level 2
    {
        {1, 1, 1, 1, 1, 1},
        {1, 0, 0, 0, 0, 1},
        {1, 0, 0, 0, 0, 1},
        {1, 0, 0, 0, 0, 1},
        {1, 0, 0, 0, 0, 1},
        {1, 1, 1, 1, 1, 1}},
    // Level 3 – Placeholder (copy and edit)
    {
        {1, 1, 1, 1, 0, 0},
        {1, 1, 1, 1, 0, 0},
        {1, 1, 1, 1, 0, 0},
        {1, 1, 1, 1, 0, 0},
        {1, 1, 1, 1, 0, 0},
        {1, 1, 1, 1, 0, 0}},
    // Level 4 – Placeholder (copy and edit)
    {
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1}},
    // Level 5 – Placeholder (copy and edit)
    {
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1}},
    // Level 6 – Placeholder (copy and edit)
    {
        {0, 0, 1, 1, 0, 0},
        {0, 0, 1, 1, 0, 0},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {0, 0, 1, 1, 0, 0},
        {0, 0, 1, 1, 0, 0}},
    // Level 7 – Placeholder (copy and edit)
    {
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1}},
    // Level 8 – Placeholder (copy and edit)
    {
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1}},
    // Level 9 – Placeholder (copy and edit)
    {
        {0, 0, 1, 1, 0, 0},
        {0, 1, 0, 0, 1, 0},
        {1, 1, 1, 1, 1, 1},
        {0, 1, 1, 1, 1, 0},
        {0, 1, 1, 1, 1, 0},
        {0, 1, 1, 1, 1, 0}},
    // Level 10 – Placeholder (copy and edit)
    {
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1}}};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void setMuxChannel(int s0, int s1, int s2, int s3, uint8_t channel);
void setMuxEnabled(int enPin, bool enabled);
bool readSensor(uint8_t index);
void scanSensors();
void handleSensorChange(uint8_t row, uint8_t col, bool inserted);
bool patternMatchesCurrentLevel();
uint8_t targetAt(uint8_t level, uint8_t row, uint8_t col);
void enterState(GameState next);
void handleStateMachine();
void updateLeds();
void displayUpdate();
void playToneSuccess();
void playToneError();
void playToneLevelComplete();
void playToneNeutral();
void playToneVictory();
void uploadResults();
void startLevel(uint8_t levelIndex);
void IRAM_ATTR onGreenButtonFalling();
void IRAM_ATTR onRedButtonFalling();
void pollButtons();

// ---------------------------------------------------------------------------
// Multiplexer helpers
// ---------------------------------------------------------------------------
void setMuxChannel(int s0, int s1, int s2, int s3, uint8_t channel) {
  digitalWrite(s0, (channel >> 0) & 0x01);
  digitalWrite(s1, (channel >> 1) & 0x01);
  digitalWrite(s2, (channel >> 2) & 0x01);
  digitalWrite(s3, (channel >> 3) & 0x01);
  delayMicroseconds(MUX_SETTLE_US);
}

void setMuxEnabled(int enPin, bool enabled) {
  if (MUX_EN_TIED_TO_GND) {
    return;
  }
  // CD74HC4067 EN is active LOW
  digitalWrite(enPin, enabled ? LOW : HIGH);
}

// Maps hole index 0–35 → mux channel or direct GPIO.
//  0–15  : Mux #1 channels 0–15  (holes 1–16)
// 16–31  : Mux #2 channels 0–15  (holes 17–32)
// 32–35  : direct GPIO           (holes 33–36)
bool readSensor(uint8_t index) {
  if (index >= HOLE_COUNT) {
    return false;
  }

  int raw = LOW;

  if (index < MUX_CHANNEL_COUNT) {
    const uint8_t channel = index;
    setMuxEnabled(PIN_MUX2_EN, false);
    setMuxEnabled(PIN_MUX1_EN, true);
    setMuxChannel(PIN_MUX1_S0, PIN_MUX1_S1, PIN_MUX1_S2, PIN_MUX1_S3, channel);
    raw = digitalRead(PIN_MUX1_SIG);
  } else if (index < (MUX_CHANNEL_COUNT * 2)) {
    const uint8_t channel = static_cast<uint8_t>(index - MUX_CHANNEL_COUNT);
    setMuxEnabled(PIN_MUX1_EN, false);
    setMuxEnabled(PIN_MUX2_EN, true);
    setMuxChannel(PIN_MUX2_S0, PIN_MUX2_S1, PIN_MUX2_S2, PIN_MUX2_S3, channel);
    raw = digitalRead(PIN_MUX2_SIG);
  } else {
    const uint8_t directIndex = static_cast<uint8_t>(index - (MUX_CHANNEL_COUNT * 2));
    raw = digitalRead(DIRECT_SENSOR_PINS[directIndex]);
  }

  const bool logicalHigh = (raw == HIGH);
  return SENSOR_ACTIVE_LOW ? !logicalHigh : logicalHigh;
}

void scanSensors() {
  const uint32_t now = millis();

  for (uint8_t i = 0; i < HOLE_COUNT; i++) {
    const uint8_t row = i / GRID_SIZE;
    const uint8_t col = i % GRID_SIZE;
    const bool sample = readSensor(i);

    if (sample != debounceCandidate[row][col]) {
      debounceCandidate[row][col] = sample;
      debounceStartMs[row][col] = now;
      continue;
    }

    if ((now - debounceStartMs[row][col]) < SENSOR_DEBOUNCE_MS) {
      continue;
    }

    if (sample == lastStablePegState[row][col]) {
      continue;
    }

    lastStablePegState[row][col] = sample;
    currentPegState[row][col] = sample;

    Serial.print("[SENSOR] hole ");
    Serial.print(i + 1);  // 1-based hole number for humans
    Serial.print(" (r");
    Serial.print(row);
    Serial.print(",c");
    Serial.print(col);
    Serial.print(") -> ");
    Serial.println(sample ? "PEG IN" : "PEG OUT");

    handleSensorChange(row, col, sample);
  }

  setMuxEnabled(PIN_MUX1_EN, false);
  setMuxEnabled(PIN_MUX2_EN, false);
}

uint8_t targetAt(uint8_t level, uint8_t row, uint8_t col) {
  return pgm_read_byte(&(levelPatterns[level][row][col]));
}

bool patternMatchesCurrentLevel() {
  for (uint8_t r = 0; r < GRID_SIZE; r++) {
    for (uint8_t c = 0; c < GRID_SIZE; c++) {
      const bool needed = targetAt(currentLevel, r, c) != 0;
      if (currentPegState[r][c] != needed) {
        return false;
      }
    }
  }
  return true;
}

void handleSensorChange(uint8_t row, uint8_t col, bool inserted) {
  if (currentState != LEVEL_ACTIVE) {
    return;
  }

  const bool needed = targetAt(currentLevel, row, col) != 0;

  if (inserted) {
    if (needed) {
      playToneSuccess();
    } else {
      errorCount[currentLevel]++;
      Serial.print("[ERROR] wrong hole r");
      Serial.print(row);
      Serial.print(",c");
      Serial.print(col);
      Serial.print("  count=");
      Serial.println(errorCount[currentLevel]);
      playToneError();
    }
  } else {
    playToneNeutral();
  }

  if (patternMatchesCurrentLevel()) {
    elapsedTimeMs[currentLevel] = millis() - levelStartMs;
    Serial.print("[LEVEL] complete  time_ms=");
    Serial.print(elapsedTimeMs[currentLevel]);
    Serial.print("  errors=");
    Serial.println(errorCount[currentLevel]);
    playToneLevelComplete();
    enterState(LEVEL_COMPLETE);
  }
}

// ---------------------------------------------------------------------------
// Placeholder UI / audio / network
// ---------------------------------------------------------------------------
void displayUpdate() {
  // TODO: drive ILI9341 (Adafruit_ILI9341) per currentState
  // IDLE: wait-to-start
  // LEVEL_ACTIVE: Level X, Time mm:ss, Errors N
  // LEVEL_COMPLETE: results + “Press Green for next”
  // GAME_COMPLETE: congratulations + totals
}

void playToneSuccess() {
  // TODO: short rising two-note success melody on PIN_BUZZER
}

void playToneError() {
  // TODO: low buzz ~200 Hz, 300 ms on PIN_BUZZER
}

void playToneLevelComplete() {
  // TODO: two-tone chime on PIN_BUZZER
}

void playToneNeutral() {
  // TODO: short beep on peg removal (not success/error)
}

void playToneVictory() {
  // TODO: game-complete melody
}

void uploadResults() {
  // TODO: Wi-Fi connect + HTTP POST JSON
  // Payload: key, timestamp, level_times[], level_errors[], total_time, total_errors
  Serial.println("[WIFI] uploadResults() placeholder — not yet implemented");
}

void updateLeds() {
  switch (currentState) {
    case IDLE:
      digitalWrite(PIN_LED_GREEN, LOW);
      digitalWrite(PIN_LED_RED, HIGH);
      break;
    case LEVEL_ACTIVE:
      digitalWrite(PIN_LED_GREEN, HIGH);
      digitalWrite(PIN_LED_RED, LOW);
      break;
    case LEVEL_COMPLETE: {
      const bool blink = ((millis() / 100) % 2) == 0;
      digitalWrite(PIN_LED_GREEN, blink ? HIGH : LOW);
      digitalWrite(PIN_LED_RED, blink ? HIGH : LOW);
      break;
    }
    case GAME_COMPLETE:
      digitalWrite(PIN_LED_GREEN, HIGH);
      digitalWrite(PIN_LED_RED, HIGH);
      break;
  }
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
void enterState(GameState next) {
  if (next == currentState) {
    return;
  }

  Serial.print("[STATE] ");
  Serial.print(gameStateName(currentState));
  Serial.print(" -> ");
  Serial.println(gameStateName(next));

  currentState = next;
  displayUpdate();
  updateLeds();
}

void startLevel(uint8_t levelIndex) {
  if (levelIndex >= LEVEL_COUNT) {
    enterState(GAME_COMPLETE);
    playToneVictory();
    uploadResults();
    return;
  }

  currentLevel = levelIndex;
  errorCount[currentLevel] = 0;
  elapsedTimeMs[currentLevel] = 0;
  levelStartMs = millis();

  Serial.print("[LEVEL] starting ");
  Serial.println(currentLevel + 1);

  enterState(LEVEL_ACTIVE);
}

void handleStateMachine() {
  // Placeholder transitions are also driven from pollButtons() / handleSensorChange().
  switch (currentState) {
    case IDLE:
      break;
    case LEVEL_ACTIVE:
      break;
    case LEVEL_COMPLETE:
      break;
    case GAME_COMPLETE:
      break;
  }
}

// ---------------------------------------------------------------------------
// Buttons (ISR records press; loop classifies short vs long)
// ---------------------------------------------------------------------------
void IRAM_ATTR onGreenButtonFalling() {
  greenPressMs = millis();
  greenIrqPending = true;
}

void IRAM_ATTR onRedButtonFalling() {
  redPressMs = millis();
  redIrqPending = true;
}

void pollButtons() {
  const uint32_t now = millis();

  if (greenIrqPending) {
    greenIrqPending = false;
    greenHeld = true;
    greenDownMs = greenPressMs;
  }
  if (redIrqPending) {
    redIrqPending = false;
    redHeld = true;
    redDownMs = redPressMs;
  }

  const bool greenDown = digitalRead(PIN_BTN_GREEN) == LOW;
  const bool redDown = digitalRead(PIN_BTN_RED) == LOW;

  if (greenHeld && !greenDown) {
    const uint32_t heldMs = now - greenDownMs;
    greenHeld = false;

    if (heldMs >= BUTTON_LONG_MS) {
      Serial.println("[BTN] green LONG");
      if (currentState == LEVEL_ACTIVE) {
        startLevel(currentLevel + 1);  // testing: skip forward
      }
    } else if (heldMs < BUTTON_SHORT_MAX_MS) {
      Serial.println("[BTN] green SHORT");
      if (currentState == IDLE) {
        startLevel(0);
      } else if (currentState == LEVEL_COMPLETE) {
        if (currentLevel + 1 >= LEVEL_COUNT) {
          enterState(GAME_COMPLETE);
          playToneVictory();
          uploadResults();
        } else {
          startLevel(currentLevel + 1);
        }
      }
    }
  }

  if (redHeld && !redDown) {
    const uint32_t heldMs = now - redDownMs;
    redHeld = false;

    if (heldMs >= BUTTON_LONG_MS) {
      Serial.println("[BTN] red LONG");
      if (currentState == LEVEL_ACTIVE || currentState == LEVEL_COMPLETE) {
        const uint8_t prev = (currentLevel == 0) ? 0 : static_cast<uint8_t>(currentLevel - 1);
        startLevel(prev);  // testing: skip backward
      }
    } else {
      Serial.println("[BTN] red SHORT (ignored while playing)");
    }
  }
}

// ---------------------------------------------------------------------------
// Arduino entry
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[BOOT] Smart Peg Board scaffold");

  pinMode(PIN_MUX1_S0, OUTPUT);
  pinMode(PIN_MUX1_S1, OUTPUT);
  pinMode(PIN_MUX1_S2, OUTPUT);
  pinMode(PIN_MUX1_S3, OUTPUT);
  pinMode(PIN_MUX1_SIG, INPUT);
  if (!MUX_EN_TIED_TO_GND) {
    pinMode(PIN_MUX1_EN, OUTPUT);
  }

  pinMode(PIN_MUX2_S0, OUTPUT);
  pinMode(PIN_MUX2_S1, OUTPUT);
  pinMode(PIN_MUX2_S2, OUTPUT);
  pinMode(PIN_MUX2_S3, OUTPUT);
  pinMode(PIN_MUX2_SIG, INPUT);
  if (!MUX_EN_TIED_TO_GND) {
    pinMode(PIN_MUX2_EN, OUTPUT);
  }

  pinMode(PIN_SENSOR_33, INPUT);
  pinMode(PIN_SENSOR_34, INPUT);
  pinMode(PIN_SENSOR_35, INPUT);
  pinMode(PIN_SENSOR_36, INPUT);

  pinMode(PIN_BTN_GREEN, INPUT_PULLUP);
  pinMode(PIN_BTN_RED, INPUT);  // GPIO 36 is input-only; use external pull-up

  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  setMuxEnabled(PIN_MUX1_EN, false);
  setMuxEnabled(PIN_MUX2_EN, false);
  digitalWrite(PIN_BUZZER, LOW);

  attachInterrupt(digitalPinToInterrupt(PIN_BTN_GREEN), onGreenButtonFalling, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_RED), onRedButtonFalling, FALLING);

  enterState(IDLE);
  Serial.println("[STATE] entering IDLE");
}

void loop() {
  scanSensors();
  pollButtons();
  handleStateMachine();
  updateLeds();
  displayUpdate();
}

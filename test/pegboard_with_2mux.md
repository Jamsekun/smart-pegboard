# Author: James Balolong
---

# Smart Peg Board – Project Plan (v1.0)

## 1. Overview

A physical puzzle game for children. A 6×6 grid of holes is mounted on a board. Under each hole is an LM393-based photosensor module that detects the presence of a peg. The player must insert pegs to match a predefined pattern for each of 10 levels. The system:

- Detects peg insertion/removal using 36 digital sensor inputs.
- Provides immediate audio/visual feedback (buzzer tones and LEDs).
- Tracks elapsed time and error count per level.
- Displays current level, time, and errors on a 3.2" TFT.
- Advances through levels with button presses.
- After Level 10, uploads results to a web server via Wi‑Fi.

**Key features:**

- **36 sensors** are read using two 16‑channel analog/digital multiplexers (CD74HC4067) plus four direct GPIO inputs on the ESP32.
- **10 predefined levels** stored in flash memory.
- **Testing mode:** long‑press buttons to skip forward/backward between levels.
- **Debug output** via Serial.

---

## 2. Hardware

### 2.1 Component List

| Component | Qty | Notes |
|-----------|-----|-------|
| ESP32 (NodeMCU-32S) | 1 | Main controller |
| CD74HC4067 16‑ch mux | 2 | For 32 sensor inputs |
| LM393 photosensor module | 36 | Digital output (HIGH/LOW), adjustable threshold |
| 3.2" ILI9341 SPI TFT display | 1 | 320×240 pixels |
| 5V active buzzer | 1 | PWM or digital output |
| Green push button | 1 | Start / next level |
| Red push button | 1 | Reset / previous level (testing) |
| Red LED indicator | 1 | Status |
| Green LED indicator | 1 | Status |
| Power supply | 1 | 5V / 3A DC adapter or Li‑ion battery pack |
| Breadboard + jumper wires | – | Prototype construction |

### 2.2 Sensor Multiplexing Strategy

- **Mux #1** (CD74HC4067) handles sensors **1–16** (holes S1‑S16).
- **Mux #2** handles sensors **17–32** (holes S17‑S32).
- **Direct GPIO** handles sensors **33–36** (holes S33‑S36).

**Hole numbering convention:**  
The 6×6 grid is mapped row‑wise, starting from top‑left.  
Row 0: holes 1–6, Row 1: holes 7–12, … Row 5: holes 31–36.

Thus:

- Mux #1 covers holes 1–16 (Rows 0–2 plus holes 1–4 of Row 3).
- Mux #2 covers holes 17–32 (holes 5–6 of Row 3, Rows 4–5, and holes 1–2 of Row 5? Wait, 17–32 = 16 holes, so it covers holes 17–32 = Row 3 holes 5,6; Row 4 all 6; Row 5 holes 1,2,3,4? Let's recalc systematically. Row‑wise: Row 0: 1-6; Row 1: 7-12; Row 2: 13-18; Row 3: 19-24; Row 4: 25-30; Row 5: 31-36. So Mux1: 1-16 (Rows 0,1, and first 4 of Row 2). Mux2: 17-32 (last 2 of Row 2, Row 3, Row 4, first 2 of Row 5). Direct GPIO: 33-36 (last 4 of Row 5). That's consistent and covers all 36.

We'll define a mapping table in code (see Section 4.4).

### 2.3 Pin Mapping (ESP32 GPIO)

| Function | GPIO | Notes |
|----------|------|-------|
| **SPI – TFT** | | |
| TFT SCK | 18 | |
| TFT MOSI | 23 | |
| TFT DC | 2 | |
| TFT CS | 5 | |
| TFT RST | 4 | |
| **Mux #1** | | |
| S0 | 13 | Channel select bit 0 |
| S1 | 12 | bit 1 |
| S2 | 14 | bit 2 |
| S3 | 27 | bit 3 |
| SIG | 34 | Digital input from mux |
| EN | 15 | Active LOW enable (tie to GND or control) |
| **Mux #2** | | |
| S0 | 26 | |
| S1 | 25 | |
| S2 | 33 | |
| S3 | 32 | |
| SIG | 35 | Digital input |
| EN | 16 | Active LOW enable |
| **Direct sensors** | | |
| Sensor 33 | 21 | Digital input |
| Sensor 34 | 22 | Digital input |
| Sensor 35 | 19 | Digital input |
| Sensor 36 | 17? | Need to avoid conflict with buttons etc. |
| **Buttons** | | |
| Green button | 0? | Use pull‑up, active LOW |
| Red button | 36? | Input only? |
| **LEDs** | | |
| Green LED | 17? | Via transistor/resistor |
| Red LED | 5? | |
| **Buzzer** | 23? | PWM or digital |

*Note: The above mapping is tentative and must be validated against available pins and conflicts. ESP32 has many GPIOs; we will finalize after schematic design. We will reserve GPIOs 0, 2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39 as needed. Some pins have restrictions (input only, boot strapping). We'll choose a mapping that avoids boot issues.*

### 2.4 Power Supply

- Recommended: **5V / 3A DC adapter** (stable for prototype).  
- Alternative: **Li‑ion battery pack** (e.g., 2×18650 with boost converter to 5V).  
- The ESP32 operates at 3.3V; use an onboard regulator if not already present.  
- Sensor modules and multiplexers typically run at 5V but have 3.3V‑compatible outputs; check module specs.

---

## 3. Software Design

### 3.1 State Machine

States:

- `IDLE` – waiting for start. Red LED solid, green LED off.
- `LEVEL_ACTIVE` – game in progress. Green LED solid, red off. Timer running.
- `LEVEL_COMPLETE` – pattern matched; timer stopped; display results. LEDs short fast blink.
- `GAME_COMPLETE` – all levels finished. Show final summary, upload to web, play victory melody.
- *(Optional)* `ERROR` – unrecoverable fault.

Transitions:

- `IDLE` → `LEVEL_ACTIVE` on **Green button short press**.
- `LEVEL_ACTIVE` → `LEVEL_COMPLETE` when current sensor state equals target pattern.
- `LEVEL_COMPLETE` → `LEVEL_ACTIVE` (next level) on **Green button short press**.
- `LEVEL_COMPLETE` → `GAME_COMPLETE` if current level == 10 (after confirmation press).
- `LEVEL_ACTIVE` → `LEVEL_ACTIVE` (skip to next level) on **Green button long press (5 s)** – testing mode.
- `LEVEL_ACTIVE` → `LEVEL_ACTIVE` (skip to previous level) on **Red button long press (5 s)** – testing mode.
- `LEVEL_COMPLETE` → `LEVEL_ACTIVE` (previous level) on **Red button long press (5 s)** – testing mode.

### 3.2 Level Patterns

Stored as a constant 3D array in `PROGMEM`:

```cpp
const uint8_t levelPatterns[10][6][6] = {
    // Level 1
    {
        {1,1,1,1,1,1},
        {0,0,0,0,0,0},
        {0,0,0,0,0,0},
        {0,0,0,0,0,0},
        {0,0,0,0,0,0},
        {0,0,0,0,0,0}
    },
    // Level 2
    {
        {1,1,1,1,1,1},
        {1,0,0,0,0,1},
        {1,0,0,0,0,1},
        {1,0,0,0,0,1},
        {1,0,0,0,0,1},
        {1,1,1,1,1,1}
    },
    // Level 3 – Placeholder (copy and edit)
    {
        {1,1,1,1,0,0},
        {1,1,1,1,0,0},
        {1,1,1,1,0,0},
        {1,1,1,1,0,0},
        {1,1,1,1,0,0},
        {1,1,1,1,0,0}
    },

    // Level 4 – Placeholder (copy and edit)
    {
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1}
    },

        // Level 5 – Placeholder (copy and edit)
    {
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1}
    },

        // Level 6 – Placeholder (copy and edit)
    {
        {0,0,1,1,0,0},
        {0,0,1,1,0,0},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {0,0,1,1,0,0},
        {0,0,1,1,0,0}
    },

    // Level 7 – Placeholder (copy and edit)
    {
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1}
    },

    // Level 8 – Placeholder (copy and edit)
    {
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1}
    },

    // Level 9 – Placeholder (copy and edit)
    {
        {0,0,1,1,0,0},
        {0,1,0,0,1,0},
        {1,1,1,1,1,1},
        {0,1,1,1,1,0},
        {0,1,1,1,1,0},
        {0,1,1,1,1,0}
    },

    // Level 10 – Placeholder (copy and edit)
    {
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1},
        {1,1,1,1,1,1}
    }
    // ... up to Level 10
};
```

The user will fill in Levels 3–10 later.

### 3.3 Sensor Reading

- Each sensor is digital (0 = no peg, 1 = peg present).  
- To read all 36 sensors:
  1. For each of the 16 channels on Mux #1, set S0‑S3, read SIG, store in `currentState`.
  2. Repeat for Mux #2.
  3. Read the four direct GPIO pins for holes 33–36.
- Apply debouncing: require the same reading for 20 ms before accepting a change.
- Maintain a `currentState[6][6]` array representing actual peg positions.

### 3.4 Correct/Incorrect Detection

- On every sensor change (0→1 or 1→0):
  - **Peg inserted (0→1):**
    - If `targetState[row][col] == 1` → play success tone.
    - Else → play error tone and increment `errorCount` (only if this hole was previously 0; repeated insertions while peg stays do not increment).
  - **Peg removed (1→0):**
    - Play a short beep (neutral, not error/success). This is allowed and does not affect errors.
- After each change, check if `currentState` equals `targetState`. If yes → level complete.

### 3.5 Timer & Errors

- `elapsedTime` starts when entering `LEVEL_ACTIVE` (after start button).
- `errorCount` per level.
- Errors are counted in real time on wrong insertion as described above.
- At level completion, the time and error count are displayed and stored.

### 3.6 Button Handling

- Use **interrupts** on both buttons (falling edge) for responsiveness.
- In the ISR, record the timestamp of press/release.
- Main loop debounces and classifies:
  - **Short press** (< 3 s): normal action.
  - **Long press** (≥ 5 s): testing skip action.
- Green button:
  - Short: start next level (if IDLE or LEVEL_COMPLETE).
  - Long: skip to next level (testing, only when LEVEL_ACTIVE).
- Red button:
  - Short: ignored while playing.
  - Long: skip to previous level (testing, only when LEVEL_ACTIVE or LEVEL_COMPLETE).

### 3.7 Display UI (ILI9341 TFT)

**During gameplay (LEVEL_ACTIVE):**
- Top: “Level X” (large font)
- Middle: “Time: mm:ss”
- Below: “Errors: N”
- Bottom: status message (e.g., “Insert pegs!”)

**On level complete (LEVEL_COMPLETE):**
- “Level X Complete!”
- “Time: mm:ss”
- “Errors: N”
- “Press Green for next”

**Game complete (GAME_COMPLETE):**
- “Congratulations!”
- “Total Time: mm:ss”
- “Total Errors: N”
- Summary table (optional, if display space permits)

**Note:** The target pattern is **not** shown on the display because it is physically printed on the board.

### 3.8 Buzzer & LED Feedback

**Buzzer tones:**

- **Success (correct peg):** short happy melody (e.g., two rising notes).
- **Error (wrong peg):** low buzz (200 Hz, 300 ms).
- **Level complete:** two‑tone chime.
- **Game complete:** Final Fantasy victory theme (sequence of notes).

**LED indicators:**

| State | Green LED | Red LED |
|-------|-----------|---------|
| IDLE | Off | Solid |
| LEVEL_ACTIVE | Solid | Off |
| LEVEL_COMPLETE | Short fast blink | Short fast blink |
| Error (momentary) | – | Short fast blink |

*Note: During error, the red LED may blink briefly; otherwise, the state LED remains solid.*

### 3.9 Wi‑Fi & Web Upload

- **Wi‑Fi credentials:** hardcoded in `config.h` (SSID and password).
- **Endpoint:** HTTP POST to a predefined URL.
- **Payload (JSON):**
  ```json
  {
    "key": "device_or_player_key",
    "timestamp": "2025-01-01T12:00:00Z",
    "level_times": [12.5, 15.3, ...],
    "level_errors": [0, 1, ...],
    "total_time": 123.4,
    "total_errors": 3
  }
  ```
- **Key:** Hardcoded in `config.h`, sent in the JSON body (or HTTP header if preferred).
- **Implementation:** Use `HTTPClient` library. If upload fails, retry or store locally? (Decide later; simplest: just attempt and log result to Serial).

### 3.10 Debugging

- Use `Serial.begin(115200)`.
- Print:
  - State transitions (e.g., “Entering LEVEL_ACTIVE”).
  - Sensor changes (hole number, new state).
  - Errors and timer at level completion.
  - Final results after Level 10.
  - Wi‑Fi connection status and upload response.

---

## 4. Implementation Notes

### 4.1 Sensor‑to‑Multiplexer Mapping

Define an array `sensorMap[36]` that maps each hole index (0‑35) to a physical input source:

- For holes 0‑15 (1–16): Mux #1 channel 0–15.
- For holes 16‑31 (17–32): Mux #2 channel 0–15.
- For holes 32‑35 (33–36): direct GPIO pins (input).

In code, we can create functions:

```cpp
bool readSensor(uint8_t index); // index 0-35
```

which internally selects the correct mux channel or reads the GPIO.

### 4.2 Code Structure

```
/src
├── main.cpp          // setup, loop, state machine
├── config.h          // WiFi credentials, pins, thresholds
├── sensors.h/.cpp    // sensor reading and multiplexer control
├── display.h/.cpp    // TFT functions
├── buzzer.h/.cpp     // tone generation
├── buttons.h/.cpp    // button ISR and debounce
├── levels.h/.cpp     // pattern storage and access
├── wifi.h/.cpp       // upload results
└── state_machine.h/.cpp
```

### 4.3 Development Environment

- PlatformIO or Arduino IDE.
- Libraries: `Adafruit_ILI9341`, `Adafruit_GFX`, `HTTPClient`, `WiFi`, `ArduinoJson` (optional).

---

## 5. Testing Plan

1. **Sensor calibration:** Adjust each LM393 module potentiometer so that a peg reliably triggers a HIGH, and absence gives LOW under ambient light.
2. **Multiplexer mapping:** Verify that each sensor corresponds to the correct hole by toggling pegs and monitoring Serial output.
3. **Button debouncing:** Test short and long presses, ensure no false triggers.
4. **State machine:** Walk through each state and transition manually.
5. **Display:** Verify UI updates smoothly without flickering.
6. **Wi‑Fi upload:** Use a local test server (e.g., Postman mock) to confirm JSON payload.
7. **Full game test:** Play through all levels (using skip functions if needed) and confirm final upload.

---

## 6. Future Improvements

- Add level selection menu.
- Store results locally (SD card) if upload fails.
- Random level generation.
- Adjustable difficulty (timer limits, error penalties).
- Battery level monitoring.
- OTA firmware updates.

---

This document provides a solid foundation for beginning development. Adjust pin assignments and minor details as you build the prototype. Good luck!
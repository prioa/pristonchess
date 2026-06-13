#include "board_driver.h"
#include "chess_utils.h"
#include "led_colors.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#include <math.h>
#include "serial_tee.h"  // must be last: redefines Serial -> tee

// 74HC595 shift register pin mapping: bits are sent MSB first, so bit 7 shifts to QH, bit 0 stays at QA
// col 0 -> QA (pin 15), col 1 -> QB (pin 1), ..., col 7 -> QH (pin 7)
static int shiftRegPin(int col) {
  const int pins[] = {15, 1, 2, 3, 4, 5, 6, 7}; // QA=15, QB=1, QC=2, QD=3, QE=4, QF=5, QG=6, QH=7
  return (col >= 0 && col < 8) ? pins[col] : -1;
}
static char shiftRegOutput(int col) {
  return (col >= 0 && col < 8) ? (char)('A' + col) : '?'; // col 0 -> 'A' (QA), col 7 -> 'H' (QH)
}

// Static members for animation queue system
QueueHandle_t BoardDriver::animationQueue = nullptr;
TaskHandle_t BoardDriver::animationTaskHandle = nullptr;
SemaphoreHandle_t BoardDriver::ledMutex = nullptr;
BoardDriver* BoardDriver::instance = nullptr;

static LedRGB hsvToRgb(float hue, float sat, float val) {
  float h = fmod(hue, 360.0f) / 60.0f;
  int hi = (int)h;
  float f = h - hi, p = val * (1.0f - sat), q = val * (1.0f - sat * f), t = val * (1.0f - sat * (1.0f - f));
  float r, g, b;
  switch (hi % 6) {
    case 0: r=val;g=t;  b=p;   break;
    case 1: r=q;  g=val;b=p;   break;
    case 2: r=p;  g=val;b=t;   break;
    case 3: r=p;  g=q;  b=val; break;
    case 4: r=t;  g=p;  b=val; break;
    default:r=val;g=p;  b=q;   break;
  }
  return {(uint8_t)(r*255), (uint8_t)(g*255), (uint8_t)(b*255)};
}

static float rgbToHue(LedRGB c) {
  float r = c.r/255.0f, g = c.g/255.0f, b = c.b/255.0f;
  float mx = max(max(r,g),b), mn = min(min(r,g),b), d = mx - mn;
  if (d < 1e-6f) return 0.0f;
  float h;
  if (mx == r)      h = 60.0f * fmod((g-b)/d, 6.0f);
  else if (mx == g) h = 60.0f * ((b-r)/d + 2.0f);
  else              h = 60.0f * ((r-g)/d + 4.0f);
  return h < 0 ? h + 360.0f : h;
}

static float rgbSaturation(LedRGB c) {
  float r = c.r/255.0f, g = c.g/255.0f, b = c.b/255.0f;
  float mx = max(max(r,g),b);
  return (mx < 1e-6f) ? 0.0f : (mx - min(min(r,g),b)) / mx;
}

// 5x7 column-major font, ASCII 0x20..0x5A (space..'Z'); bit 0 = top row.
// Shared by the scrolling-text idle animation (doText) and the blocking boot
// status messages (showBootMessage).
static const uint8_t TEXT_FONT[][5] = {
  {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
  {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
  {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
  {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
  {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
  {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
  {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
  {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
  {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
  {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
  {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
  {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
  {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
  {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};
static const int TEXT_FONT_GLYPHS = (int)(sizeof(TEXT_FONT) / 5);
// Map a character to its glyph row in TEXT_FONT (folds lowercase to uppercase,
// unknown chars -> space).
static int textGlyphIndex(char ch) {
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  int idx = (int)ch - 0x20;
  return (idx >= 0 && idx < TEXT_FONT_GLYPHS) ? idx : 0;
}

// ---------------------------
// LED Strip Col/Row to Pixel index mapping (default)
// ---------------------------
// Straight (non-serpentine) wiring: every row runs left→right (DI→DO) and the
// rows are stacked, so the strip index is simply row * NUM_COLS + col.
static constexpr int DefaultRowColToLEDindexMap[NUM_ROWS][NUM_COLS] = {
    {0,  1,  2,  3,  4,  5,  6,  7},
    {8,  9,  10, 11, 12, 13, 14, 15},
    {16, 17, 18, 19, 20, 21, 22, 23},
    {24, 25, 26, 27, 28, 29, 30, 31},
    {32, 33, 34, 35, 36, 37, 38, 39},
    {40, 41, 42, 43, 44, 45, 46, 47},
    {48, 49, 50, 51, 52, 53, 54, 55},
    {56, 57, 58, 59, 60, 61, 62, 63},
};

BoardDriver::BoardDriver() : strip(nullptr), lastEnabledCol(-2), brightness(BRIGHTNESS), dimMultiplier(70), idleAnimation(IdleAnimation::RAINBOW), animSpeed(1.0f), idleSaturation(100), idleRandomColor(false), autoDim(false), nightOff(false), autoDimStart(22), autoDimEnd(7), autoDimBrightness(60), swapAxes(0), hwConfig(HardwareConfig::defaults()), calibrating(false), animRestartPending(false), calibrated(false), debugHeldCol(-1) {
  idleColor  = {255, 255, 255};
  idleColor2 = {255, 128, 0};
  textColor  = {255, 255, 255};
  textBg     = {0, 0, 0};
  textMode   = 0;
  strncpy(idleText, "PRISTON CHESS", sizeof(idleText) - 1);
  idleText[sizeof(idleText) - 1] = '\0';
  for (int s = 0; s < PAINT_SLOTS; s++)
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++)
        paintGrids[s][r][c] = {0, 0, 0};
  paintSel = 0;
  for (int s = 0; s < PAINT_SLOTS; s++) paintNames[s][0] = '\0';
  for (int c = 0; c < NUM_COLS; c++) audioBands[c] = 0;
  audioLastMs = 0;
  gameDir = 1; gameInputMs = 0; gameSeq = 0;
  for (int i = 0; i < NUM_ROWS; i++)
    toLogicalRow[i] = i;
  for (int i = 0; i < NUM_COLS; i++)
    toLogicalCol[i] = i;
  for (int row = 0; row < NUM_ROWS; row++)
    for (int col = 0; col < NUM_COLS; col++) {
      ledIndexMap[row][col] = DefaultRowColToLEDindexMap[row][col];
      currentColors[row][col] = LedColors::Off;
    }
}

void BoardDriver::beginHardware() {
  // Initialize animation queue system
  instance = this;
  ledMutex = xSemaphoreCreateMutex();
  animationQueue = xQueueCreate(8, sizeof(AnimationJob));
  // Priority 2 (above the Arduino loopTask's priority 1) so animation frames
  // preempt the busy main loop and render on schedule — otherwise heavy loop
  // work (Stockfish, sensor scan, JSON) stalls frames and the LEDs look laggy.
  // Frames only need a few ms then vTaskDelay back to sleep, so the loop is fine.
  // 8 KB stack — doMultiPathHighlight alone burns ~3 KB on locals (paths,
  // base/highlight snapshots, acc grid) and the replay loop's nesting adds
  // call-frame depth on top. 4 KB was overflowing after 2-3 moves.
  xTaskCreatePinnedToCore(animationWorkerTask, "AnimWorker", 8192, nullptr, 2, &animationTaskHandle, 1);
  // Load hardware pin configuration from NVS (must happen before any GPIO or strip init)
  loadHardwareConfig();
  loadHighlightColors();
  // https://github.com/Makuna/NeoPixelBus/wiki/ESP32-NeoMethods
  strip = new NeoPixelBusLg<NeoGrbFeature, NeoEsp32Rmt1Ws2812xMethod, NeoGammaNullMethod>(LED_COUNT, hwConfig.ledPin);
  strip->Begin();
  loadLedSettings(); // Load LED settings from NVS (brightness, dim multiplier)
  strip->SetLuminance(brightness);
  // Shift register pins as outputs
  pinMode(hwConfig.srDataPin, OUTPUT);
  pinMode(hwConfig.srClkPin, OUTPUT);
  pinMode(hwConfig.srLatchPin, OUTPUT);
  disableAllCols();
  // Row pins as inputs with pull-up (A3144 open-collector pulls LOW when active)
  for (int i = 0; i < NUM_ALL_ROW_PINS; i++)
    pinMode(ALL_ROW_PINS[i], INPUT_PULLUP);
  calibrated = loadCalibration();
  if (calibrated) {
    // Initialize sensors state without debouncing to prevent brief LED flashes at boot (live game recover board setup)
    bool initialRawState[NUM_ROWS][NUM_COLS];
    readRawSensors(initialRawState);
    unsigned long now = millis();
    for (int col = 0; col < NUM_COLS; col++) {
      for (int row = 0; row < NUM_ROWS; row++) {
        uint8_t logicalRow = toLogicalRow[swapAxes ? col : row];
        uint8_t logicalCol = toLogicalCol[swapAxes ? row : col];
        sensorState[logicalRow][logicalCol] = initialRawState[row][col];
        sensorPrev[logicalRow][logicalCol] = initialRawState[row][col];
        sensorRaw[logicalRow][logicalCol] = initialRawState[row][col];
        sensorDebounceTime[logicalRow][logicalCol] = now;
      }
    }
  } else {
    for (int row = 0; row < NUM_ROWS; row++) {
      for (int col = 0; col < NUM_COLS; col++) {
        sensorState[row][col] = false;
        sensorPrev[row][col] = false;
        sensorRaw[row][col] = false;
        sensorDebounceTime[row][col] = 0;
      }
    }
  }
}

void BoardDriver::checkCalibration() {
  if (!calibrated) {
    bool wasSkipped = runCalibration();
    if (!wasSkipped) {
      saveCalibration();
      calibrated = true;
    }
  }
}

// Animation worker task - processes jobs from queue
void BoardDriver::animationWorkerTask(void* param) {
  AnimationJob job;
  while (true) {
    if (xQueueReceive(animationQueue, &job, portMAX_DELAY) == pdTRUE) {
      // Skip animations while calibration is running on the main task
      if (instance->calibrating.load()) {
        if (job.stopFlag)
          delete job.stopFlag;
        continue;
      }
      xSemaphoreTake(ledMutex, portMAX_DELAY);
      instance->executeAnimation(job);
      xSemaphoreGive(ledMutex);
    }
  }
}

void BoardDriver::executeAnimation(const AnimationJob& job) {
  switch (job.type) {
    case AnimationType::CAPTURE:
      doCapture(job.params.capture.row, job.params.capture.col);
      break;
    case AnimationType::PROMOTION:
      doPromotion(job.params.promotion.row, job.params.promotion.col);
      break;
    case AnimationType::BLINK:
      doBlink(job.params.blink.row, job.params.blink.col, job.params.blink.color, job.params.blink.times, job.params.blink.clearAfter);
      break;
    case AnimationType::WAITING:
      doWaiting(job.stopFlag);
      break;
    case AnimationType::THINKING:
      doThinking(job.stopFlag);
      break;
    case AnimationType::FIREWORK:
      doFirework(job.params.firework.color);
      break;
    case AnimationType::FLASH:
      doFlash(job.params.flash.color, job.params.flash.times);
      break;
    case AnimationType::RAINBOW:      doRainbow(job.stopFlag);     break;
    case AnimationType::BREATHING:    doBreathing(job.stopFlag);   break;
    case AnimationType::CHASE:        doChase(job.stopFlag);       break;
    case AnimationType::TWINKLE:      doTwinkle(job.stopFlag);     break;
    case AnimationType::SOLID:        doSolid(job.stopFlag);       break;
    case AnimationType::FIRE:         doFire(job.stopFlag);        break;
    case AnimationType::PLASMA:       doPlasma(job.stopFlag);      break;
    case AnimationType::METEOR:       doMeteor(job.stopFlag);      break;
    case AnimationType::CHESS_PULSE:  doChessPulse(job.stopFlag);  break;
    case AnimationType::COLOR_CYCLE:  doColorCycle(job.stopFlag);  break;
    case AnimationType::SCAN:         doScan(job.stopFlag);        break;
    case AnimationType::FIREWORKS:    doFireworks(job.stopFlag);   break;
    case AnimationType::RIPPLE:       doRipple(job.stopFlag);      break;
    case AnimationType::MATRIX:       doMatrixRain(job.stopFlag);  break;
    case AnimationType::AURORA:       doAurora(job.stopFlag);      break;
    case AnimationType::SPIRAL:       doSpiral(job.stopFlag);      break;
    case AnimationType::STANDOFF:     doStandoff(job.stopFlag);    break;
    case AnimationType::KNIGHT_TOUR:  doKnightTour(job.stopFlag);  break;
    case AnimationType::REPLAY:       doReplay(job.stopFlag);      break;
    case AnimationType::ATTACK:       doAttack(job.stopFlag);      break;
    case AnimationType::TOUCH:        doTouch(job.stopFlag);       break;
    case AnimationType::LIFE:         doLife(job.stopFlag);        break;
    case AnimationType::SAND:         doSand(job.stopFlag);        break;
    case AnimationType::LAVA:         doLava(job.stopFlag);        break;
    case AnimationType::RAIN:         doRain(job.stopFlag);        break;
    case AnimationType::STARFIELD:    doStarfield(job.stopFlag);   break;
    case AnimationType::WALLCLOCK:    doWallclock(job.stopFlag);   break;
    case AnimationType::TEXT:         doText(job.stopFlag);        break;
    case AnimationType::PAINT:        doPaint(job.stopFlag);       break;
    case AnimationType::TETRIS:       doTetris(job.stopFlag);      break;
    case AnimationType::AUDIO:        doAudio(job.stopFlag);       break;
    case AnimationType::SNAKE:        doSnake(job.stopFlag);       break;
    case AnimationType::PONG:         doPong(job.stopFlag);        break;
    case AnimationType::LANGTON:      doLangton(job.stopFlag);     break;
    case AnimationType::BOUNCE:       doBounce(job.stopFlag);      break;
    case AnimationType::SNOW:         doSnow(job.stopFlag);        break;
    case AnimationType::BUBBLES:      doBubbles(job.stopFlag);     break;
    case AnimationType::SORT:         doSort(job.stopFlag);        break;
    case AnimationType::WAVE:         doWave(job.stopFlag);        break;
    case AnimationType::RULE30:       doRule30(job.stopFlag);      break;
    case AnimationType::BREAKOUT:     doBreakout(job.stopFlag);    break;
    case AnimationType::TRON:         doTron(job.stopFlag);        break;
    case AnimationType::CATCH:        doCatch(job.stopFlag);       break;
    case AnimationType::SIMON:        doSimon(job.stopFlag);       break;
    case AnimationType::CHECK_BORDER: doCheckBorderFlash();        break;
    case AnimationType::CHARGE:       doCharge(job.stopFlag);      break;
    case AnimationType::SHIELDWALL:   doShieldwall(job.stopFlag);  break;
    case AnimationType::COURT:        doCourt(job.stopFlag);       break;
    case AnimationType::MOVE_PATH:
      // Stash the player so doMovePath can pick the per-side trail colour /
      // speed without changing its existing signature.
      mphPlayer = job.params.path.player ? job.params.path.player : 'w';
      doMovePath(job.params.path.fromRow, job.params.path.fromCol,
                 job.params.path.toRow,   job.params.path.toCol);
      break;
    case AnimationType::MULTI_PATH_HL: doMultiPathHighlight(); break;
    case AnimationType::CHECKMATE_SPIRAL: doCheckmateSpiral(); break;
    case AnimationType::CHECKMATE_ANIM:   doCheckmateAnimation(); break;
    case AnimationType::MODE_CHANGE_TV:   doModeChangeTransition(); break;
    case AnimationType::CELL_PULSE:
      doCellPulse(job.params.blink.row, job.params.blink.col, job.params.blink.color);
      break;
  }
}

bool BoardDriver::loadCalibration() {
  if (!ChessUtils::ensureNvsInitialized()) {
    Serial.println("NVS init failed - calibration not loaded");
    return false;
  }
  Preferences prefs;
  prefs.begin("boardCal", false);
  uint8_t ver = prefs.getUChar("ver", 0);
  if (ver != 1) {
    prefs.end();
    return false;
  }

  // Verify pin configuration matches
  size_t rowPinsLen = prefs.getBytesLength("rowPins");
  if (rowPinsLen != NUM_ROWS) {
    prefs.end();
    return false;
  }
  uint8_t savedRowPins[NUM_ROWS];
  prefs.getBytes("rowPins", savedRowPins, sizeof(savedRowPins));
  for (int i = 0; i < NUM_ROWS; i++)
    if (savedRowPins[i] != hwConfig.rowPins[i]) {
      prefs.end();
      return false;
    }
  uint8_t savedSRPins[3];
  prefs.getBytes("srPins", savedSRPins, sizeof(savedSRPins));
  if (savedSRPins[0] != hwConfig.srClkPin || savedSRPins[1] != hwConfig.srLatchPin || savedSRPins[2] != hwConfig.srDataPin) {
    prefs.end();
    return false;
  }

  size_t rowLen = prefs.getBytesLength("row");
  size_t colLen = prefs.getBytesLength("col");
  size_t ledLen = prefs.getBytesLength("led");
  if (rowLen != NUM_ROWS || colLen != NUM_COLS || ledLen != LED_COUNT) {
    prefs.end();
    return false;
  }
  swapAxes = prefs.getUChar("swap", 0);
  prefs.getBytes("row", toLogicalRow, NUM_ROWS);
  prefs.getBytes("col", toLogicalCol, NUM_COLS);
  uint8_t ledFlat[LED_COUNT];
  prefs.getBytes("led", ledFlat, LED_COUNT);
  int idx = 0;
  for (int row = 0; row < NUM_ROWS; row++)
    for (int col = 0; col < NUM_COLS; col++)
      ledIndexMap[row][col] = ledFlat[idx++];
  prefs.end();
  Serial.println("Board calibration loaded from NVS");
  return true;
}

void BoardDriver::saveCalibration() {
  if (!ChessUtils::ensureNvsInitialized()) {
    Serial.println("NVS init failed - calibration not saved");
    return;
  }
  Preferences prefs;
  prefs.begin("boardCal", false);
  prefs.putUChar("ver", 1);
  prefs.putBytes("rowPins", hwConfig.rowPins, sizeof(hwConfig.rowPins));
  uint8_t srPins[3] = {hwConfig.srClkPin, hwConfig.srLatchPin, hwConfig.srDataPin};
  prefs.putBytes("srPins", srPins, sizeof(srPins));
  prefs.putUChar("swap", swapAxes);
  prefs.putBytes("row", toLogicalRow, NUM_ROWS);
  prefs.putBytes("col", toLogicalCol, NUM_COLS);
  uint8_t ledFlat[LED_COUNT];
  int idx = 0;
  for (int row = 0; row < NUM_ROWS; row++)
    for (int col = 0; col < NUM_COLS; col++)
      ledFlat[idx++] = ledIndexMap[row][col];
  prefs.putBytes("led", ledFlat, LED_COUNT);
  prefs.end();
  Serial.println("Board calibration saved to NVS");
}

void BoardDriver::readRawSensors(bool rawState[NUM_ROWS][NUM_COLS]) {
  for (int row = 0; row < NUM_ROWS; row++)
    for (int col = 0; col < NUM_COLS; col++)
      rawState[row][col] = false;

  for (int col = 0; col < NUM_COLS; col++) {
    enableCol(col);
    for (int row = 0; row < NUM_ROWS; row++)
      rawState[row][col] = (digitalRead(hwConfig.rowPins[row]) == LOW);
  }
  disableAllCols();
}

bool BoardDriver::waitForBoardEmpty(unsigned long stableMs) {
  bool rawState[NUM_ROWS][NUM_COLS];
  unsigned long lastWarningTime = millis();
  unsigned long lastLedUpdate = 0;
  unsigned long stableStart = 0;
  uint8_t pulse = 0;
  int8_t pulseDir = 4;

  while (true) {
    readRawSensors(rawState);
    int pressedCount = 0;
    for (int row = 0; row < NUM_ROWS; row++)
      for (int col = 0; col < NUM_COLS; col++)
        if (rawState[row][col])
          pressedCount++;

    unsigned long now = millis();

    // Visual feedback: triggered squares blink red, empty squares dim orange pulse
    if (now - lastLedUpdate >= 40) {
      lastLedUpdate = now;
      pulse += pulseDir;
      if (pulse >= 60 || pulse == 0) pulseDir = -pulseDir;
      for (int row = 0; row < NUM_ROWS; row++) {
        for (int col = 0; col < NUM_COLS; col++) {
          if (rawState[row][col])
            strip->SetPixelColor(ledIndexMap[row][col], RgbColor(200, 0, 0)); // red = still triggered
          else
            strip->SetPixelColor(ledIndexMap[row][col], RgbColor(pulse / 4, 0, 0)); // dim pulse = waiting
        }
      }
      strip->Show();
    }

    if (pressedCount == 0) {
      if (stableStart == 0)
        stableStart = now;
      if (now - stableStart >= stableMs) {
        clearAllLEDs();
        return true;
      }
    } else {
      stableStart = 0;
      if (now - lastWarningTime >= CALIBRATION_WARNING_INTERVAL_MS) {
        lastWarningTime = now;
        Serial.printf("Board not empty - %d sensor(s) still detecting a magnet:\n", pressedCount);
        for (int row = 0; row < NUM_ROWS; row++)
          for (int col = 0; col < NUM_COLS; col++)
            if (rawState[row][col])
              Serial.printf("  GPIO %d + 74HC595 Q%c (pin %d)\n", hwConfig.rowPins[row], shiftRegOutput(col), shiftRegPin(col));
      }
    }
    delay(SENSOR_READ_DELAY_MS);
  }
}

bool BoardDriver::waitForSingleRawPress(int& rawRow, int& rawCol, unsigned long stableMs) {
  bool rawState[NUM_ROWS][NUM_COLS];
  int lastRow = -1;
  int lastCol = -1;
  unsigned long stableStart = 0;
  unsigned long lastWarningTime = millis();

  while (true) {
    readRawSensors(rawState);
    int count = 0;
    int foundRow = -1;
    int foundCol = -1;
    for (int row = 0; row < NUM_ROWS; row++)
      for (int col = 0; col < NUM_COLS; col++)
        if (rawState[row][col]) {
          count++;
          foundRow = row;
          foundCol = col;
        }
    if (count == 1) {
      if (foundRow == lastRow && foundCol == lastCol) {
        if (stableStart == 0) {
          stableStart = millis();
          Serial.printf("  Detect start: GPIO %d + 74HC595 Q%c (pin %d)\n", hwConfig.rowPins[foundRow], shiftRegOutput(foundCol), shiftRegPin(foundCol));
        }
        if (millis() - stableStart >= stableMs) {
          rawRow = foundRow;
          rawCol = foundCol;
          return true;
        }
      } else {
        // Position changed - warn about unstable detection
        if (lastRow >= 0 && lastCol >= 0) {
          Serial.println("Sensor reading unstable - detected square changed. Hold piece steady on one square.");
          Serial.printf("  Previous: GPIO %d + 74HC595 Q%c (pin %d), Current: GPIO %d + 74HC595 Q%c (pin %d)\n", hwConfig.rowPins[lastRow], shiftRegOutput(lastCol), shiftRegPin(lastCol), hwConfig.rowPins[foundRow], shiftRegOutput(foundCol), shiftRegPin(foundCol));
        }
        lastRow = foundRow;
        lastCol = foundCol;
        stableStart = 0;
      }
    } else {
      // Multiple presses or no presses detected
      unsigned long now = millis();
      if (now - lastWarningTime >= CALIBRATION_WARNING_INTERVAL_MS) {
        lastWarningTime = now;
        if (count == 0) {
          Serial.println("No sensor detecting a magnet - place a piece on the requested square");
        } else {
          Serial.printf("Multiple sensors (%d) detected simultaneously but need exactly 1:\n", count);
          for (int row = 0; row < NUM_ROWS; row++)
            for (int col = 0; col < NUM_COLS; col++)
              if (rawState[row][col])
                Serial.printf("  GPIO %d + 74HC595 Q%c (pin %d)\n", hwConfig.rowPins[row], shiftRegOutput(col), shiftRegPin(col));
        }
      }
      stableStart = 0;
    }
    delay(SENSOR_READ_DELAY_MS);
  }
}

void BoardDriver::showCalibrationError() {
  for (int i = 0; i < LED_COUNT; i++)
    strip->SetPixelColor(i, RgbColor(LedColors::Red.r, LedColors::Red.g, LedColors::Red.b));
  showLEDs();
  delay(500);
  waitForBoardEmpty();
  clearAllLEDs();
}

bool BoardDriver::calibrateAxis(Axis axis, uint8_t* axisPinsOrder, size_t NUM_PINS, bool firstAxisSwapped) {
  if ((NUM_ROWS != NUM_COLS) || (NUM_ROWS != NUM_PINS)) {
    Serial.println("Non-square boards not supported for calibration");
    return false;
  }

  Axis detectedAxis = UnknownAxis;
  int firstRow = -1;
  int firstCol = -1;
  uint8_t counts[NUM_PINS] = {0};
  for (int i = 0; i < NUM_PINS; i++)
    axisPinsOrder[i] = -1;

  // If this is column calibration and we already have row mapping, find expected raw row/col for rank 1
  int expectedRawPin = -1;
  bool useRow = true; // Whether to check row or col during column calibration
  if (axis == ColsAxis)
    for (int i = 0; i < NUM_ROWS; i++)
      if (toLogicalRow[i] == 7) {
        expectedRawPin = i;
        // If first axis swapped, toLogicalRow is indexed by raw cols, so we check col
        useRow = !firstAxisSwapped;
        break;
      }

  for (int i = 0; i < NUM_PINS; i++) {
    char square[3];
    if (axis == RowsAxis) {
      square[0] = 'a';
      square[1] = (char)('8' - i);
    } else {
      square[0] = (char)('a' + i);
      square[1] = '1';
    }
    square[2] = '\0';

    Serial.printf("Place a piece on %s (%s calibration)\n", square, axisToChessRankFile(axis).c_str());
    int row = 0;
    int col = 0;
    waitForSingleRawPress(row, col);
    Serial.printf("  Detected: row=%d (GPIO %d), col=%d (74HC595 Q%c, pin %d)\n", row, hwConfig.rowPins[row], col, shiftRegOutput(col), shiftRegPin(col));

    // Verify pin consistency for column calibration
    if (axis == ColsAxis && expectedRawPin != -1) {
      int actualPin = useRow ? row : col;
      if (actualPin != expectedRawPin) {
        if (useRow)
          Serial.printf("[ERROR] Expected piece on rank 1 = row %d (GPIO %d) but detected on row %d (GPIO %d) which is not rank 1. Place piece on %s.\n", expectedRawPin, hwConfig.rowPins[expectedRawPin], actualPin, hwConfig.rowPins[actualPin], square);
        else
          Serial.printf("[ERROR] Expected piece on rank 1 = col %d (74HC595 Q%c, pin %d) but detected on col %d (74HC595 Q%c, pin %d) which is not rank 1. Place piece on %s.\n", expectedRawPin, shiftRegOutput(expectedRawPin), shiftRegPin(expectedRawPin), actualPin, shiftRegOutput(actualPin), shiftRegPin(actualPin), square);
        showCalibrationError();
        i--;
        continue;
      }
    }

    if (i == 0) {
      firstRow = row;
      firstCol = col;
      Serial.println("Remove the piece");
      waitForBoardEmpty();
      continue;
    }

    if (detectedAxis == UnknownAxis && i == 1) {
      if (row == firstRow && col != firstCol) {
        detectedAxis = ColsAxis;
        axisPinsOrder[firstCol] = i - 1;
        counts[firstCol]++;
        Serial.printf("%s calibration using cols %s\n", axisToChessRankFile(axis).c_str(), axis != detectedAxis ? "(axis swap)" : "(no axis swap)");
      } else if (col == firstCol && row != firstRow) {
        detectedAxis = RowsAxis;
        axisPinsOrder[firstRow] = i - 1;
        counts[firstRow]++;
        Serial.printf("%s calibration using rows %s\n", axisToChessRankFile(axis).c_str(), axis != detectedAxis ? "(axis swap)" : "(no axis swap)");
      } else {
        Serial.printf("\n============== AMBIGUOUS %s CALIBRATION ==============\n", axisToChessRankFile(axis).c_str());
        Serial.printf("First press:  row=%d (GPIO %d), col=%d (74HC595 Q%c, pin %d)\n", firstRow, hwConfig.rowPins[firstRow], firstCol, shiftRegOutput(firstCol), shiftRegPin(firstCol));
        Serial.printf("Second press: row=%d (GPIO %d), col=%d (74HC595 Q%c, pin %d)\n", row, hwConfig.rowPins[row], col, shiftRegOutput(col), shiftRegPin(col));
        Serial.printf("PROBLEM: %s\n", (row == firstRow && col == firstCol) ? "Both presses detected by the SAME sensor" : "Both row AND column changed between presses");
        Serial.println("==========================================================\n");
        showCalibrationError();
        i = -1;
        continue;
      }
    }

    if (detectedAxis == UnknownAxis) {
      // Will never happen due to above logic, but just in case
      Serial.printf("Ambiguous %s calibration (no orientation detected). Retry.\n", axisToChessRankFile(axis).c_str());
      showCalibrationError();
      i = -1;
      continue;
    }

    int pin = (detectedAxis == RowsAxis) ? row : col;
    if (counts[pin] > 0) {
      // Find what rank/file was already assigned to this pin
      int assignedIndex = axisPinsOrder[pin];
      char assignedRankFile[8];
      if (axis == RowsAxis)
        snprintf(assignedRankFile, sizeof(assignedRankFile), "rank %d", 8 - assignedIndex);
      else
        snprintf(assignedRankFile, sizeof(assignedRankFile), "file %c", 'a' + assignedIndex);
      if (detectedAxis == RowsAxis)
        Serial.printf("[ERROR] Row %d (GPIO %d) already has %s assigned. Retry %s.\n", pin, hwConfig.rowPins[pin], assignedRankFile, square);
      else
        Serial.printf("[ERROR] Col %d (74HC595 Q%c, pin %d) already has %s assigned. Retry %s.\n", pin, shiftRegOutput(pin), shiftRegPin(pin), assignedRankFile, square);
      showCalibrationError();
      i--;
      continue;
    }

    axisPinsOrder[pin] = i;
    counts[pin]++;

    Serial.println("Remove the piece");
    waitForBoardEmpty();
  }

  return axis != detectedAxis;
}

bool BoardDriver::runCalibration() {
  calibrating.store(true);
  // Calibration animation - light up each pixel sequentially
  for (int i = 0; i < LED_COUNT; i++) {
    strip->SetPixelColor(i, RgbColor(LedColors::White.r, LedColors::White.g, LedColors::White.b));
    showLEDs();
    delay(50);
  }
  delay(500);
  clearAllLEDs();

  Serial.println("========================== Board calibration required ==========================");
  Serial.println("- Connect to the AP and open the web interface to configure GPIO pins if needed");
  Serial.println("- Type 'skip' within 5 seconds to temporarily skip calibration (reboot to calibrate later)");
  Serial.println("  LEDs and sensors won't have correct mapping until calibration is completed");
  unsigned long startTime = millis();
  while (millis() - startTime < 5000) {
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      input.toLowerCase();
      if (input == "skip") {
        Serial.println("[SKIP] Calibration skipped - using default mapping");
        Serial.println("[SKIP] Sensors/LEDs will NOT work correctly!");
        Serial.println("[SKIP] You will be asked to calibrate again on next reboot");
        // Set up identity mapping (no calibration)
        swapAxes = 0;
        for (int i = 0; i < NUM_ROWS; i++) toLogicalRow[i] = i;
        for (int i = 0; i < NUM_COLS; i++) toLogicalCol[i] = i;
        for (int row = 0; row < NUM_ROWS; row++)
          for (int col = 0; col < NUM_COLS; col++)
            ledIndexMap[row][col] = row * NUM_COLS + col;
        calibrating.store(false);
        return true;
      } else {
        Serial.println("Unknown command \"" + input + "\" Type \"skip\" to skip calibration or wait 5 seconds for calibration to begin");
      }
    }
    delay(50);
  }
  Serial.println("");
  Serial.println("- Empty the board to begin calibration - instructions will follow once an empty board is detected");
  Serial.println("- WARNING: Low GPIO voltage can cause unreliable shift register behavior (74HC595 needs Vih > 0.7*Vcc) use a level shifter or HCT variant");
  Serial.println("- WARNING: Shift register outputs shouldn't power 8 sensors directly from 1 output pin, use transistors! (max 35mA per pin but each A3144 draws ~10mA");
  Serial.println("- WARNING: If powering multiple sensors from one shift register pin, expect voltage drop and shift register failure");
  Serial.println("- TIP: Try both magnet sides and move magnet closer if sensor doesn't trigger");
  Serial.println("================================================================================");
  waitForBoardEmpty();

  bool swapAxes1 = calibrateAxis(Axis::RowsAxis, toLogicalRow, NUM_ROWS, false);
  bool swapAxes2 = calibrateAxis(Axis::ColsAxis, toLogicalCol, NUM_COLS, swapAxes1);
  if (swapAxes1 != swapAxes2) {
    Serial.println("Inconsistent axis orientation detected during calibration. Restarting calibration.");
    showCalibrationError();
    return runCalibration();
  }
  swapAxes = swapAxes1 ? 1 : 0;

  // LED mapping calibration
  Serial.println("LED mapping calibration:");

  bool logicalUsed[NUM_ROWS][NUM_COLS] = {false};

  auto displayCalibrationLEDs = [&](int currentPixel) {
    for (int i = 0; i < LED_COUNT; i++)
      strip->SetPixelColor(i, RgbColor(0));
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++)
        if (logicalUsed[r][c])
          strip->SetPixelColor(ledIndexMap[r][c], RgbColor(LedColors::Green.r, LedColors::Green.g, LedColors::Green.b));
    if (currentPixel < LED_COUNT)
      strip->SetPixelColor(currentPixel, RgbColor(LedColors::White.r, LedColors::White.g, LedColors::White.b));
    showLEDs();
  };

  for (int pixelIndex = 0; pixelIndex < LED_COUNT; pixelIndex++) {
    int row = 0;
    int col = 0;
    displayCalibrationLEDs(pixelIndex);
    Serial.println("Place a piece on the white LED");
    waitForSingleRawPress(row, col);

    uint8_t logicalRow = toLogicalRow[swapAxes ? col : row];
    uint8_t logicalCol = toLogicalCol[swapAxes ? row : col];
    if (logicalUsed[logicalRow][logicalCol]) {
      Serial.printf("Duplicate square %c%c detected. Retry LED %d.\n", (char)('a' + logicalCol), (char)('8' - logicalRow), pixelIndex);
      showCalibrationError();
      pixelIndex--;
      continue;
    }
    logicalUsed[logicalRow][logicalCol] = true;
    ledIndexMap[logicalRow][logicalCol] = pixelIndex;
    Serial.printf("  LED %d -> %c%c\n", pixelIndex, (char)('a' + logicalCol), (char)('8' - logicalRow));

    displayCalibrationLEDs(pixelIndex + 1);

    Serial.println("Remove the piece");
    waitForBoardEmpty(100);
  }

  clearAllLEDs();
  Serial.println("Calibration complete");
  calibrating.store(false);
  return false;
}

void BoardDriver::loadShiftRegister(byte data, int bits) {
  if (hwConfig.srInvertOutputs)
    data = ~data;
  // Make sure latch is low before shifting data
  digitalWrite(hwConfig.srLatchPin, LOW);
  // Shift bits MSB first
  for (int i = bits - 1; i >= 0; i--) {
    digitalWrite(hwConfig.srDataPin, !!(data & (1 << i)));
    delayMicroseconds(10);
    digitalWrite(hwConfig.srClkPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(hwConfig.srClkPin, LOW);
    delayMicroseconds(10);
  }
  // Latch the data to output pins
  digitalWrite(hwConfig.srLatchPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(hwConfig.srLatchPin, LOW);
}

// === Hardware diagnostic methods ===

uint8_t BoardDriver::pullupSanityCheck() {
  disableAllCols();
  delayMicroseconds(50);
  uint8_t mask = 0;
  for (int r = 0; r < NUM_ROWS; r++)
    if (digitalRead(hwConfig.rowPins[r]) == HIGH)
      mask |= (1 << r);
  return mask;
}

bool BoardDriver::toggleSrInvert() {
  hwConfig.srInvertOutputs = !hwConfig.srInvertOutputs;
  saveHardwareConfig(hwConfig);
  disableAllCols();   // re-apply with new polarity
  return hwConfig.srInvertOutputs;
}

void BoardDriver::debugSrPattern(uint8_t pattern, bool hold) {
  if (!hold) {
    debugHeldCol.store(-1);   // resume scan
    disableAllCols();
    return;
  }
  // Pause the scan via the sentinel -2, then write the raw byte. The wait
  // is critical: without it an in-flight readSensors() can finish its
  // column-walk (and disableAllCols()) AFTER we've loaded our pattern,
  // overwriting it. Sleeping past one full main-loop tick (40 ms scan +
  // 40 ms pacing) guarantees any concurrent scan has observed the sentinel
  // and bailed before we touch the SR.
  debugHeldCol.store(-2);
  // One full readSensors() iteration is ~2 ms (8 cols × ~100 µs + reads).
  // 20 ms is 10× margin and far below human-perceivable lag, while keeping
  // the AsyncTCP task responsive to other concurrent HTTP requests.
  vTaskDelay(pdMS_TO_TICKS(20));
  loadShiftRegister(pattern, 8);
  lastEnabledCol = -2;
}

void BoardDriver::classifyPins(uint32_t durationMs, uint8_t out[8][8]) {
  uint16_t lowCount[NUM_ROWS][NUM_COLS] = {};
  uint16_t highCount[NUM_ROWS][NUM_COLS] = {};
  uint16_t transitions[NUM_ROWS][NUM_COLS] = {};
  bool prev[NUM_ROWS][NUM_COLS] = {};
  bool prevValid = false;
  uint32_t start = millis();
  while (millis() - start < durationMs) {
    bool raw[NUM_ROWS][NUM_COLS];
    readRawSensors(raw);
    for (int r = 0; r < NUM_ROWS; r++) {
      for (int c = 0; c < NUM_COLS; c++) {
        if (raw[r][c]) lowCount[r][c]++;
        else           highCount[r][c]++;
        if (prevValid && prev[r][c] != raw[r][c]) transitions[r][c]++;
        prev[r][c] = raw[r][c];
      }
    }
    prevValid = true;
    delay(15);
  }
  for (int r = 0; r < NUM_ROWS; r++) {
    for (int c = 0; c < NUM_COLS; c++) {
      uint16_t t = transitions[r][c];
      if (t > 30)                                 out[r][c] = 3;   // flicker
      else if (t > 0)                             out[r][c] = 2;   // toggled cleanly
      else if (lowCount[r][c] > 0 && highCount[r][c] == 0) out[r][c] = 1; // stuck LOW
      else                                        out[r][c] = 0;   // stuck HIGH / idle
    }
  }
}

void BoardDriver::phantomLogStart() {
  phantomHead = phantomTail = 0;
  for (int r = 0; r < NUM_ROWS; r++)
    for (int c = 0; c < NUM_COLS; c++)
      phantomLast[r][c] = false;
  phantomDeadlineMs = millis() + 30000;
  phantomActive.store(true);
}

size_t BoardDriver::phantomLogFetch(PhantomEntry* out, size_t maxN) {
  size_t n = 0;
  while (n < maxN && phantomTail != phantomHead) {
    out[n++] = phantomBuf[phantomTail];
    phantomTail = (uint16_t)((phantomTail + 1) % PHANTOM_CAP);
  }
  return n;
}

void BoardDriver::phantomTick() {
  if (!phantomActive.load()) return;
  if ((int32_t)(millis() - phantomDeadlineMs) >= 0) {
    phantomActive.store(false);
    return;
  }
  bool raw[NUM_ROWS][NUM_COLS];
  readRawSensors(raw);
  uint32_t now = millis();
  for (int r = 0; r < NUM_ROWS; r++) {
    for (int c = 0; c < NUM_COLS; c++) {
      if (raw[r][c] != phantomLast[r][c]) {
        uint16_t nextHead = (uint16_t)((phantomHead + 1) % PHANTOM_CAP);
        if (nextHead != phantomTail) {
          phantomBuf[phantomHead] = PhantomEntry{now, (uint8_t)r, (uint8_t)c, raw[r][c]};
          phantomHead = nextHead;
        }
        phantomLast[r][c] = raw[r][c];
      }
    }
  }
}

void BoardDriver::ledWalkTest() {
  acquireLEDs();
  clearAllLEDs(false);
  showLEDs();
  for (int row = 0; row < NUM_ROWS; row++) {
    for (int col = 0; col < NUM_COLS; col++) {
      currentColors[row][col] = LedRGB{255, 255, 255};
      strip->SetPixelColor(getPixelIndex(row, col), RgbColor(255, 255, 255));
      showLEDs();
      delay(140);
      currentColors[row][col] = LedColors::Off;
      strip->SetPixelColor(getPixelIndex(row, col), RgbColor(0, 0, 0));
    }
  }
  showLEDs();
  releaseLEDs();
}

void BoardDriver::ledRgbSequenceTest() {
  acquireLEDs();
  auto fill = [&](uint8_t r, uint8_t g, uint8_t b) {
    for (int row = 0; row < NUM_ROWS; row++)
      for (int col = 0; col < NUM_COLS; col++) {
        currentColors[row][col] = LedRGB{r, g, b};
        strip->SetPixelColor(getPixelIndex(row, col), RgbColor(r, g, b));
      }
    showLEDs();
  };
  fill(255, 0, 0);   delay(700);
  fill(0, 255, 0);   delay(700);
  fill(0, 0, 255);   delay(700);
  fill(255, 255, 255); delay(700);
  for (int step = 1; step <= 8; step++) {
    uint8_t v = (uint8_t)(step * 32 - 1);
    fill(0, v, v);
    delay(350);
  }
  fill(0, 0, 0);
  releaseLEDs();
}

void BoardDriver::disableAllCols() {
  if (lastEnabledCol == 7) {
    // Sequential wrap-around: shift in a single 0 to push the 1 out of QH
    loadShiftRegister(0x00, 1);
  } else {
    // Non-sequential or startup: load full byte of zeros
    loadShiftRegister(0);
  }
  lastEnabledCol = -1; // Make next enableCol(0) call use optimized 1-bit shift because register is now all zeros
}

void BoardDriver::enableCol(int col) {
  if (col == lastEnabledCol + 1) {
    if (col == 0)
      loadShiftRegister(0x01, 1); // Sequential wrap-around: register should be all zeros already, shift in a single 1 bit into QA
    else
      loadShiftRegister(0x00, 1); // Sequential access: shift in a single 0 bit to move the 1 we shifted earlier to the next column position (towards QH)
  } else {
    // Due to above logic, this condition should never occur, but just in case...
    loadShiftRegister((byte)(1 << col));
  }
  lastEnabledCol = col;
  delayMicroseconds(100); // Allow time for the column to stabilize, otherwise random readings might occur
}

void BoardDriver::debugHoldCol(int col) {
  if (col < 0 || col >= NUM_COLS) {
    disableAllCols();
    debugHeldCol.store(-1);
  } else {
    lastEnabledCol = -2; // force full-byte load
    enableCol(col);
    debugHeldCol.store(col);
  }
}

void BoardDriver::readSensors() {
  static bool debugPrinted = false;
  if (!debugPrinted) {
    debugPrinted = true;
    Serial.printf("[SR_DEBUG] latch=%d clk=%d data=%d invert=%d rows=%d %d %d %d\n",
      hwConfig.srLatchPin, hwConfig.srClkPin, hwConfig.srDataPin, hwConfig.srInvertOutputs,
      hwConfig.rowPins[0], hwConfig.rowPins[1], hwConfig.rowPins[2], hwConfig.rowPins[3]);
  }

  // Debug-Modus: eine Spalte dauerhaft aktiv halten, Scan überspringen
  int held = debugHeldCol.load();
  // -2 = raw SR pattern held via debugSrPattern → don't touch SR at all
  if (held == -2) return;
  if (held >= 0) {
    lastEnabledCol = -2;
    enableCol(held);
    // Trotzdem Zeilenwerte einlesen damit das Debug-Grid aktuell bleibt
    unsigned long currentTime = millis();
    for (int row = 0; row < NUM_ROWS; row++) {
      bool v = digitalRead(hwConfig.rowPins[row]) == LOW;
      sensorRaw[row][held] = v;
      sensorState[row][held] = v;
    }
    return;
  }

  unsigned long currentTime = millis();

  for (int col = 0; col < NUM_COLS; col++) {
    enableCol(col);
    for (int row = 0; row < NUM_ROWS; row++) {
      bool newReading = digitalRead(hwConfig.rowPins[row]) == LOW;
      uint8_t logicalRow = toLogicalRow[swapAxes ? col : row];
      uint8_t logicalCol = toLogicalCol[swapAxes ? row : col];
      // Debounce logic
      if (newReading != sensorState[logicalRow][logicalCol]) {
        if (newReading != sensorRaw[logicalRow][logicalCol]) {
          sensorRaw[logicalRow][logicalCol] = newReading;
          sensorDebounceTime[logicalRow][logicalCol] = currentTime;
        } else if (currentTime - sensorDebounceTime[logicalRow][logicalCol] >= DEBOUNCE_MS) {
          sensorState[logicalRow][logicalCol] = newReading;
        }
      } else {
        sensorRaw[logicalRow][logicalCol] = newReading;
        sensorDebounceTime[logicalRow][logicalCol] = currentTime;
      }
    }
  }
  disableAllCols();
}

bool BoardDriver::getSensorState(int row, int col) {
  return sensorState[row][col];
}

bool BoardDriver::getSensorPrev(int row, int col) {
  return sensorPrev[row][col];
}

void BoardDriver::updateSensorPrev() {
  for (int row = 0; row < NUM_ROWS; row++)
    for (int col = 0; col < NUM_COLS; col++)
      sensorPrev[row][col] = sensorState[row][col];
}

int BoardDriver::getPixelIndex(int row, int col) {
  return ledIndexMap[row][col];
}

void BoardDriver::acquireLEDs() {
  xSemaphoreTake(ledMutex, portMAX_DELAY);
}

void BoardDriver::releaseLEDs() {
  xSemaphoreGive(ledMutex);
}

void BoardDriver::clearAllLEDs(bool show) {
  for (int row = 0; row < NUM_ROWS; row++)
    for (int col = 0; col < NUM_COLS; col++)
      currentColors[row][col] = LedColors::Off;
  for (int i = 0; i < LED_COUNT; i++)
    strip->SetPixelColor(i, RgbColor(0));
  if (show)
    showLEDs();
}

void BoardDriver::setSquareLED(int row, int col, LedRGB color) {
  if (row < 0 || row >= NUM_ROWS || col < 0 || col >= NUM_COLS) return;
  currentColors[row][col] = color; // Track the intended color
  float multiplier = 1.0f;
  if ((row + col) % 2 == 0)
    multiplier = dimMultiplier / 100.0f; // Dim dark squares (parity matched to this board's orientation)
  strip->SetPixelColor(getPixelIndex(row, col), RgbColor(color.r * multiplier, color.g * multiplier, color.b * multiplier));
}

void BoardDriver::showLEDs() {
  strip->Show();
}

void BoardDriver::loadHighlightColors() {
  if (!ChessUtils::ensureNvsInitialized()) return;
  Preferences prefs;
  prefs.begin("hlColors", true);
  uint8_t b[15];
  if (prefs.isKey("v") && prefs.getBytes("v", b, sizeof(b)) == sizeof(b)) {
    hlSource  = {b[0],  b[1],  b[2]};
    hlTarget  = {b[3],  b[4],  b[5]};
    hlCapture = {b[6],  b[7],  b[8]};
    hlValid   = {b[9],  b[10], b[11]};
    hlInvalid = {b[12], b[13], b[14]};
  }
  prefs.end();
}

void BoardDriver::setHighlightColors(LedRGB s, LedRGB t, LedRGB c, LedRGB v, LedRGB i) {
  hlSource = s; hlTarget = t; hlCapture = c; hlValid = v; hlInvalid = i;
  if (!ChessUtils::ensureNvsInitialized()) return;
  Preferences prefs;
  prefs.begin("hlColors", false);
  uint8_t b[15] = { s.r, s.g, s.b, t.r, t.g, t.b, c.r, c.g, c.b, v.r, v.g, v.b, i.r, i.g, i.b };
  prefs.putBytes("v", b, sizeof(b));
  prefs.end();
}

void BoardDriver::showConnectingAnimation() {
  acquireLEDs();
  // Save current LED state
  LedRGB savedColors[NUM_ROWS][NUM_COLS];
  memcpy(savedColors, currentColors, sizeof(currentColors));
  // Show each WiFi connection attempt with animated LEDs
  for (int i = 0; i < NUM_COLS; i++) {
    setSquareLED(NUM_ROWS / 2 - 1, i, LedColors::Blue);
    setSquareLED(NUM_ROWS / 2,     i, LedColors::Blue);
    showLEDs();
    delay(125);
  }
  // Restore previous LED state
  for (int r = 0; r < NUM_ROWS; r++)
    for (int c = 0; c < NUM_COLS; c++)
      setSquareLED(r, c, savedColors[r][c]);
  showLEDs();
  releaseLEDs();
}

void BoardDriver::showBootMessage(const char* msg, LedRGB color, uint16_t stepMs, uint8_t repeats) {
  // Blocking scroll of a short status message. Runs on the caller's thread
  // (setup()/loop()), NOT via the animation queue — the worker task is not
  // guaranteed to be servicing jobs during boot. Mirrors the scroll logic of
  // doText() but terminates after the text leaves the right edge. Boot status
  // (esp. error) messages scroll slowly and repeat a few times so they can
  // actually be read — a single fast pass flew by before the user could.
  if (!msg) return;
  if (repeats < 1) repeats = 1;
  acquireLEDs();
  const int CW = 6;                                 // 5px glyph + 1px spacing
  int len = (int)strlen(msg);
  int totalW = len * CW;
  if (totalW < 1) { releaseLEDs(); return; }
  for (uint8_t pass = 0; pass < repeats; pass++) {
    // Scroll from fully off the right edge until the last column clears the left.
    for (int sx = -NUM_COLS; sx <= totalW; sx++) {
      for (int c = 0; c < NUM_COLS; c++) {
        int srcCol = c + sx;
        bool on = false;
        if (srcCol >= 0 && srcCol < totalW) {
          int colInChar = srcCol % CW;
          if (colInChar < 5) {
            uint8_t bits = TEXT_FONT[textGlyphIndex(msg[srcCol / CW])][colInChar];
            // (row check folded into the loop below)
            for (int r = 0; r < NUM_ROWS; r++)
              currentColors[r][c] = (r < 7 && (bits & (1 << r))) ? color : LedRGB{0, 0, 0};
            on = true;
          }
        }
        if (!on)
          for (int r = 0; r < NUM_ROWS; r++)
            currentColors[r][c] = LedRGB{0, 0, 0};
        for (int r = 0; r < NUM_ROWS; r++) {
          LedRGB px = currentColors[r][c];
          strip->SetPixelColor(getPixelIndex(r, c), RgbColor(px.r, px.g, px.b));
        }
      }
      showLEDs();
      delay(stepMs);
    }
    if (pass + 1 < repeats) delay(stepMs * 4);   // brief gap between passes
  }
  clearAllLEDs(false);
  showLEDs();
  releaseLEDs();
}

void BoardDriver::pulseUserConnected() {
  // Brief, non-destructive green double-pulse signalling that a device joined
  // the board's AP. Saves and restores the current LED state so an existing
  // display (e.g. the game-selection LEDs) survives. Runs on the caller's
  // thread — must NOT be called from the WiFi event task directly.
  acquireLEDs();
  LedRGB savedColors[NUM_ROWS][NUM_COLS];
  memcpy(savedColors, currentColors, sizeof(currentColors));
  for (int pulse = 0; pulse < 2; pulse++) {
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++)
        setSquareLED(r, c, LedColors::Green);
    showLEDs();
    delay(120);
    clearAllLEDs(false);
    showLEDs();
    delay(100);
  }
  for (int r = 0; r < NUM_ROWS; r++)
    for (int c = 0; c < NUM_COLS; c++)
      setSquareLED(r, c, savedColors[r][c]);
  showLEDs();
  releaseLEDs();
}

void BoardDriver::blinkSquare(int row, int col, LedRGB color, int times, bool clearAfter) {
  AnimationJob job = {AnimationType::BLINK, nullptr, {}};
  job.params.blink = {row, col, color, times, clearAfter};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
}

void BoardDriver::doBlink(int row, int col, LedRGB color, int times, bool clearAfter) {
  for (int i = 0; i < times; i++) {
    setSquareLED(row, col, color);
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(200));
    setSquareLED(row, col, LedColors::Off);
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (!clearAfter) {
    setSquareLED(row, col, color);
    showLEDs();
  }
}

void BoardDriver::fireworkAnimation(LedRGB color) {
  AnimationJob job = {AnimationType::FIREWORK, nullptr, {}};
  job.params.firework = {color};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
}

void BoardDriver::doFirework(LedRGB color) {
  clearAllLEDs(false);
  float centerX = (NUM_COLS - 1) / 2.0f;
  float centerY = (NUM_ROWS - 1) / 2.0f;
  float maxRadius = sqrtf(centerX * centerX + centerY * centerY) + 1.0f;

  // Contraction phase:
  for (float radius = maxRadius; radius > 0; radius -= 0.5) {
    for (int row = 0; row < NUM_ROWS; row++)
      for (int col = 0; col < NUM_COLS; col++) {
        float dx = col - centerX;
        float dy = row - centerY;
        float dist = sqrt(dx * dx + dy * dy);
        if (fabs(dist - radius) < 0.5)
          setSquareLED(row, col, color);
        else
          setSquareLED(row, col, LedColors::Off);
      }
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  // Expansion phase:
  for (float radius = 0; radius < maxRadius; radius += 0.5) {
    for (int row = 0; row < NUM_ROWS; row++)
      for (int col = 0; col < NUM_COLS; col++) {
        float dx = col - centerX;
        float dy = row - centerY;
        float dist = sqrt(dx * dx + dy * dy);
        if (fabs(dist - radius) < 0.5)
          setSquareLED(row, col, color);
        else
          setSquareLED(row, col, LedColors::Off);
      }
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  clearAllLEDs();
}

void BoardDriver::captureAnimation(int row, int col) {
  AnimationJob job = {AnimationType::CAPTURE, nullptr, {}};
  job.params.capture = {row, col};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
}

void BoardDriver::checkBorderFlash() {
  AnimationJob job = {AnimationType::CHECK_BORDER, nullptr, {}};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
}

void BoardDriver::doCheckBorderFlash() {
  // Three red flashes of the 28 border LEDs to signal "check", overlaid on
  // top of whatever LED state is currently showing (the pieces in
  // simulation, the highlight grid in remote play, etc.). On the OFF phase
  // we restore the saved base so the underlying picture stays continuous.
  LedRGB base[NUM_ROWS][NUM_COLS];
  for (int r = 0; r < NUM_ROWS; r++)
    for (int c = 0; c < NUM_COLS; c++)
      base[r][c] = currentColors[r][c];
  auto restoreBase = [&]() {
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++)
        setSquareLED(r, c, base[r][c]);
  };
  // Direct strip write for the border ON phase so dim-mask + brightness
  // don't gut the alert.
  auto paintBorderRed = [&]() {
    for (int row = 0; row < NUM_ROWS; row++)
      for (int col = 0; col < NUM_COLS; col++) {
        bool border = (row == 0 || row == NUM_ROWS - 1 || col == 0 || col == NUM_COLS - 1);
        if (border) {
          currentColors[row][col] = LedRGB{255, 0, 0};
          strip->SetPixelColor(getPixelIndex(row, col), RgbColor(255, 0, 0));
        } else {
          setSquareLED(row, col, base[row][col]);   // inner squares = base picture
        }
      }
  };
  for (int i = 0; i < 3; i++) {
    paintBorderRed();
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(170));
    restoreBase();
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(110));
  }
  restoreBase();
  showLEDs();
}

void BoardDriver::checkmateSpiral() {
  AnimationJob job = {AnimationType::CHECKMATE_SPIRAL, nullptr, {}};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
}

void BoardDriver::checkmateAnimation(const char board[8][8], char winner, LedRGB winnerColor, LedRGB loserColor, const char* winnerName) {
  // Stash params in class storage — the params union doesn't have room for
  // an 8×8 board, so we copy in here and the worker reads it back later.
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      cmBoardState[r][c] = board[r][c];
  cmWinner = winner;
  cmWinnerColor = winnerColor;
  cmLoserColor = loserColor;
  cmWinnerName[0] = '\0';
  if (winnerName && *winnerName) {
    strncpy(cmWinnerName, winnerName, sizeof(cmWinnerName) - 1);
    cmWinnerName[sizeof(cmWinnerName) - 1] = '\0';
  }
  AnimationJob job = {AnimationType::CHECKMATE_ANIM, nullptr, {}};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
}

void BoardDriver::doCheckmateAnimation() {
  endgameAbort.store(false);
  auto aborted = [&]() {
    return endgameAbort.load() || uxQueueMessagesWaiting(animationQueue) > 0;
  };
  // Phase 1 — every piece of the LOSING side blinks red three times. Winner
  // pieces stay dark for contrast; empty squares stay dark.
  auto isLoserPiece = [&](char p) -> bool {
    if (p == ' ' || p == '\0') return false;
    bool isWhite = (p >= 'A' && p <= 'Z');
    return (cmWinner == 'w') ? !isWhite : isWhite;
  };
  for (int i = 0; i < 3; i++) {
    if (aborted()) return;
    for (int r = 0; r < NUM_ROWS; r++) {
      for (int c = 0; c < NUM_COLS; c++) {
        if (isLoserPiece(cmBoardState[r][c])) {
          currentColors[r][c] = LedRGB{255, 0, 0};
          strip->SetPixelColor(getPixelIndex(r, c), RgbColor(255, 0, 0));
        } else {
          currentColors[r][c] = LedColors::Off;
          strip->SetPixelColor(getPixelIndex(r, c), RgbColor(0, 0, 0));
        }
      }
    }
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(230));
    clearAllLEDs(false);
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(160));
  }
  if (aborted()) return;

  // Phase 2 — fill the board rank-by-rank in the winner's colour, starting
  // from the winner's back rank and washing across to the loser's side. The
  // sweep direction encodes "the winner overruns the board".
  int startRow = (cmWinner == 'w') ? NUM_ROWS - 1 : 0;
  int step     = (cmWinner == 'w') ? -1 : 1;
  for (int rank = 0; rank < NUM_ROWS; rank++) {
    if (aborted()) return;
    int r = startRow + step * rank;
    for (int c = 0; c < NUM_COLS; c++) {
      currentColors[r][c] = cmWinnerColor;
      strip->SetPixelColor(getPixelIndex(r, c), RgbColor(cmWinnerColor.r, cmWinnerColor.g, cmWinnerColor.b));
    }
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(130));
  }
  // Hold the full winner-coloured board so the win lands visually.
  vTaskDelay(pdMS_TO_TICKS(900));

  // Phase 3 — outside-in red spiral on top of the winner colour. Each cell
  // jumps to red as the snail visits it; trail stays painted.
  {
    int top = 0, bottom = NUM_ROWS - 1, left = 0, right = NUM_COLS - 1;
    while (top <= bottom && left <= right) {
      for (int c = left; c <= right; c++) {
        currentColors[top][c] = LedRGB{255, 0, 0};
        strip->SetPixelColor(getPixelIndex(top, c), RgbColor(255, 0, 0));
        showLEDs();
        if (aborted()) return;
        vTaskDelay(pdMS_TO_TICKS(30));
      }
      top++;
      for (int r = top; r <= bottom; r++) {
        currentColors[r][right] = LedRGB{255, 0, 0};
        strip->SetPixelColor(getPixelIndex(r, right), RgbColor(255, 0, 0));
        showLEDs();
        if (aborted()) return;
        vTaskDelay(pdMS_TO_TICKS(30));
      }
      right--;
      if (top <= bottom) {
        for (int c = right; c >= left; c--) {
          currentColors[bottom][c] = LedRGB{255, 0, 0};
          strip->SetPixelColor(getPixelIndex(bottom, c), RgbColor(255, 0, 0));
          showLEDs();
          vTaskDelay(pdMS_TO_TICKS(30));
        }
        bottom--;
      }
      if (left <= right) {
        for (int r = bottom; r >= top; r--) {
          currentColors[r][left] = LedRGB{255, 0, 0};
          strip->SetPixelColor(getPixelIndex(r, left), RgbColor(255, 0, 0));
          showLEDs();
          vTaskDelay(pdMS_TO_TICKS(30));
        }
        left++;
      }
    }
  }
  vTaskDelay(pdMS_TO_TICKS(400));

  // Phase 4 — three full-board red blinks.
  for (int i = 0; i < 3; i++) {
    if (aborted()) return;
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++) {
        currentColors[r][c] = LedColors::Off;
        strip->SetPixelColor(getPixelIndex(r, c), RgbColor(0, 0, 0));
      }
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(140));
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++) {
        currentColors[r][c] = LedRGB{255, 0, 0};
        strip->SetPixelColor(getPixelIndex(r, c), RgbColor(255, 0, 0));
      }
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(180));
  }
  // All off.
  clearAllLEDs(false);
  showLEDs();
  vTaskDelay(pdMS_TO_TICKS(350));

  // Phase 5 — scroll "<winnerName> gewinnt" once across the board in the
  // winner colour. Uses the 5×7 column-major font already defined in doText
  // (kept inline here to avoid duplicating the font table).
  {
    static const uint8_t CM_FONT[][5] = {
      {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
      {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
      {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
      {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
      {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
      {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
      {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
      {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
      {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
      {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
      {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
      {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
      {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
      {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
      {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
    };
    const int GLYPH_COUNT = (int)(sizeof(CM_FONT) / 5);
    auto gIdx = [&](char ch) -> int {
      if (ch >= 'a' && ch <= 'z') ch -= 32;
      int idx = (int)ch - 0x20;
      return (idx >= 0 && idx < GLYPH_COUNT) ? idx : 0;
    };
    char msg[64];
    const char* name = cmWinnerName[0] ? cmWinnerName : ((cmWinner == 'w') ? "WEISS" : "SCHWARZ");
    snprintf(msg, sizeof(msg), "%s GEWINNT", name);
    int msgLen = (int)strlen(msg);
    int totalCols = msgLen * 6 + NUM_COLS;   // 5+1 per char, plus a board's worth of trailing gap
    LedRGB fg = cmWinnerColor;
    for (int offset = 0; offset < totalCols; offset++) {
      for (int c = 0; c < NUM_COLS; c++) {
        int srcCol = offset + c - NUM_COLS;     // start off-screen right, scroll left
        uint8_t bits = 0;
        if (srcCol >= 0 && srcCol < msgLen * 6) {
          int charIdx = srcCol / 6;
          int colInChar = srcCol % 6;
          if (colInChar < 5) bits = CM_FONT[gIdx(msg[charIdx])][colInChar];
        }
        for (int r = 0; r < NUM_ROWS; r++) {
          bool on = r < 7 && (bits & (1 << r));
          LedRGB col = on ? fg : LedColors::Off;
          currentColors[r][c] = col;
          strip->SetPixelColor(getPixelIndex(r, c), RgbColor(col.r, col.g, col.b));
        }
      }
      showLEDs();
      if (aborted()) { clearAllLEDs(false); showLEDs(); return; }
      vTaskDelay(pdMS_TO_TICKS(170));   // slow enough to actually read
    }
  }
  clearAllLEDs(false);
  showLEDs();
  (void)cmLoserColor;
}

void BoardDriver::modeChangeTransition() {
  AnimationJob job = {AnimationType::MODE_CHANGE_TV, nullptr, {}};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
}

void BoardDriver::doModeChangeTransition() {
  // TV-style off → on between animations: outside-in fade to black, then
  // inside-out cyan reveal that settles to black ready for the next anim to
  // take over.
  LedRGB base[NUM_ROWS][NUM_COLS];
  for (int r = 0; r < NUM_ROWS; r++)
    for (int c = 0; c < NUM_COLS; c++)
      base[r][c] = currentColors[r][c];

  const float cx = (NUM_COLS - 1) * 0.5f;
  const float cy = (NUM_ROWS - 1) * 0.5f;
  const float maxR = sqrtf(cx * cx + cy * cy) + 1.0f;

  auto writeDirect = [&](int r, int c, uint8_t R, uint8_t G, uint8_t B) {
    currentColors[r][c] = LedRGB{R, G, B};
    strip->SetPixelColor(getPixelIndex(r, c), RgbColor(R, G, B));
  };

  // Phase 1 — OFF: shrink the lit area from the edges to the centre. Cells
  // outside the shrinking circle go dark; cells inside hold their previous
  // colour. The result reads as "the picture pulls into the centre and out".
  for (float radius = maxR; radius > -0.5f; radius -= 0.55f) {
    for (int r = 0; r < NUM_ROWS; r++) {
      for (int c = 0; c < NUM_COLS; c++) {
        float dx = c - cx, dy = r - cy;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist > radius)
          writeDirect(r, c, 0, 0, 0);
        else
          setSquareLED(r, c, base[r][c]);
      }
    }
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(28));
  }

  // Brief black gap so the off→on transition reads cleanly.
  vTaskDelay(pdMS_TO_TICKS(60));

  // Phase 2 — ON: expand a cyan glow from the centre outward, then fade it
  // back to black so the next animation starts on a clean slate.
  const LedRGB onColor = {60, 220, 255};
  for (float radius = 0.0f; radius < maxR + 1.0f; radius += 0.55f) {
    for (int r = 0; r < NUM_ROWS; r++) {
      for (int c = 0; c < NUM_COLS; c++) {
        float dx = c - cx, dy = r - cy;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist <= radius) {
          float edge = radius - dist;     // how deep inside the circle we are
          float bri = (edge > 2.0f) ? 1.0f : (0.35f + 0.65f * (edge / 2.0f));
          if (bri > 1.0f) bri = 1.0f;
          writeDirect(r, c,
            (uint8_t)(onColor.r * bri),
            (uint8_t)(onColor.g * bri),
            (uint8_t)(onColor.b * bri));
        } else {
          writeDirect(r, c, 0, 0, 0);
        }
      }
    }
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(28));
  }
  // Settle on full-bright cyan briefly before the fade-out.
  vTaskDelay(pdMS_TO_TICKS(80));
  for (int step = 0; step < 8; step++) {
    float fade = 1.0f - (float)(step + 1) / 8.0f;
    uint8_t R = (uint8_t)(onColor.r * fade);
    uint8_t G = (uint8_t)(onColor.g * fade);
    uint8_t B = (uint8_t)(onColor.b * fade);
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++)
        writeDirect(r, c, R, G, B);
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(32));
  }
  clearAllLEDs();
}

void BoardDriver::doCheckmateSpiral() {
  // "Schnecke" pattern — walk the 64 squares in a classic outside-in spiral
  // and light each one red, leaving the trail behind. Ends with the entire
  // board solid red, held for a beat so the player feels the loss.
  clearAllLEDs(false);
  showLEDs();
  int top = 0, bottom = NUM_ROWS - 1, left = 0, right = NUM_COLS - 1;
  while (top <= bottom && left <= right) {
    for (int c = left; c <= right; c++) {
      setSquareLED(top, c, LedColors::Red);
      showLEDs();
      vTaskDelay(pdMS_TO_TICKS(40));
    }
    top++;
    for (int r = top; r <= bottom; r++) {
      setSquareLED(r, right, LedColors::Red);
      showLEDs();
      vTaskDelay(pdMS_TO_TICKS(40));
    }
    right--;
    if (top <= bottom) {
      for (int c = right; c >= left; c--) {
        setSquareLED(bottom, c, LedColors::Red);
        showLEDs();
        vTaskDelay(pdMS_TO_TICKS(40));
      }
      bottom--;
    }
    if (left <= right) {
      for (int r = bottom; r >= top; r--) {
        setSquareLED(r, left, LedColors::Red);
        showLEDs();
        vTaskDelay(pdMS_TO_TICKS(40));
      }
      left++;
    }
  }
  vTaskDelay(pdMS_TO_TICKS(1500));
}

void BoardDriver::cellPulse(int row, int col, LedRGB color) {
  AnimationJob job = {AnimationType::CELL_PULSE, nullptr, {}};
  job.params.blink = {row, col, color, 0, false};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
}

void BoardDriver::doCellPulse(int row, int col, LedRGB color) {
  // Soft up-down pulse on a freshly-placed cell — two ramps from base →
  // bright → base, leaving the underlying piece colour intact.
  LedRGB orig = currentColors[row][col];
  const int STEPS = 8;
  for (int phase = 0; phase < 2; phase++) {
    // Ramp up to bright
    for (int s = 1; s <= STEPS; s++) {
      float t = (float)s / STEPS;
      uint8_t cr = (uint8_t)(orig.r + ((int)color.r - (int)orig.r) * t);
      uint8_t cg = (uint8_t)(orig.g + ((int)color.g - (int)orig.g) * t);
      uint8_t cb = (uint8_t)(orig.b + ((int)color.b - (int)orig.b) * t);
      currentColors[row][col] = LedRGB{cr, cg, cb};
      strip->SetPixelColor(getPixelIndex(row, col), RgbColor(cr, cg, cb));
      showLEDs();
      vTaskDelay(pdMS_TO_TICKS(14));
    }
    // Ramp back down to orig
    for (int s = STEPS - 1; s >= 0; s--) {
      float t = (float)s / STEPS;
      uint8_t cr = (uint8_t)(orig.r + ((int)color.r - (int)orig.r) * t);
      uint8_t cg = (uint8_t)(orig.g + ((int)color.g - (int)orig.g) * t);
      uint8_t cb = (uint8_t)(orig.b + ((int)color.b - (int)orig.b) * t);
      currentColors[row][col] = LedRGB{cr, cg, cb};
      strip->SetPixelColor(getPixelIndex(row, col), RgbColor(cr, cg, cb));
      showLEDs();
      vTaskDelay(pdMS_TO_TICKS(14));
    }
  }
}

void BoardDriver::movePathAnimation(int fromRow, int fromCol, int toRow, int toCol, char player) {
  AnimationJob job = {AnimationType::MOVE_PATH, nullptr, {}};
  job.params.path = {fromRow, fromCol, toRow, toCol, player};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
}

void BoardDriver::doMovePath(int fr, int fc, int tr, int tc) {
  // Animate the piece's travel from source → target. The trail is overlaid
  // on top of whatever LED state is currently showing — pieces, highlights,
  // whatever — so the board never goes dark beneath it.
  int dr = tr - fr, dc = tc - fc;
  int adr = abs(dr), adc = abs(dc);
  if (adr == 0 && adc == 0) return;
  bool isKnight = (adr == 1 && adc == 2) || (adr == 2 && adc == 1);
  int pathR[10], pathC[10], pathLen = 0;
  if (isKnight) {
    int cornerR = (adr > adc) ? tr : fr;
    int cornerC = (adr > adc) ? fc : tc;
    pathR[pathLen] = fr;      pathC[pathLen++] = fc;
    pathR[pathLen] = cornerR; pathC[pathLen++] = cornerC;
    pathR[pathLen] = tr;      pathC[pathLen++] = tc;
  } else {
    int steps = (adr > adc) ? adr : adc;
    int sr = (dr > 0) - (dr < 0);
    int sc = (dc > 0) - (dc < 0);
    for (int i = 0; i <= steps && pathLen < 10; i++) {
      pathR[pathLen] = fr + sr * i;
      pathC[pathLen] = fc + sc * i;
      pathLen++;
    }
  }
  float dim = getAutoDimFactor();

  // Snapshot the current LED state so we can overlay the trail without
  // wiping the picture behind it.
  LedRGB base[NUM_ROWS][NUM_COLS];
  for (int r = 0; r < NUM_ROWS; r++)
    for (int c = 0; c < NUM_COLS; c++)
      base[r][c] = currentColors[r][c];
  auto restoreBase = [&]() {
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++)
        setSquareLED(r, c, base[r][c]);
  };

  // Per-player styling: trail colour + speed multiplier from user settings.
  // effectivePathColor honours sim's blue/green override when active.
  LedRGB trailColor = effectivePathColor(mphPlayer);
  float speed = (mphPlayer == 'b') ? animChessSpeedB : animChessSpeedW;
  if (speed < 0.25f) speed = 0.25f;
  if (speed > 3.0f)  speed = 3.0f;
  // Per-frame pacing. Per-cell delay is capped at 60 ms so a single hop
  // doesn't drag — the user's complaint was "source is lit way too long".
  // Long paths still fit inside the total walkBudgetMs/speed window.
  uint32_t totalBudget = (uint32_t)((float)walkBudgetMs / speed);
  uint32_t frameDelay = totalBudget / (pathLen > 0 ? pathLen : 1);
  if (frameDelay < 22) frameDelay = 22;
  if (frameDelay > 60) frameDelay = 60;

  for (int i = 0; i < pathLen; i++) {
    restoreBase();
    for (int j = 0; j <= i; j++) {
      float depth = (float)(i - j) / (float)pathLen;   // 0 = head, 1 = tail
      // Squared falloff so the SOURCE (depth high) drops to near-zero quickly
      // — head stays bright (depth=0 → 1.0), but cells behind it dim much
      // faster than a linear gradient. Source visibility shrinks from
      // "lit the whole walk" to "brief flash on pickup, then gone".
      float f = (1.0f - depth);
      float bri = f * f * 0.95f + 0.08f;
      if (bri > 1.0f) bri = 1.0f;
      bri *= dim;
      uint8_t cR = (uint8_t)(trailColor.r * bri);
      uint8_t cG = (uint8_t)(trailColor.g * bri);
      uint8_t cB = (uint8_t)(trailColor.b * bri);
      setSquareLED(pathR[j], pathC[j], LedRGB{cR, cG, cB});
    }
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(frameDelay));
  }
  // Tail fade-out: the SOURCE square dies FIRST (it's the farthest from the
  // target — the piece has visibly left it), then each successive cell, and
  // finally the destination. Using `j` directly as the stagger order means
  // source (j=0) gets progress=f starting at frame 0, while the destination
  // (j=pathLen-1) only starts fading at frame pathLen-1. Net effect: when
  // the after-move cellPulse on the destination kicks in, the source has
  // long been dark.
  // Per-cell tail fade: source (j=0) fades in 4 frames, target (j=pathLen-1)
  // fades in ~8 frames — source is visibly the FASTEST off, then each
  // successive cell takes longer. Per-cell start frame stays j so the order
  // is preserved. tailFrameDelay 12 ms ≈ 80 fps for the smoothest gradient.
  uint32_t tailFrameDelay = 12;
  // Source fades in 2 frames (24 ms) — by the time the head reaches the
  // target the source already had a quadratic-falloff dim during the walk,
  // so a brief tail finish is enough.
  int srcWindow = 2;
  int tgtWindow = 6;
  auto windowFor = [&](int j) {
    if (pathLen <= 1) return srcWindow;
    return srcWindow + ((tgtWindow - srcWindow) * j) / (pathLen - 1);
  };
  int tailFrames = (pathLen - 1) + windowFor(pathLen - 1);
  // Per-cell starting brightness = the walk's last-frame brightness. No
  // jump at the walk → tail handoff.
  float walkEndBri[10];
  for (int j = 0; j < pathLen; j++) {
    float depth = (float)((pathLen - 1) - j) / (float)pathLen;
    float f = (1.0f - depth);
    float b = f * f * 0.95f + 0.08f;        // same quadratic falloff as walk
    if (b > 1.0f) b = 1.0f;
    walkEndBri[j] = b;
  }
  for (int f = 0; f < tailFrames; f++) {
    restoreBase();
    for (int j = 0; j < pathLen; j++) {
      int progress = f - j;                            // start staggered: j=0 first
      int myWindow = windowFor(j);
      if (progress >= myWindow) continue;
      float fadeFactor = (progress < 0) ? 1.0f : 1.0f - (float)progress / (float)myWindow;
      float bri = walkEndBri[j] * fadeFactor * dim;
      uint8_t cR = (uint8_t)(trailColor.r * bri);
      uint8_t cG = (uint8_t)(trailColor.g * bri);
      uint8_t cB = (uint8_t)(trailColor.b * bri);
      setSquareLED(pathR[j], pathC[j], LedRGB{cR, cG, cB});
    }
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(tailFrameDelay));
  }
  // Final: clean base. The destination piece is part of the base render that
  // simulation/chess flow paints right after; we don't paint anything extra.
  restoreBase();
  showLEDs();
}

void BoardDriver::multiPathHighlight(int srcR, int srcC,
                                     const uint8_t* tgtR, const uint8_t* tgtC,
                                     const uint8_t* tgtCapture, int n,
                                     char player) {
  if (n > MPH_MAX) n = MPH_MAX;
  // Signal any in-flight replay loop to exit so the worker can pick up the
  // next job without finishing the old replay schedule first.
  mphAbort.store(true);
  mphSrcR = srcR; mphSrcC = srcC; mphCount = n;
  mphPlayer = (player == 'b') ? 'b' : 'w';
  for (int i = 0; i < n; i++) {
    mphR[i] = tgtR[i];
    mphC[i] = tgtC[i];
    mphCapture[i] = tgtCapture[i];
  }
  AnimationJob job = {AnimationType::MULTI_PATH_HL, nullptr, {}};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
}

void BoardDriver::doMultiPathHighlight() {
  // When a piece is picked up in simulation, walk a cyan trail from the
  // source to each legal target SIMULTANEOUSLY, then land on the static
  // highlight (source = hlSource, targets = hlTarget / hlCapture).
  mphAbort.store(false);   // fresh run — clear any stale abort flag
  int n = mphCount;
  if (n == 0) return;

  // Build each target's path (max 8 steps + start = 9 per path).
  static constexpr int MAX_LEN = 9;
  int paths[MPH_MAX][MAX_LEN][2];
  int pathLens[MPH_MAX];
  int maxFrames = 0;
  for (int i = 0; i < n; i++) {
    int tr = mphR[i], tc = mphC[i];
    int dr = tr - mphSrcR, dc = tc - mphSrcC;
    int adr = abs(dr), adc = abs(dc);
    bool isKnight = (adr == 1 && adc == 2) || (adr == 2 && adc == 1);
    int len = 0;
    if (isKnight) {
      // 4-cell L: source → mid (1st step of long leg) → corner (end of long
      // leg) → target. Walking BOTH intermediates means a piece sitting on
      // the long leg's first square also gets overridden — fixes "knight
      // skips over a piece and the piece's LED stays lit".
      int sR = (dr > 0) - (dr < 0);
      int sC = (dc > 0) - (dc < 0);
      paths[i][len][0] = mphSrcR; paths[i][len++][1] = mphSrcC;
      if (adr > adc) {
        // Long leg = 2 rows, short leg = 1 col.
        paths[i][len][0] = mphSrcR + sR;     paths[i][len++][1] = mphSrcC;
        paths[i][len][0] = mphSrcR + 2 * sR; paths[i][len++][1] = mphSrcC;   // corner
      } else {
        // Long leg = 2 cols, short leg = 1 row.
        paths[i][len][0] = mphSrcR; paths[i][len++][1] = mphSrcC + sC;
        paths[i][len][0] = mphSrcR; paths[i][len++][1] = mphSrcC + 2 * sC;   // corner
      }
      paths[i][len][0] = tr;      paths[i][len++][1] = tc;
    } else {
      int steps = (adr > adc) ? adr : adc;
      int sr = (dr > 0) - (dr < 0);
      int sc = (dc > 0) - (dc < 0);
      for (int s = 0; s <= steps && len < MAX_LEN; s++) {
        paths[i][len][0] = mphSrcR + sr * s;
        paths[i][len][1] = mphSrcC + sc * s;
        len++;
      }
    }
    pathLens[i] = len;
    if (len > maxFrames) maxFrames = len;
  }

  // Snapshot the un-dimmed piece base (origBase) and pre-compute the dimmed
  // version (base). When "dim others on pickup" is enabled, the pickup
  // smoothly fades from origBase → base across the pulse phase below, then
  // every subsequent painting uses `base` so non-path cells stay dimmed for
  // the rest of the pickup. Configurable percentage instead of fixed 50 %.
  LedRGB origBase[NUM_ROWS][NUM_COLS];
  LedRGB base[NUM_ROWS][NUM_COLS];
  bool dimOthers = dimOthersOnPickup;
  float dimFactor = dimOthers ? (dimOthersPct / 100.0f) : 1.0f;
  if (dimFactor > 1.0f) dimFactor = 1.0f;
  for (int r = 0; r < NUM_ROWS; r++)
    for (int c = 0; c < NUM_COLS; c++) {
      origBase[r][c] = currentColors[r][c];
      base[r][c] = LedRGB{
          (uint8_t)(origBase[r][c].r * dimFactor),
          (uint8_t)(origBase[r][c].g * dimFactor),
          (uint8_t)(origBase[r][c].b * dimFactor)};
    }

  float dim = getAutoDimFactor();

  // Pickup confirmation phase — paint the source square in the player's path
  // colour FIRST so the user sees "I picked up this piece" before any trail
  // starts crawling outward. Without this, the source pixel was just frame-0
  // of every parallel head and read as "all targets" rather than the source.
  // SIM overrides per-player colour to fixed blue/green; everywhere else
  // uses the user-configured chess animation settings.
  LedRGB pathColor = effectivePathColor(mphPlayer);
  // Source colour = pathColor lerped 50 % toward white. Makes the picked-up
  // square distinctly more "present" than the path/target cells which sit
  // at the plain pathColor — user complaint was that source disappeared into
  // the rest of the highlight at equal brightness.
  LedRGB srcColor = LedRGB{
      (uint8_t)(pathColor.r + (255 - pathColor.r) * 0.50f),
      (uint8_t)(pathColor.g + (255 - pathColor.g) * 0.50f),
      (uint8_t)(pathColor.b + (255 - pathColor.b) * 0.50f)};
  // Pulse phase. When dim-on-pickup is OFF: one frame, source pop, brief
  // sleep. When it's ON: a WAVE of dimming expands from the source square
  // outward — cells closer to the picked-up piece fade to dimmed first,
  // cells at the corners last. Per-cell fade timing is driven by distance
  // to the source vs the current wave radius.
  if (!dimOthers) {
    // Fast path — match the old "instant" pulse.
    currentColors[mphSrcR][mphSrcC] = srcColor;
    strip->SetPixelColor(getPixelIndex(mphSrcR, mphSrcC), RgbColor(srcColor.r, srcColor.g, srcColor.b));
    showLEDs();
    if (pickupPulseMs > 0) vTaskDelay(pdMS_TO_TICKS(pickupPulseMs));
  } else {
    const int   WAVE_STEPS = 14;
    const float MAX_R = sqrtf((float)((NUM_ROWS - 1) * (NUM_ROWS - 1) + (NUM_COLS - 1) * (NUM_COLS - 1))) + 1.0f;
    const float FADE_BAND = 1.4f;   // cells fade in over this many wave-radius units
    uint16_t totalPulse = (pickupPulseMs > 0) ? pickupPulseMs : 240;
    if (totalPulse < 200) totalPulse = 200;   // needs time for the wave to read visibly
    uint32_t stepMs = totalPulse / WAVE_STEPS;
    if (stepMs < 14) stepMs = 14;
    for (int step = 1; step <= WAVE_STEPS; step++) {
      float frac  = (float)step / (float)WAVE_STEPS;          // 0..1
      float waveR = frac * (MAX_R + FADE_BAND);               // ahead of edge so corner cells finish
      for (int r = 0; r < NUM_ROWS; r++) {
        for (int c = 0; c < NUM_COLS; c++) {
          float dr = (float)r - (float)mphSrcR;
          float dc = (float)c - (float)mphSrcC;
          float dist = sqrtf(dr * dr + dc * dc);
          float cellT;
          if (dist >= waveR)            cellT = 0.0f;   // wave hasn't reached → still origBase
          else if (dist > waveR - FADE_BAND) cellT = (waveR - dist) / FADE_BAND;   // in fade band
          else                           cellT = 1.0f;   // wave has passed → fully dimmed
          LedRGB o = origBase[r][c];
          LedRGB d = base[r][c];
          uint8_t cr = (uint8_t)(o.r + ((int)d.r - (int)o.r) * cellT);
          uint8_t cg = (uint8_t)(o.g + ((int)d.g - (int)o.g) * cellT);
          uint8_t cb = (uint8_t)(o.b + ((int)d.b - (int)o.b) * cellT);
          setSquareLED(r, c, LedRGB{cr, cg, cb});
        }
      }
      // Source on top — full bright the whole way, bypassing dim mask.
      currentColors[mphSrcR][mphSrcC] = srcColor;
      strip->SetPixelColor(getPixelIndex(mphSrcR, mphSrcC), RgbColor(srcColor.r, srcColor.g, srcColor.b));
      showLEDs();
      vTaskDelay(pdMS_TO_TICKS(stepMs));
    }
  }

  float acc[NUM_ROWS][NUM_COLS];
  float speed = (mphPlayer == 'b') ? animChessSpeedB : animChessSpeedW;
  if (speed < 0.25f) speed = 0.25f;
  if (speed > 3.0f)  speed = 3.0f;
  uint32_t frameDelay = (uint32_t)(90.0f / speed);

  // Per-target brightness ceiling: knight L-trails are clamped to a much
  // lower value than slider trails so a knight target reads as "clearly
  // brighter" than the path it jumped over. Slider targets aren't capped —
  // the trail and target both hit full pathColor.
  float targetCeiling[MPH_MAX];
  for (int i = 0; i < n; i++) {
    int dr = mphR[i] - mphSrcR, dc = mphC[i] - mphSrcC;
    int adr = abs(dr), adc = abs(dc);
    bool isKnight = (adr == 1 && adc == 2) || (adr == 2 && adc == 1);
    targetCeiling[i] = isKnight ? (knightPathPct / 100.0f) : 1.0f;
  }

  for (int frame = 0; frame < maxFrames; frame++) {
    // Bail early if the move was already committed or a new pickup arrived.
    if (mphAbort.load() || uxQueueMessagesWaiting(animationQueue) > 0) return;
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++)
        acc[r][c] = 0.0f;
    for (int i = 0; i < n; i++) {
      int head = (frame < pathLens[i]) ? frame : (pathLens[i] - 1);
      for (int s = 0; s <= head; s++) {
        float depth = (float)(head - s) / (float)pathLens[i];
        float bri = (1.0f - depth) * 0.95f + 0.15f;
        if (bri > 1.0f) bri = 1.0f;
        // Cap intermediate path cells (NOT the actual target endpoint) — for
        // knight L-paths this dims the jumped-over corner while the target
        // itself stays bright. Slider paths use ceiling=1.0 → no effect.
        bool isTargetCell = (s == pathLens[i] - 1);
        if (!isTargetCell) bri *= targetCeiling[i];
        int rr = paths[i][s][0], cc = paths[i][s][1];
        if (bri > acc[rr][cc]) acc[rr][cc] = bri;
      }
    }
    // Composite: base + cyan trail (max per channel to avoid washout).
    for (int r = 0; r < NUM_ROWS; r++) {
      for (int c = 0; c < NUM_COLS; c++) {
        if (acc[r][c] > 0.01f) {
          float bri = acc[r][c] * dim;
          // Trail overrides the base AND bypasses BOTH the dark-square dim
          // mask AND the autoDim multiplier — what the user picked is what
          // they get on the LEDs. Otherwise night-time autoDim or a low
          // dimMultiplier would silently swallow the already-faint knight
          // path (5 % × autoDim 50 % = 2.5 % ≈ invisible).
          float briRaw = acc[r][c];
          uint8_t cR = (uint8_t)(pathColor.r * briRaw);
          uint8_t cG = (uint8_t)(pathColor.g * briRaw);
          uint8_t cB = (uint8_t)(pathColor.b * briRaw);
          currentColors[r][c] = LedRGB{cR, cG, cB};
          strip->SetPixelColor(getPixelIndex(r, c), RgbColor(cR, cG, cB));
        } else {
          setSquareLED(r, c, base[r][c]);
        }
      }
    }
    // Knight L-intermediates (mid AND corner — indices 1 and 2 of the 4-cell
    // L-path) stay at the configured low brightness every frame so they
    // ALWAYS override any underlying piece colour, not just when the head
    // happens to be crossing them.
    float kPct = knightPathPct / 100.0f;
    uint8_t kR = (uint8_t)(pathColor.r * kPct);
    uint8_t kG = (uint8_t)(pathColor.g * kPct);
    uint8_t kB = (uint8_t)(pathColor.b * kPct);
    for (int i = 0; i < n; i++) {
      int dr = mphR[i] - mphSrcR, dc = mphC[i] - mphSrcC;
      int adr = abs(dr), adc = abs(dc);
      bool isKnight = (adr == 1 && adc == 2) || (adr == 2 && adc == 1);
      if (!isKnight) continue;
      // paths[i][1] = mid, paths[i][2] = corner. Both are L-intermediates.
      for (int idx = 1; idx <= 2; idx++) {
        int rr = paths[i][idx][0], cc = paths[i][idx][1];
        if (rr == mphSrcR && cc == mphSrcC) continue;
        currentColors[rr][cc] = LedRGB{kR, kG, kB};
        strip->SetPixelColor(getPixelIndex(rr, cc), RgbColor(kR, kG, kB));
      }
    }
    // Source stays at the brighter srcColor (lerped to white) so it visually
    // dominates the path cells around it.
    currentColors[mphSrcR][mphSrcC] = srcColor;
    strip->SetPixelColor(getPixelIndex(mphSrcR, mphSrcC), RgbColor(srcColor.r, srcColor.g, srcColor.b));
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(frameDelay));
  }

  // Final state: base + targets at full path colour + knight corners dimmed
  // + source at full path colour. No distance gradient — targets stay at a
  // single bright level so the contrast against the 22 % knight corner is
  // unambiguous.
  for (int r = 0; r < NUM_ROWS; r++)
    for (int c = 0; c < NUM_COLS; c++)
      setSquareLED(r, c, base[r][c]);
  // Knight L-intermediates at 5 % — paint BOTH cells of the long leg
  // (paths[i][1] and paths[i][2]) so a piece sitting on either gets
  // overridden. Direct strip write bypasses the dark-square dim mask.
  float endK_pct = knightPathPct / 100.0f;
  uint8_t endK_R = (uint8_t)(pathColor.r * endK_pct);
  uint8_t endK_G = (uint8_t)(pathColor.g * endK_pct);
  uint8_t endK_B = (uint8_t)(pathColor.b * endK_pct);
  for (int i = 0; i < n; i++) {
    int dr = mphR[i] - mphSrcR, dc = mphC[i] - mphSrcC;
    int adr = abs(dr), adc = abs(dc);
    bool isKnight = (adr == 1 && adc == 2) || (adr == 2 && adc == 1);
    if (!isKnight) continue;
    for (int idx = 1; idx <= 2; idx++) {
      int rr = paths[i][idx][0], cc = paths[i][idx][1];
      if (rr == mphSrcR && cc == mphSrcC) continue;
      currentColors[rr][cc] = LedRGB{endK_R, endK_G, endK_B};
      strip->SetPixelColor(getPixelIndex(rr, cc), RgbColor(endK_R, endK_G, endK_B));
    }
  }
  // Targets land at the FULL player path colour — no distance gradient. The
  // contrast against the dim corner makes the knight's destination pop, and
  // it removes the brightness pop the user complained about (animation peak
  // and end-state were different brightnesses → "heller nach Animation").
  for (int i = 0; i < n; i++) {
    uint8_t cr, cg, cb;
    if (mphCapture[i]) {
      LedRGB cap = hlCaptureColor();
      cr = (uint8_t)(cap.r * 0.65f + pathColor.r * 0.35f);
      cg = (uint8_t)(cap.g * 0.65f + pathColor.g * 0.35f);
      cb = (uint8_t)(cap.b * 0.65f + pathColor.b * 0.35f);
    } else {
      cr = pathColor.r; cg = pathColor.g; cb = pathColor.b;
    }
    currentColors[mphR[i]][mphC[i]] = LedRGB{cr, cg, cb};
    strip->SetPixelColor(getPixelIndex(mphR[i], mphC[i]), RgbColor(cr, cg, cb));
  }
  // Source: the brighter srcColor on top, no dim mask.
  currentColors[mphSrcR][mphSrcC] = srcColor;
  strip->SetPixelColor(getPixelIndex(mphSrcR, mphSrcC), RgbColor(srcColor.r, srcColor.g, srcColor.b));
  showLEDs();

  // Snapshot the highlight end-state so the replay loop can paint over it
  // without losing source / target / knight-corner colours.
  LedRGB highlight[NUM_ROWS][NUM_COLS];
  for (int r = 0; r < NUM_ROWS; r++)
    for (int c = 0; c < NUM_COLS; c++)
      highlight[r][c] = currentColors[r][c];

  // Replay loop: as long as the piece stays picked up, periodically re-walk
  // the trail as a brighter overlay on top of the static highlight — the
  // user wants the path "nachgemalt" (re-painted) with a delay, not erased.
  // mphAbort is set by the next multiPathHighlight() / clear, so a new
  // pickup pre-empts the replay immediately.
  const int MAX_REPLAYS   = 12;        // ~ a minute of held-down replays
  const int DELAY_TICKS   = replayIntervalMs / 50;
  const float OVERLAY_MIX = replayOverlayPct / 100.0f;
  for (int rep = 0; rep < MAX_REPLAYS; rep++) {
    for (int t = 0; t < DELAY_TICKS; t++) {
      if (mphAbort.load() || uxQueueMessagesWaiting(animationQueue) > 0) return;
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    // Re-walk: bright overlay over the highlight base. acc reused.
    for (int frame = 0; frame < maxFrames; frame++) {
      if (mphAbort.load() || uxQueueMessagesWaiting(animationQueue) > 0) return;
      for (int r = 0; r < NUM_ROWS; r++)
        for (int c = 0; c < NUM_COLS; c++)
          acc[r][c] = 0.0f;
      for (int i = 0; i < n; i++) {
        int head = (frame < pathLens[i]) ? frame : (pathLens[i] - 1);
        for (int s = 0; s <= head; s++) {
          float depth = (float)(head - s) / (float)pathLens[i];
          float bri = (1.0f - depth) * 0.95f + 0.15f;
          if (bri > 1.0f) bri = 1.0f;
          int rr = paths[i][s][0], cc = paths[i][s][1];
          if (bri > acc[rr][cc]) acc[rr][cc] = bri;
        }
      }
      for (int r = 0; r < NUM_ROWS; r++) {
        for (int c = 0; c < NUM_COLS; c++) {
          LedRGB h = highlight[r][c];
          if (acc[r][c] > 0.01f) {
            // Brighten the underlying highlight toward white — the trail
            // "lights it up" without overwriting the highlight colour.
            float mix = acc[r][c] * OVERLAY_MIX;
            uint8_t cR = (uint8_t)(h.r + (255 - h.r) * mix);
            uint8_t cG = (uint8_t)(h.g + (255 - h.g) * mix);
            uint8_t cB = (uint8_t)(h.b + (255 - h.b) * mix);
            currentColors[r][c] = LedRGB{cR, cG, cB};
            strip->SetPixelColor(getPixelIndex(r, c), RgbColor(cR, cG, cB));
          } else {
            currentColors[r][c] = h;
            strip->SetPixelColor(getPixelIndex(r, c), RgbColor(h.r, h.g, h.b));
          }
        }
      }
      showLEDs();
      vTaskDelay(pdMS_TO_TICKS(frameDelay));
    }
    // Soft fade back to the plain highlight so the overlay doesn't snap off.
    for (int fadeStep = 1; fadeStep <= 4; fadeStep++) {
      if (mphAbort.load() || uxQueueMessagesWaiting(animationQueue) > 0) return;
      float fadeOut = 1.0f - (float)fadeStep / 4.0f;
      for (int r = 0; r < NUM_ROWS; r++) {
        for (int c = 0; c < NUM_COLS; c++) {
          LedRGB h = highlight[r][c];
          if (acc[r][c] > 0.01f) {
            float mix = acc[r][c] * OVERLAY_MIX * fadeOut;
            uint8_t cR = (uint8_t)(h.r + (255 - h.r) * mix);
            uint8_t cG = (uint8_t)(h.g + (255 - h.g) * mix);
            uint8_t cB = (uint8_t)(h.b + (255 - h.b) * mix);
            currentColors[r][c] = LedRGB{cR, cG, cB};
            strip->SetPixelColor(getPixelIndex(r, c), RgbColor(cR, cG, cB));
          } else {
            currentColors[r][c] = h;
            strip->SetPixelColor(getPixelIndex(r, c), RgbColor(h.r, h.g, h.b));
          }
        }
      }
      showLEDs();
      vTaskDelay(pdMS_TO_TICKS(35));
    }
  }
}

void BoardDriver::waitForAnimationQueue(uint32_t maxWaitMs) {
  // Block until every queued job has been dequeued AND the worker has
  // finished its current animation. Used at game start so the clock doesn't
  // tick during the opening flourish.
  uint32_t waited = 0;
  while (uxQueueMessagesWaiting(animationQueue) > 0 && waited < maxWaitMs) {
    vTaskDelay(pdMS_TO_TICKS(40));
    waited += 40;
  }
  // Worker may still be inside executeAnimation; grab the mutex once to wait
  // for it, then release immediately.
  if (xSemaphoreTake(ledMutex, pdMS_TO_TICKS(maxWaitMs))) {
    xSemaphoreGive(ledMutex);
  }
}

void BoardDriver::doCapture(int centerRow, int centerCol) {
  float centerX = centerCol + 0.5f;
  float centerY = centerRow + 0.5f;

  const int numWaves = 3;
  const int totalFrames = 20;
  const float waveSpeed = 0.4f;
  const float waveWidth = 1.2f;

  // Snapshot the current LED state so each frame can be rebuilt as
  // "base + additive wave" — the underlying picture stays visible
  // while the red/yellow shock pulses out from the captured square.
  LedRGB base[NUM_ROWS][NUM_COLS];
  for (int r = 0; r < NUM_ROWS; r++)
    for (int c = 0; c < NUM_COLS; c++)
      base[r][c] = currentColors[r][c];

  for (int frame = 0; frame < totalFrames; frame++) {
    for (int row = 0; row < NUM_ROWS; row++) {
      for (int col = 0; col < NUM_COLS; col++) {
        float dx = col - centerX;
        float dy = row - centerY;
        float dist = sqrt(dx * dx + dy * dy);

        uint8_t waveR = 0, waveG = 0, waveB = 0;
        for (int w = 0; w < numWaves; w++) {
          float waveRadius = (frame - w * 4) * waveSpeed;
          if (waveRadius < 0) continue;
          float distToWave = fabs(dist - waveRadius);
          if (distToWave < waveWidth) {
            float intensity = 1.0f - (distToWave / waveWidth);
            intensity *= intensity;
            float fadeOut = 1.0f - (waveRadius / 6.0f);
            if (fadeOut < 0) fadeOut = 0;
            intensity *= fadeOut;
            if (w % 2 == 0) {
              waveR = max(waveR, (uint8_t)(LedColors::Red.r * intensity));
              waveG = max(waveG, (uint8_t)(LedColors::Red.g * intensity));
              waveB = max(waveB, (uint8_t)(LedColors::Red.b * intensity));
            } else {
              waveR = max(waveR, (uint8_t)(LedColors::Yellow.r * intensity));
              waveG = max(waveG, (uint8_t)(LedColors::Yellow.g * intensity));
              waveB = max(waveB, (uint8_t)(LedColors::Yellow.b * intensity));
            }
          }
        }
        // Composite base + wave (clamped additive). When the wave is silent
        // for this pixel, the underlying piece colour stays intact.
        LedRGB b = base[row][col];
        uint16_t mR = (uint16_t)b.r + waveR;
        uint16_t mG = (uint16_t)b.g + waveG;
        uint16_t mB = (uint16_t)b.b + waveB;
        if (mR > 255) mR = 255;
        if (mG > 255) mG = 255;
        if (mB > 255) mB = 255;
        setSquareLED(row, col, LedRGB{(uint8_t)mR, (uint8_t)mG, (uint8_t)mB});
      }
    }
    // The captured square stays bright red throughout so the eye locks onto it.
    setSquareLED(centerRow, centerCol, LedColors::Red);
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  // End: restore the underlying picture. NO clearAllLEDs at the end.
  for (int r = 0; r < NUM_ROWS; r++)
    for (int c = 0; c < NUM_COLS; c++)
      setSquareLED(r, c, base[r][c]);
  showLEDs();
}

void BoardDriver::promotionAnimation(int row, int col) {
  AnimationJob job = {AnimationType::PROMOTION, nullptr, {}};
  job.params.promotion.row = row;
  job.params.promotion.col = col;
  xQueueSend(animationQueue, &job, portMAX_DELAY);
}

void BoardDriver::doPromotion(int row, int col) {
  clearAllLEDs(false);
  // Column-based waterfall animation
  for (int step = 0; step < 16; step++) {
    for (int r = 0; r < NUM_ROWS; r++) {
      // Create a golden wave moving up and down the column
      if ((step + r) % 8 < 4)
        setSquareLED(r, col, LedColors::Yellow);
      else
        setSquareLED(r, col, LedColors::Off);
    }
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // Leave yellow LED on the promotion square to indicate a promotion piece needs to be chosen (LED will be cleared by replacing the pawn for the new piece or by any other move)
  clearAllLEDs(false);
  setSquareLED(row, col, LedColors::Yellow);
  showLEDs();
}

void BoardDriver::flashBoardAnimation(LedRGB color, int times) {
  AnimationJob job = {AnimationType::FLASH, nullptr, {}};
  job.params.flash = {color, times};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
}

void BoardDriver::doFlash(LedRGB color, int times) {
  for (int i = 0; i < times; i++) {
    clearAllLEDs();
    vTaskDelay(pdMS_TO_TICKS(200));
    // Light up entire board with specified color
    for (int row = 0; row < NUM_ROWS; row++)
      for (int col = 0; col < NUM_COLS; col++)
        setSquareLED(row, col, color);
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  clearAllLEDs();
}

void BoardDriver::startupAnimation() {
  // Two-act boot sequence, ~1.8 s total.
  //   Act 1: cells light up in an outside→inside spiral, each cell hued by
  //          its distance from the centre (blue at the edges, warm red at
  //          the heart). Feels like the board "waking up" from the outside.
  //   Act 2: brief white flash, then a smooth fade to black so whatever
  //          comes next starts on a clean slate.
  acquireLEDs();
  clearAllLEDs(false);
  showLEDs();

  const float cx = (NUM_COLS - 1) * 0.5f;
  const float cy = (NUM_ROWS - 1) * 0.5f;
  const float maxDist = sqrtf(cx * cx + cy * cy);

  auto write = [&](int r, int c, uint8_t R, uint8_t G, uint8_t B) {
    currentColors[r][c] = LedRGB{R, G, B};
    strip->SetPixelColor(getPixelIndex(r, c), RgbColor(R, G, B));
  };

  // Build outside-in spiral order (64 cells in 4 concentric rings).
  uint8_t orderR[64], orderC[64];
  int n = 0;
  int top = 0, bottom = NUM_ROWS - 1, left = 0, right = NUM_COLS - 1;
  while (top <= bottom && left <= right) {
    for (int c = left; c <= right; c++)            { orderR[n] = top;    orderC[n++] = c; }
    top++;
    for (int r = top; r <= bottom; r++)            { orderR[n] = r;      orderC[n++] = right; }
    right--;
    if (top <= bottom)
      for (int c = right; c >= left; c--)          { orderR[n] = bottom; orderC[n++] = c; }
    bottom--;
    if (left <= right)
      for (int r = bottom; r >= top; r--)          { orderR[n] = r;      orderC[n++] = left; }
    left++;
  }

  // Act 1 — spiral fill (~20 ms per cell × 64 ≈ 1.3 s).
  for (int i = 0; i < n; i++) {
    int r = orderR[i], c = orderC[i];
    float dx = c - cx, dy = r - cy;
    float dist = sqrtf(dx * dx + dy * dy);
    float t = dist / (maxDist > 0.0f ? maxDist : 1.0f);  // 0 centre → 1 corner
    // Hue: 220° (deep blue) at the corners, 0° (red) at the centre.
    float hue = 220.0f * t;
    LedRGB col = hsvToRgb(hue, 1.0f, 1.0f);
    write(r, c, col.r, col.g, col.b);
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  // Act 2a — short white flash.
  for (int r = 0; r < NUM_ROWS; r++)
    for (int c = 0; c < NUM_COLS; c++)
      write(r, c, 255, 255, 255);
  showLEDs();
  vTaskDelay(pdMS_TO_TICKS(90));

  // Act 2b — fade-out.
  for (int step = 0; step < 10; step++) {
    float f = 1.0f - (float)(step + 1) / 10.0f;
    uint8_t v = (uint8_t)(255 * f);
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++)
        write(r, c, v, v, v);
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(30));
  }
  clearAllLEDs(false);
  showLEDs();
  releaseLEDs();
}

void BoardDriver::fillUpAnimation(LedRGB color, int msPerLed) {
  // Serpentine sweep: even rows L→R, odd rows R→L — feels more "alive"
  // than a raster scan and follows how the LED strip is wired in many builds.
  acquireLEDs();
  clearAllLEDs(false);
  for (int row = 0; row < NUM_ROWS; row++) {
    for (int i = 0; i < NUM_COLS; i++) {
      int col = (row & 1) ? (NUM_COLS - 1 - i) : i;
      setSquareLED(row, col, color);
      showLEDs();
      delay(msPerLed);
    }
  }
  releaseLEDs();
}

std::atomic<bool>* BoardDriver::startThinkingAnimation() {
  auto* stopFlag = new std::atomic<bool>(false);
  AnimationJob job = {AnimationType::THINKING, stopFlag, {}};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
  return stopFlag;
}

void BoardDriver::doThinking(std::atomic<bool>* stopFlag) {
  static const int corners[][2] = {{0, 0}, {0, NUM_COLS-1}, {NUM_ROWS-1, 0}, {NUM_ROWS-1, NUM_COLS-1}};

  // Color configuration - center hue shown at peak brightness
  static const float HUE_CENTER = 240.0f; // Blue hue
  static const float HUE_RANGE = 10.0f;   // Shift toward purple when dim
  static const float BRIGHTNESS_MIN = 0.08f;
  static const float BRIGHTNESS_MAX = 1.0f;

  float phase = 0.0f;            // 0 to 2*PI for smooth sine wave
  const float phaseStep = 0.04f; // Controls breathing speed

  clearAllLEDs(false);
  while (!stopFlag || !stopFlag->load()) {
    // Smooth sine wave for breathing (0 to 1)
    float breathe = (sinf(phase) + 1.0f) * 0.5f;

    // Brightness follows the breathing curve
    float brightness = BRIGHTNESS_MIN + breathe * (BRIGHTNESS_MAX - BRIGHTNESS_MIN);

    // Hue synced: center hue at peak, shifts toward purple as it dims
    // Using cosine so hue is at center when brightness peaks
    float hue = HUE_CENTER + HUE_RANGE * (1.0f - breathe);

    // HSV to RGB conversion (saturation = 1.0)
    float h = fmod(hue, 360.0f) / 60.0f;
    int hi = (int)h;
    float f = h - hi;
    float v = brightness;
    float q = v * (1.0f - f);
    float t = v * f;

    uint8_t r = 0, g = 0, b = 0;
    switch (hi) {
      case 0:
        r = v * 255;
        g = t * 255;
        b = 0;
        break;
      case 1:
        r = q * 255;
        g = v * 255;
        b = 0;
        break;
      case 2:
        r = 0;
        g = v * 255;
        b = t * 255;
        break;
      case 3:
        r = 0;
        g = q * 255;
        b = v * 255;
        break;
      case 4:
        r = t * 255;
        g = 0;
        b = v * 255;
        break;
      default:
        r = v * 255;
        g = 0;
        b = q * 255;
        break;
    }

    for (auto& corner : corners)
      setSquareLED(corner[0], corner[1], LedRGB{r, g, b});
    showLEDs();

    phase += phaseStep;
    if (phase >= 2.0f * M_PI)
      phase -= 2.0f * M_PI;

    vTaskDelay(pdMS_TO_TICKS(30));
  }
  clearAllLEDs();
  delete stopFlag;
}

std::atomic<bool>* BoardDriver::startWaitingAnimation() {
  auto* stopFlag = new std::atomic<bool>(false);
  AnimationJob job = {AnimationType::WAITING, stopFlag, {}};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
  return stopFlag;
}

void BoardDriver::doWaiting(std::atomic<bool>* stopFlag) {
  // Border perimeter for NUM_ROWS x NUM_COLS board (clockwise)
  static const int positions[][2] = {
    {0,0},{0,1},{0,2},{0,3},{0,4},{0,5},{0,6},{0,7},
    {1,7},{2,7},{3,7},
    {3,6},{3,5},{3,4},{3,3},{3,2},{3,1},{3,0},
    {2,0},{1,0}
  };
  static const int numPositions = sizeof(positions) / sizeof(positions[0]);

  int frame = 0;
  while (!stopFlag || !stopFlag->load()) {
    clearAllLEDs(false);
    // Light up consecutive LEDs moving around the board
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 2; j++) {
        int idx = (frame + i + (j * (numPositions / 2))) % numPositions;
        setSquareLED(positions[idx][0], positions[idx][1], LedColors::White);
      }
    }
    showLEDs();
    frame = (frame + 1) % numPositions;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  clearAllLEDs();
  delete stopFlag;
}

std::atomic<bool>* BoardDriver::startIdleAnimation() {
  if (idleAnimation == IdleAnimation::OFF) {
    clearAllLEDs();
    return nullptr;
  }
  auto* stopFlag = new std::atomic<bool>(false);
  AnimationType type;
  switch (idleAnimation) {
    case IdleAnimation::BREATHING:   type = AnimationType::BREATHING;   break;
    case IdleAnimation::CHASE:       type = AnimationType::CHASE;       break;
    case IdleAnimation::TWINKLE:     type = AnimationType::TWINKLE;     break;
    case IdleAnimation::SOLID:       type = AnimationType::SOLID;       break;
    case IdleAnimation::FIRE:        type = AnimationType::FIRE;        break;
    case IdleAnimation::PLASMA:      type = AnimationType::PLASMA;      break;
    case IdleAnimation::METEOR:      type = AnimationType::METEOR;      break;
    case IdleAnimation::CHESS_PULSE: type = AnimationType::CHESS_PULSE; break;
    case IdleAnimation::COLOR_CYCLE: type = AnimationType::COLOR_CYCLE; break;
    case IdleAnimation::SCAN:        type = AnimationType::SCAN;        break;
    case IdleAnimation::FIREWORKS:   type = AnimationType::FIREWORKS;   break;
    case IdleAnimation::RIPPLE:      type = AnimationType::RIPPLE;      break;
    case IdleAnimation::MATRIX:      type = AnimationType::MATRIX;      break;
    case IdleAnimation::AURORA:      type = AnimationType::AURORA;      break;
    case IdleAnimation::SPIRAL:      type = AnimationType::SPIRAL;      break;
    case IdleAnimation::STANDOFF:    type = AnimationType::STANDOFF;    break;
    case IdleAnimation::KNIGHT_TOUR: type = AnimationType::KNIGHT_TOUR; break;
    case IdleAnimation::REPLAY:      type = AnimationType::REPLAY;      break;
    case IdleAnimation::ATTACK:      type = AnimationType::ATTACK;      break;
    case IdleAnimation::TOUCH:       type = AnimationType::TOUCH;       break;
    case IdleAnimation::LIFE:        type = AnimationType::LIFE;        break;
    case IdleAnimation::SAND:        type = AnimationType::SAND;        break;
    case IdleAnimation::LAVA:        type = AnimationType::LAVA;        break;
    case IdleAnimation::RAIN:        type = AnimationType::RAIN;        break;
    case IdleAnimation::STARFIELD:   type = AnimationType::STARFIELD;   break;
    case IdleAnimation::WALLCLOCK:   type = AnimationType::WALLCLOCK;   break;
    case IdleAnimation::TEXT:        type = AnimationType::TEXT;        break;
    case IdleAnimation::PAINT:       type = AnimationType::PAINT;       break;
    case IdleAnimation::TETRIS:      type = AnimationType::TETRIS;      break;
    case IdleAnimation::AUDIO:       type = AnimationType::AUDIO;       break;
    case IdleAnimation::SNAKE:       type = AnimationType::SNAKE;       break;
    case IdleAnimation::PONG:        type = AnimationType::PONG;        break;
    case IdleAnimation::LANGTON:     type = AnimationType::LANGTON;     break;
    case IdleAnimation::BOUNCE:      type = AnimationType::BOUNCE;      break;
    case IdleAnimation::SNOW:        type = AnimationType::SNOW;        break;
    case IdleAnimation::BUBBLES:     type = AnimationType::BUBBLES;     break;
    case IdleAnimation::SORT:        type = AnimationType::SORT;        break;
    case IdleAnimation::WAVE:        type = AnimationType::WAVE;        break;
    case IdleAnimation::RULE30:      type = AnimationType::RULE30;      break;
    case IdleAnimation::BREAKOUT:    type = AnimationType::BREAKOUT;    break;
    case IdleAnimation::TRON:        type = AnimationType::TRON;        break;
    case IdleAnimation::CATCH:       type = AnimationType::CATCH;       break;
    case IdleAnimation::SIMON:       type = AnimationType::SIMON;       break;
    case IdleAnimation::CHARGE:      type = AnimationType::CHARGE;      break;
    case IdleAnimation::SHIELDWALL:  type = AnimationType::SHIELDWALL;  break;
    case IdleAnimation::COURT:       type = AnimationType::COURT;       break;
    default:                         type = AnimationType::RAINBOW;     break;
  }
  AnimationJob job = {type, stopFlag, {}};
  xQueueSend(animationQueue, &job, portMAX_DELAY);
  return stopFlag;
}

void BoardDriver::doRainbow(std::atomic<bool>* stopFlag) {
  float baseHue = 0.0f;
  while (!stopFlag || !stopFlag->load()) {
    for (int row = 0; row < NUM_ROWS; row++) {
      for (int col = 0; col < NUM_COLS; col++) {
        float diag = (float)(row + col) / (float)(NUM_ROWS + NUM_COLS - 2);
        float hue = fmod(baseHue + diag * 360.0f, 360.0f);
        setSquareLED(row, col, hsvToRgb(hue, 1.0f, 1.0f));
      }
    }
    showLEDs();
    baseHue += 2.0f;
    if (baseHue >= 360.0f) baseHue -= 360.0f;
    animDelay(28);
  }
  clearAllLEDs();
  delete stopFlag;
}

void BoardDriver::doBreathing(std::atomic<bool>* stopFlag) {
  float phase = 0.0f;
  while (!stopFlag || !stopFlag->load()) {
    float t = (sinf(phase) + 1.0f) * 0.5f;
    float b = 0.04f + 0.96f * (t * t);  // cubic ease: long dim, quick bright
    uint8_t r = (uint8_t)(idleColor.r * b);
    uint8_t g = (uint8_t)(idleColor.g * b);
    uint8_t bv = (uint8_t)(idleColor.b * b);
    for (int row = 0; row < NUM_ROWS; row++)
      for (int col = 0; col < NUM_COLS; col++)
        setSquareLED(row, col, LedRGB{r, g, bv});
    showLEDs();
    phase += 0.022f;
    if (phase >= 2.0f * M_PI) phase -= 2.0f * M_PI;
    animDelay(20);
  }
  clearAllLEDs();
  delete stopFlag;
}

void BoardDriver::doChase(std::atomic<bool>* stopFlag) {
  // Perimeter of the NUM_ROWS x NUM_COLS board, clockwise from top-left.
  // Built from the grid dimensions so it scales (4x8, 8x8, …) without a hardcode.
  int border[2 * (NUM_ROWS + NUM_COLS) - 4][2];
  int N = 0;
  for (int c = 0; c < NUM_COLS; c++)      { border[N][0] = 0;            border[N][1] = c;            N++; } // top  L→R
  for (int r = 1; r < NUM_ROWS; r++)      { border[N][0] = r;            border[N][1] = NUM_COLS - 1; N++; } // right T→B
  for (int c = NUM_COLS - 2; c >= 0; c--) { border[N][0] = NUM_ROWS - 1; border[N][1] = c;            N++; } // bottom R→L
  for (int r = NUM_ROWS - 2; r >= 1; r--) { border[N][0] = r;            border[N][1] = 0;            N++; } // left B→T
  const int TAIL = 10;
  const int NUM_COMETS = 2;
  float baseHue = idleHue(0.0f);
  int frame = 0;
  while (!stopFlag || !stopFlag->load()) {
    for (int i = 0; i < LED_COUNT; i++) strip->SetPixelColor(i, RgbColor(0));
    for (int c = 0; c < NUM_COMETS; c++) {
      float cometHue = fmod(baseHue + c * (360.0f / NUM_COMETS), 360.0f);
      int head = (frame + c * (N / NUM_COMETS)) % N;
      for (int t = 0; t < TAIL; t++) {
        int idx = (head - t + N * 100) % N;
        float fade = powf(1.0f - (float)t / TAIL, 2.0f);
        LedRGB px = hsvToRgb(cometHue, 1.0f, fade);
        strip->SetPixelColor(ledIndexMap[border[idx][0]][border[idx][1]], RgbColor(px.r, px.g, px.b));
      }
    }
    showLEDs();
    frame = (frame + 1) % N;
    animDelay(42);
  }
  clearAllLEDs();
  delete stopFlag;
}

void BoardDriver::doTwinkle(std::atomic<bool>* stopFlag) {
  float pixBrightness[NUM_ROWS][NUM_COLS] = {};
  float hues[NUM_ROWS][NUM_COLS] = {};
  bool active[NUM_ROWS][NUM_COLS] = {};
  float baseHue = rgbToHue(idleColor);
  bool isChromatic = !idleRandomColor && rgbSaturation(idleColor) > 0.15f;
  uint32_t seed = 12345;
  auto rng = [&]() -> uint32_t { seed ^= seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  while (!stopFlag || !stopFlag->load()) {
    // Spawn new twinkles (higher rate for denser starfield)
    if ((rng() % 3) == 0) {
      int row = rng() % NUM_ROWS, col = rng() % NUM_COLS;
      if (!active[row][col]) {
        active[row][col] = true;
        pixBrightness[row][col] = 0;
        float variation = (float)((int)(rng() % 61) - 30);
        hues[row][col] = isChromatic
          ? fmod(baseHue + variation + 360.0f, 360.0f)
          : (float)(rng() % 360);
      }
    }
    for (int row = 0; row < NUM_ROWS; row++) {
      for (int col = 0; col < NUM_COLS; col++) {
        if (active[row][col]) {
          pixBrightness[row][col] += 0.06f;
          if (pixBrightness[row][col] > 1.0f)
            pixBrightness[row][col] = 1.0f - (pixBrightness[row][col] - 1.0f) * 1.6f;
          if (pixBrightness[row][col] < 0) { pixBrightness[row][col] = 0; active[row][col] = false; }
          setSquareLED(row, col, hsvToRgb(hues[row][col], 1.0f, pixBrightness[row][col]));
        } else {
          setSquareLED(row, col, LedRGB{0, 0, 0});
        }
      }
    }
    showLEDs();
    animDelay(30);
  }
  clearAllLEDs();
  delete stopFlag;
}

void BoardDriver::doSolid(std::atomic<bool>* stopFlag) {
  for (int row = 0; row < NUM_ROWS; row++)
    for (int col = 0; col < NUM_COLS; col++)
      setSquareLED(row, col, idleColor);
  showLEDs();
  while (!stopFlag || !stopFlag->load())
    animDelay(100);
  clearAllLEDs();
  delete stopFlag;
}

void BoardDriver::doFire(std::atomic<bool>* stopFlag) {
  float heat[NUM_ROWS][NUM_COLS] = {};
  uint32_t seed = 54321;
  auto rng  = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  auto rngf = [&]() -> float    { return (float)(rng()&0xFFFF)/65535.0f; };
  float baseHue = idleHue(0.0f);
  float sat     = idleSaturation / 100.0f;
  while (!stopFlag || !stopFlag->load()) {
    for (int c = 0; c < NUM_COLS; c++) heat[NUM_ROWS-1][c] = 0.5f + rngf()*0.5f;
    for (int r = NUM_ROWS-2; r >= 0; r--)
      for (int c = 0; c < NUM_COLS; c++) {
        float avg = (heat[r+1][c] + heat[r+1][(c-1+NUM_COLS)%NUM_COLS] +
                     heat[r+1][(c+1)%NUM_COLS] + heat[r][c]) * 0.25f;
        heat[r][c] = fmax(0.0f, avg - 0.05f - rngf()*0.04f);
      }
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) {
      float h = heat[r][c];
      LedRGB col;
      if      (h < 0.3f) col = hsvToRgb(baseHue,       sat, h/0.3f*0.4f);
      else if (h < 0.6f) col = hsvToRgb(baseHue + (h-0.3f)/0.3f*30.0f, sat, 0.4f+(h-0.3f)/0.3f*0.4f);
      else if (h < 0.85f)col = hsvToRgb(baseHue+30.0f + (h-0.6f)/0.25f*30.0f, sat, 0.8f+(h-0.6f)/0.25f*0.15f);
      else               col = hsvToRgb(baseHue+60.0f, sat*(1.0f-(h-0.85f)/0.15f), 0.95f);
      col.r=(uint8_t)(col.r*dim); col.g=(uint8_t)(col.g*dim); col.b=(uint8_t)(col.b*dim);
      setSquareLED(r, c, col);
    }
    showLEDs();
    animDelay(40);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doPlasma(std::atomic<bool>* stopFlag) {
  float t = 0.0f;
  float hueOff = idleHue(0.0f);
  float sat    = idleSaturation / 100.0f;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) {
      float x = c - 3.5f, y = r - 3.5f;
      float v = sinf(x*0.8f + t)
              + sinf(y*0.8f + t*1.3f)
              + sinf((x+y)*0.5f + t*0.7f)
              + sinf(sqrtf(x*x+y*y)*1.2f - t);
      float hue = fmod(v*45.0f + hueOff + t*20.0f + 360.0f, 360.0f);
      float bri = (0.6f + sinf(v)*0.4f) * dim;
      setSquareLED(r, c, hsvToRgb(hue, sat, bri < 0 ? 0 : bri));
    }
    showLEDs();
    t += 0.05f;
    animDelay(35);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doFireworks(std::atomic<bool>* stopFlag) {
  const float CX = (NUM_COLS - 1) / 2.0f;
  const float CY = (NUM_ROWS - 1) / 2.0f;
  const float MAX_R = sqrtf(CX * CX + CY * CY) + 1.0f; // reach just past the corners

  const int MAX_FW = 4;
  enum Phase { FREE, LAUNCH, BURST };
  struct Firework {
    Phase phase;
    float x, y;     // current head (LAUNCH) / explosion centre (BURST)
    float ty;       // target height where the shell bursts
    float radius;   // expanding ring radius
    float energy;   // 1 -> 0 brightness envelope during the burst
    float hue;
  } fw[MAX_FW];
  for (int i = 0; i < MAX_FW; i++) fw[i].phase = FREE;

  // persistent buffer holds spark afterglow so trails linger and fade
  float buf[NUM_ROWS][NUM_COLS][3] = {};

  uint32_t seed = 0xF00D5;
  auto rng  = [&]() -> uint32_t { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
  auto rngf = [&]() -> float    { return (float)(rng() & 0xFFFF) / 65535.0f; };

  float baseHue   = rgbToHue(idleColor);
  bool  chromatic = !idleRandomColor && rgbSaturation(idleColor) > 0.15f; // tint bursts toward primary; random/white = full rainbow
  float sat       = idleSaturation / 100.0f;
  if (sat < 0.2f) sat = 0.2f;

  auto addPix = [&](int r, int c, float h, float v) {
    if (r < 0 || r >= NUM_ROWS || c < 0 || c >= NUM_COLS || v <= 0.0f) return;
    LedRGB col = hsvToRgb(h, sat, v > 1.0f ? 1.0f : v);
    if (col.r > buf[r][c][0]) buf[r][c][0] = col.r;
    if (col.g > buf[r][c][1]) buf[r][c][1] = col.g;
    if (col.b > buf[r][c][2]) buf[r][c][2] = col.b;
  };

  int spawnTimer = 0;
  while (!stopFlag || !stopFlag->load()) {
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++) {
        buf[r][c][0] *= 0.6f; buf[r][c][1] *= 0.6f; buf[r][c][2] *= 0.6f;
      }

    // launch a new shell from the bottom-centre when a slot frees up
    if (--spawnTimer <= 0) {
      for (int i = 0; i < MAX_FW; i++)
        if (fw[i].phase == FREE) {
          fw[i].phase  = LAUNCH;
          fw[i].x      = CX + (rngf() - 0.5f) * 1.5f;
          fw[i].y      = NUM_ROWS - 0.5f;
          fw[i].ty     = 0.6f + rngf() * (CY - 0.3f); // burst in the upper-centre region
          fw[i].radius = 0.0f;
          fw[i].energy = 1.0f;
          fw[i].hue    = chromatic ? fmod(baseHue + (rngf() - 0.5f) * 80.0f + 360.0f, 360.0f)
                                   : rngf() * 360.0f;
          break;
        }
      spawnTimer = 12 + (int)(rng() % 22);
    }

    for (int i = 0; i < MAX_FW; i++) {
      if (fw[i].phase == LAUNCH) {
        fw[i].y -= 0.55f;
        int hx = (int)roundf(fw[i].x);
        addPix((int)roundf(fw[i].y),     hx, 45.0f, 1.0f);  // gold head
        addPix((int)roundf(fw[i].y) + 1, hx, 30.0f, 0.4f);  // dim tail
        if (fw[i].y <= fw[i].ty) { fw[i].phase = BURST; fw[i].y = fw[i].ty; }
      } else if (fw[i].phase == BURST) {
        fw[i].radius += 0.45f;
        fw[i].energy -= 0.05f;
        float life = fw[i].energy < 0 ? 0 : fw[i].energy;
        if (fw[i].radius < 1.2f) // bright explosion core
          addPix((int)roundf(fw[i].y), (int)roundf(fw[i].x), fw[i].hue, 1.0f);
        const float ringW = 1.2f;
        for (int r = 0; r < NUM_ROWS; r++)
          for (int c = 0; c < NUM_COLS; c++) {
            float dx = c - fw[i].x, dy = r - fw[i].y;
            float dd = fabsf(sqrtf(dx * dx + dy * dy) - fw[i].radius);
            if (dd < ringW) {
              float inten = 1.0f - dd / ringW;
              addPix(r, c, fw[i].hue, inten * inten * life);
            }
          }
        if (fw[i].radius >= MAX_R || fw[i].energy <= 0.0f) fw[i].phase = FREE;
      }
    }

    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++)
        setSquareLED(r, c, LedRGB{(uint8_t)(buf[r][c][0] * dim),
                                  (uint8_t)(buf[r][c][1] * dim),
                                  (uint8_t)(buf[r][c][2] * dim)});
    showLEDs();
    animDelay(33);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doRipple(std::atomic<bool>* stopFlag) {
  const int MAXD = 4;
  struct Drop { float x, y, age, hue; bool active; } d[MAXD] = {};
  uint32_t seed = 0x1357B;
  auto rng  = [&]() -> uint32_t { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
  auto rngf = [&]() -> float    { return (float)(rng() & 0xFFFF) / 65535.0f; };
  float baseHue = rgbSaturation(idleColor) > 0.15f ? rgbToHue(idleColor) : 205.0f; // water blue
  float sat     = idleSaturation / 100.0f;
  if (sat < 0.2f) sat = 0.2f;
  const float MAX_AGE_R = sqrtf((float)(NUM_ROWS * NUM_ROWS + NUM_COLS * NUM_COLS)) + 2.0f;
  int spawnTimer = 0;
  while (!stopFlag || !stopFlag->load()) {
    if (--spawnTimer <= 0) {
      for (int i = 0; i < MAXD; i++)
        if (!d[i].active) {
          d[i].active = true; d[i].age = 0.0f;
          d[i].x = rngf() * (NUM_COLS - 1);
          d[i].y = rngf() * (NUM_ROWS - 1);
          d[i].hue = idleRandomColor ? rngf() * 360.0f : fmod(baseHue + (rngf() - 0.5f) * 40.0f + 360.0f, 360.0f);
          break;
        }
      spawnTimer = 8 + (int)(rng() % 14);
    }
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++) {
        float v = 0.0f, hue = baseHue;
        for (int i = 0; i < MAXD; i++) {
          if (!d[i].active) continue;
          float dx = c - d[i].x, dy = r - d[i].y;
          float dist = sqrtf(dx * dx + dy * dy);
          float front = d[i].age * 0.6f;
          if (dist <= front + 1.0f) {
            float env  = expf(-d[i].age * 0.05f) / (1.0f + dist * 0.45f);
            float term = (0.5f + 0.5f * sinf((front - dist) * 2.2f)) * env;
            if (term > v) { v = term; hue = d[i].hue; }
          }
        }
        setSquareLED(r, c, hsvToRgb(hue, sat, (v > 1 ? 1 : v) * dim));
      }
    showLEDs();
    for (int i = 0; i < MAXD; i++)
      if (d[i].active) { d[i].age += 1.0f; if (d[i].age * 0.6f > MAX_AGE_R) d[i].active = false; }
    animDelay(34);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doMatrixRain(std::atomic<bool>* stopFlag) {
  float head[NUM_COLS], spd[NUM_COLS]; int len[NUM_COLS]; bool active[NUM_COLS];
  for (int c = 0; c < NUM_COLS; c++) { active[c] = false; head[c] = 0; spd[c] = 0; len[c] = 0; }
  uint32_t seed = 0x2468A;
  auto rng  = [&]() -> uint32_t { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
  auto rngf = [&]() -> float    { return (float)(rng() & 0xFFFF) / 65535.0f; };
  float baseHue = idleHue(120.0f); // matrix green default
  float sat     = idleSaturation / 100.0f;
  if (sat < 0.2f) sat = 0.2f;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++)
        setSquareLED(r, c, LedRGB{0, 0, 0});
    for (int c = 0; c < NUM_COLS; c++) {
      if (!active[c]) {
        if ((rng() % 100) < 8) {
          active[c] = true;
          head[c] = -(float)(rng() % 4);
          spd[c]  = 0.3f + rngf() * 0.45f;
          len[c]  = 3 + (int)(rng() % 4);
        }
        continue;
      }
      head[c] += spd[c];
      for (int t = 0; t <= len[c]; t++) {
        int r = (int)roundf(head[c]) - t;
        if (r < 0 || r >= NUM_ROWS) continue;
        float frac = 1.0f - (float)t / (len[c] + 1);
        if (t == 0)
          setSquareLED(r, c, hsvToRgb(baseHue, sat * 0.4f, dim));         // bright near-white head
        else
          setSquareLED(r, c, hsvToRgb(baseHue, sat, frac * frac * dim));  // fading tail
      }
      if ((int)roundf(head[c]) - len[c] > NUM_ROWS) active[c] = false;
    }
    showLEDs();
    animDelay(60);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doAurora(std::atomic<bool>* stopFlag) {
  float t = 0.0f;
  float baseHue = idleHue(140.0f); // green-cyan default
  float sat     = idleSaturation / 100.0f;
  if (sat < 0.2f) sat = 0.4f;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++) {
        float curtain  = sinf(c * 0.8f + t) + 0.6f * sinf(c * 1.7f - t * 1.3f);
        float vertical = sinf(r * 0.7f - t * 0.5f + curtain);
        float bri = 0.18f + 0.5f * (0.5f + 0.5f * vertical);
        bri *= 0.4f + 0.6f * (float)(NUM_ROWS - 1 - r) / (NUM_ROWS - 1); // brighter up high
        float hue = fmod(baseHue + curtain * 25.0f + r * 6.0f + 360.0f, 360.0f);
        setSquareLED(r, c, hsvToRgb(hue, sat, (bri > 1 ? 1 : bri) * dim));
      }
    showLEDs();
    t += 0.05f;
    animDelay(40);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doSpiral(std::atomic<bool>* stopFlag) {
  const float CX = (NUM_COLS - 1) / 2.0f;
  const float CY = (NUM_ROWS - 1) / 2.0f;
  const int ARMS = 2;
  float ang = 0.0f;
  float baseHue  = rgbToHue(idleColor);
  bool  chromatic = !idleRandomColor && rgbSaturation(idleColor) > 0.15f;
  float sat = idleSaturation / 100.0f;
  if (sat < 0.2f) sat = 0.2f;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++) {
        float dx = c - CX, dy = r - CY;
        float dist = sqrtf(dx * dx + dy * dy);
        float phase = atan2f(dy, dx) * ARMS - dist * 0.9f + ang;
        float v = 0.5f + 0.5f * sinf(phase);
        v = v * v * v; // crisp arms
        float hue = chromatic ? fmod(baseHue + dist * 18.0f + 360.0f, 360.0f)
                              : fmod(dist * 30.0f + ang * 30.0f + 360.0f, 360.0f);
        setSquareLED(r, c, hsvToRgb(hue, sat, v * dim));
      }
    showLEDs();
    ang += 0.12f;
    animDelay(33);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doStandoff(std::atomic<bool>* stopFlag) {
  // The two armies on their starting ranks face off: a highlight sweeps across
  // the files while the two sides breathe in opposition. Primary colour = the
  // near army (bottom two ranks), Secondary colour = the far army (top two ranks).
  LedRGB cNear = idleColor;
  LedRGB cFar  = idleColor2;
  float phase = 0.0f;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++) {
        bool far  = (r == 0 || r == 1);
        bool near = (r == NUM_ROWS - 1 || r == NUM_ROWS - 2);
        if (!far && !near) { setSquareLED(r, c, LedRGB{0, 0, 0}); continue; }
        LedRGB base   = far ? cFar : cNear;
        float sideOff = far ? 0.0f : (float)M_PI;                 // sides pulse opposite
        bool backRank = (r == 0 || r == NUM_ROWS - 1);
        float sweep  = 0.5f + 0.5f * sinf(c * 0.7f - phase * 2.0f + sideOff);
        float breath = 0.45f + 0.55f * (0.5f + 0.5f * sinf(phase + sideOff));
        float bri = (0.2f + 0.8f * sweep) * breath;
        if (!backRank) bri *= 0.6f;                               // pawns dimmer than pieces
        bri *= dim;
        if (bri > 1.0f) bri = 1.0f;
        setSquareLED(r, c, LedRGB{(uint8_t)(base.r * bri), (uint8_t)(base.g * bri), (uint8_t)(base.b * bri)});
      }
    showLEDs();
    phase += 0.06f;
    animDelay(33);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doCharge(std::atomic<bool>* stopFlag) {
  // Two armies advance from their back ranks toward the middle, collide in a
  // bright flash, then withdraw. A continuous loop using both idle colours.
  LedRGB cFar  = idleColor2;
  LedRGB cNear = idleColor;
  float t = 0.0f;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    // Position of each army's leading rank: 0..3 → starts at 0/7, marches to 3/4
    // then springs back. Saw-tooth with smoothed apex around the centre clash.
    float phase = fmodf(t, 2.0f);            // 0..2
    float push  = phase < 1.0f ? phase : 2.0f - phase;
    int farLead  = (int)floorf(push * 3.0f); // 0..3
    int nearLead = NUM_ROWS - 1 - farLead;
    // Clash brightness — peaks when armies meet at rows 3/4
    float clash = (push > 0.85f) ? (push - 0.85f) / 0.15f : 0.0f;
    clash *= clash;
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++) {
        LedRGB base = {0, 0, 0};
        float bri = 0.0f;
        if (r <= farLead) {
          base = cFar;
          float depth = (float)(farLead - r) / (float)(farLead + 1);
          bri = 0.35f + 0.55f * (1.0f - depth);
        } else if (r >= nearLead) {
          base = cNear;
          float depth = (float)(r - nearLead) / (float)(NUM_ROWS - nearLead);
          bri = 0.35f + 0.55f * (1.0f - depth);
        }
        // File-sine sparkle so each rank shimmers
        bri *= 0.78f + 0.22f * sinf(c * 1.1f + t * 6.0f);
        // Mix in the clash flash on the middle ranks
        if (clash > 0.0f && r >= 3 && r <= 4) {
          float mix = clash;
          base = LedRGB{
            (uint8_t)fminf(255.0f, base.r + mix * (255 - base.r)),
            (uint8_t)fminf(255.0f, base.g + mix * (255 - base.g)),
            (uint8_t)fminf(255.0f, base.b + mix * (255 - base.b))
          };
          bri = fmaxf(bri, 0.6f + 0.4f * mix);
        }
        bri *= dim;
        if (bri < 0.0f) bri = 0.0f;
        if (bri > 1.0f) bri = 1.0f;
        setSquareLED(r, c, LedRGB{(uint8_t)(base.r * bri), (uint8_t)(base.g * bri), (uint8_t)(base.b * bri)});
      }
    showLEDs();
    t += 0.025f;
    animDelay(40);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doShieldwall(std::atomic<bool>* stopFlag) {
  // Both armies hold the line: each side breathes in alternation while a
  // softer light walks file-by-file along their shield wall. The middle four
  // ranks stay dark — the no-man's-land between two armies.
  LedRGB cFar  = idleColor2;
  LedRGB cNear = idleColor;
  float t = 0.0f;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    float breathFar  = 0.5f + 0.5f * sinf(t);
    float breathNear = 0.5f + 0.5f * sinf(t + (float)M_PI);
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++) {
        bool far  = (r <= 1);
        bool near = (r >= NUM_ROWS - 2);
        if (!far && !near) { setSquareLED(r, c, LedRGB{0, 0, 0}); continue; }
        LedRGB base = far ? cFar : cNear;
        float breath = far ? breathFar : breathNear;
        // Walking spotlight along the wall, opposite directions per side
        float spotPos = far ? fmodf(t * 1.6f, (float)NUM_COLS)
                            : fmodf((float)NUM_COLS - fmodf(t * 1.6f, (float)NUM_COLS), (float)NUM_COLS);
        float dx = fabsf((float)c - spotPos);
        if (dx > NUM_COLS / 2.0f) dx = NUM_COLS - dx;
        float spot = expf(-dx * dx * 0.6f);
        float bri = 0.20f + 0.55f * breath + 0.35f * spot;
        if ((r == 1 || r == NUM_ROWS - 2)) bri *= 0.65f;  // pawn rank dimmer
        bri *= dim;
        if (bri > 1.0f) bri = 1.0f;
        setSquareLED(r, c, LedRGB{(uint8_t)(base.r * bri), (uint8_t)(base.g * bri), (uint8_t)(base.b * bri)});
      }
    showLEDs();
    t += 0.055f;
    animDelay(40);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doCourt(std::atomic<bool>* stopFlag) {
  // The royal court: king (e-file = col 4) and queen (d-file = col 3) on both
  // back ranks glow brightest; light radiates outward across the board, slowly
  // pulsing. The two idle colours mark the two crowns.
  LedRGB cFar  = idleColor2;
  LedRGB cNear = idleColor;
  float t = 0.0f;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    float pulse = 0.55f + 0.45f * sinf(t * 1.6f);
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++) {
        // Distance to nearest royal square (d/e on either back rank)
        float dRow1 = (float)r;                          // far back rank = 0
        float dRow2 = (float)(NUM_ROWS - 1 - r);         // near back rank = 7
        float dCol  = fminf(fabsf(c - 3.0f), fabsf(c - 4.0f));
        float dFar  = sqrtf(dRow1 * dRow1 + dCol * dCol);
        float dNear = sqrtf(dRow2 * dRow2 + dCol * dCol);
        // Halo intensity from each crown, falling off with distance
        float haloFar  = expf(-dFar  * 0.55f);
        float haloNear = expf(-dNear * 0.55f);
        // Slow rotating shimmer so it doesn't sit dead-still
        float shimmer = 0.85f + 0.15f * sinf(c * 0.9f + r * 0.4f + t * 1.2f);
        float briFar  = haloFar  * pulse * shimmer;
        float briNear = haloNear * (1.5f - pulse) * shimmer;
        float total = briFar + briNear;
        if (total < 0.02f) { setSquareLED(r, c, LedRGB{0, 0, 0}); continue; }
        float mix = briFar / (briFar + briNear + 1e-6f);
        LedRGB col = {
          (uint8_t)(cFar.r * mix + cNear.r * (1.0f - mix)),
          (uint8_t)(cFar.g * mix + cNear.g * (1.0f - mix)),
          (uint8_t)(cFar.b * mix + cNear.b * (1.0f - mix))
        };
        float bri = fminf(1.0f, total) * dim;
        setSquareLED(r, c, LedRGB{(uint8_t)(col.r * bri), (uint8_t)(col.g * bri), (uint8_t)(col.b * bri)});
      }
    showLEDs();
    t += 0.05f;
    animDelay(40);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doKnightTour(std::atomic<bool>* stopFlag) {
  static const int KM[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
  int8_t order[NUM_ROWS * NUM_COLS][2];
  int pathLen = 1;
  {
    bool vis[NUM_ROWS][NUM_COLS] = {};
    int kr = 0, kc = 0;
    vis[0][0] = true; order[0][0] = 0; order[0][1] = 0;
    for (int step = 1; step < NUM_ROWS * NUM_COLS; step++) {
      int bestR = -1, bestC = -1, bestDeg = 99;
      for (int m = 0; m < 8; m++) {
        int nr = kr + KM[m][0], nc = kc + KM[m][1];
        if (nr < 0 || nr >= NUM_ROWS || nc < 0 || nc >= NUM_COLS || vis[nr][nc]) continue;
        int deg = 0;
        for (int k = 0; k < 8; k++) {
          int ar = nr + KM[k][0], ac = nc + KM[k][1];
          if (ar >= 0 && ar < NUM_ROWS && ac >= 0 && ac < NUM_COLS && !vis[ar][ac]) deg++;
        }
        if (deg < bestDeg) { bestDeg = deg; bestR = nr; bestC = nc; }
      }
      if (bestR < 0) break;
      kr = bestR; kc = bestC; vis[kr][kc] = true;
      order[step][0] = kr; order[step][1] = kc; pathLen = step + 1;
    }
  }
  float glow[NUM_ROWS][NUM_COLS] = {};
  int idx = 0, frameCnt = 0;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) glow[r][c] *= 0.82f;
    if (++frameCnt >= 3) { frameCnt = 0; idx = (idx + 1) % pathLen; glow[order[idx][0]][order[idx][1]] = 1.0f; }
    float hue = fmod((float)idx / pathLen * 360.0f, 360.0f);
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) {
      float g = glow[r][c];
      setSquareLED(r, c, g > 0.01f ? hsvToRgb(hue, 1.0f, g * dim) : LedRGB{0, 0, 0});
    }
    setSquareLED(order[idx][0], order[idx][1], hsvToRgb(hue, 0.15f, dim)); // bright head
    showLEDs();
    animDelay(45);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doReplay(std::atomic<bool>* stopFlag) {
  // First 16 plies of Morphy's "Opera Game" (1858). {fromRow,fromCol,toRow,toCol}, row 0 = rank 8.
  static const int8_t MV[16][4] = {
    {6,4,4,4},{1,4,3,4},{7,6,5,5},{1,3,2,3},{6,3,4,3},{0,2,4,6},
    {4,3,3,4},{4,6,5,5},{7,3,5,5},{2,3,3,4},{7,5,4,2},{0,6,2,5},
    {5,5,5,1},{0,3,1,4},{7,1,5,2},{1,2,2,2}
  };
  const int NMV = 16;
  float glow[NUM_ROWS][NUM_COLS] = {};
  int mv = 0, pause = 0; float t = 0.0f;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) glow[r][c] *= 0.78f;
    LedRGB col = idleRandomColor ? hsvToRgb(fmod(mv * 67.0f, 360.0f), 0.9f, 1.0f) : ((mv % 2 == 0) ? idleColor : idleColor2);
    if (pause > 0) {
      pause--;
    } else {
      float fr = MV[mv][0], fc = MV[mv][1], tr = MV[mv][2], tc = MV[mv][3];
      int rr = (int)roundf(fr + (tr - fr) * t), cc = (int)roundf(fc + (fc == fc ? (tc - fc) * t : 0));
      if (rr >= 0 && rr < NUM_ROWS && cc >= 0 && cc < NUM_COLS) glow[rr][cc] = 1.0f;
      t += 0.12f;
      if (t >= 1.0f) { glow[(int)tr][(int)tc] = 1.0f; t = 0.0f; mv = (mv + 1) % NMV; pause = 8; }
    }
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) {
      float g = glow[r][c];
      setSquareLED(r, c, g > 0.01f ? LedRGB{(uint8_t)(col.r*g*dim),(uint8_t)(col.g*g*dim),(uint8_t)(col.b*g*dim)} : LedRGB{0,0,0});
    }
    showLEDs();
    animDelay(40);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doAttack(std::atomic<bool>* stopFlag) {
  struct Demo { int r, c, type; float hue; }; // type: 0 knight,1 bishop,2 rook,3 queen,4 king
  static const Demo demos[] = { {4,4,0,60.0f},{3,3,1,200.0f},{4,3,2,330.0f},{3,4,3,20.0f},{4,4,4,120.0f} };
  const int NDEMO = 5;
  bool reach[NUM_ROWS][NUM_COLS];
  auto computeReach = [&](const Demo& d) {
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) reach[r][c] = false;
    if (d.type == 0) {
      static const int KM[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
      for (int m = 0; m < 8; m++) { int nr=d.r+KM[m][0], nc=d.c+KM[m][1]; if(nr>=0&&nr<NUM_ROWS&&nc>=0&&nc<NUM_COLS) reach[nr][nc]=true; }
    } else if (d.type == 4) {
      for (int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++){ if(!dr&&!dc)continue; int nr=d.r+dr,nc=d.c+dc; if(nr>=0&&nr<NUM_ROWS&&nc>=0&&nc<NUM_COLS) reach[nr][nc]=true; }
    } else {
      static const int DIAG[4][2]={{-1,-1},{-1,1},{1,-1},{1,1}};
      static const int ORTH[4][2]={{-1,0},{1,0},{0,-1},{0,1}};
      if (d.type==1||d.type==3) for(int k=0;k<4;k++){ int nr=d.r,nc=d.c; for(;;){ nr+=DIAG[k][0]; nc+=DIAG[k][1]; if(nr<0||nr>=NUM_ROWS||nc<0||nc>=NUM_COLS)break; reach[nr][nc]=true; } }
      if (d.type==2||d.type==3) for(int k=0;k<4;k++){ int nr=d.r,nc=d.c; for(;;){ nr+=ORTH[k][0]; nc+=ORTH[k][1]; if(nr<0||nr>=NUM_ROWS||nc<0||nc>=NUM_COLS)break; reach[nr][nc]=true; } }
    }
  };
  int di = 0, hold = 0; float phase = 0.0f;
  computeReach(demos[0]);
  float demoHue = idleRandomColor ? (float)(esp_random() % 360) : demos[0].hue;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    float pulse = 0.35f + 0.65f * (0.5f + 0.5f * sinf(phase));
    const Demo& d = demos[di];
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) {
      if (r == d.r && c == d.c)      setSquareLED(r, c, hsvToRgb(demoHue, 0.2f, dim));
      else if (reach[r][c])          setSquareLED(r, c, hsvToRgb(demoHue, 1.0f, pulse * dim));
      else                           setSquareLED(r, c, LedRGB{0, 0, 0});
    }
    showLEDs();
    phase += 0.12f;
    if (++hold >= 90) { hold = 0; di = (di + 1) % NDEMO; computeReach(demos[di]); demoHue = idleRandomColor ? (float)(esp_random() % 360) : demos[di].hue; }
    animDelay(33);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doTouch(std::atomic<bool>* stopFlag) {
  const int MAXR = 6;
  struct Ripple { float x, y, age, hue; bool active; } rp[MAXR] = {};
  bool prev[NUM_ROWS][NUM_COLS] = {};
  float baseHue = rgbSaturation(idleColor) > 0.15f ? rgbToHue(idleColor) : 195.0f;
  float sat = idleSaturation / 100.0f; if (sat < 0.2f) sat = 0.6f;
  float tm = 0.0f;
  uint32_t seed = 0x5151;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) {
      bool cur = sensorState[r][c];
      if (cur && !prev[r][c])
        for (int i = 0; i < MAXR; i++) if (!rp[i].active) { rp[i].active=true; rp[i].x=c; rp[i].y=r; rp[i].age=0; rp[i].hue = idleRandomColor ? (float)(rng()%360) : fmod(baseHue + (float)(rng()%60) - 30.0f + 360.0f, 360.0f); break; }
      prev[r][c] = cur;
    }
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) {
      float v = 0.05f + 0.05f * (0.5f + 0.5f * sinf(tm + (r + c) * 0.3f)); // faint ambient
      float hue = baseHue;
      if (sensorState[r][c]) { v = 1.0f; }
      for (int i = 0; i < MAXR; i++) {
        if (!rp[i].active) continue;
        float dx = c - rp[i].x, dy = r - rp[i].y;
        float dist = sqrtf(dx*dx + dy*dy);
        float front = rp[i].age * 0.6f;
        if (dist <= front + 1.0f) {
          float env = expf(-rp[i].age * 0.06f) / (1.0f + dist * 0.4f);
          float term = (0.5f + 0.5f * sinf((front - dist) * 2.2f)) * env;
          if (term > v) { v = term; hue = rp[i].hue; }
        }
      }
      setSquareLED(r, c, hsvToRgb(hue, sat, (v > 1 ? 1 : v) * dim));
    }
    showLEDs();
    for (int i = 0; i < MAXR; i++) if (rp[i].active) { rp[i].age += 1.0f; if (rp[i].age * 0.6f > 13.0f) rp[i].active = false; }
    tm += 0.05f;
    animDelay(34);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doLife(std::atomic<bool>* stopFlag) {
  bool cur[NUM_ROWS][NUM_COLS], nxt[NUM_ROWS][NUM_COLS];
  uint8_t age[NUM_ROWS][NUM_COLS];
  uint32_t seed = 0xA11FE;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  float baseHue = idleHue(120.0f);
  float sat = idleSaturation / 100.0f; if (sat < 0.2f) sat = 0.8f;
  auto reseed = [&]() { baseHue = idleHue(120.0f); for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++){ cur[r][c]=(rng()%100)<38; age[r][c]=cur[r][c]?1:0; } };
  reseed();
  int frameCnt = 0, stable = 0, gens = 0;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    if (++frameCnt >= 7) {
      frameCnt = 0;
      int pop = 0; bool changed = false;
      for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) {
        int nb = 0;
        for (int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++){ if(!dr&&!dc)continue; int rr=(r+dr+NUM_ROWS)%NUM_ROWS, cc=(c+dc+NUM_COLS)%NUM_COLS; if(cur[rr][cc])nb++; }
        nxt[r][c] = cur[r][c] ? (nb==2||nb==3) : (nb==3);
        if (nxt[r][c]) pop++;
        if (nxt[r][c] != cur[r][c]) changed = true;
      }
      for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++){ cur[r][c]=nxt[r][c]; if(cur[r][c]){ if(age[r][c]<255)age[r][c]++; } else age[r][c]=0; }
      gens++;
      if (!changed) stable++; else stable = 0;
      if (pop == 0 || stable >= 3 || gens > 250) { reseed(); stable = 0; gens = 0; }
    }
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) {
      if (cur[r][c]) { float h = fmod(baseHue + age[r][c] * 8.0f, 360.0f); float b = 0.4f + 0.6f * (age[r][c] > 8 ? 1.0f : age[r][c] / 8.0f); setSquareLED(r, c, hsvToRgb(h, sat, b * dim)); }
      else setSquareLED(r, c, LedRGB{0, 0, 0});
    }
    showLEDs();
    animDelay(33);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doSand(std::atomic<bool>* stopFlag) {
  uint8_t grid[NUM_ROWS][NUM_COLS] = {};
  uint32_t seed = 0x5A4D;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  LedRGB c1, c2;
  auto rollSandColors = [&]() {
    c1 = idleRandomColor ? hsvToRgb((float)(esp_random() % 360), 0.9f, 1.0f) : (rgbSaturation(idleColor) > 0.15f ? idleColor : LedRGB{255,180,60});
    c2 = idleRandomColor ? hsvToRgb((float)(esp_random() % 360), 0.9f, 1.0f) : idleColor2;
  };
  rollSandColors();
  int settled = 0, spawnTimer = 0; unsigned int colorTog = 0;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    if (--spawnTimer <= 0) { int c = rng() % NUM_COLS; if (!grid[0][c]) grid[0][c] = (colorTog++ & 1) ? 2 : 1; spawnTimer = 1; }
    bool moved = false;
    for (int r = NUM_ROWS - 2; r >= 0; r--) for (int c = 0; c < NUM_COLS; c++) {
      if (!grid[r][c]) continue;
      if (!grid[r+1][c]) { grid[r+1][c] = grid[r][c]; grid[r][c] = 0; moved = true; }
      else {
        bool left  = c > 0           && !grid[r+1][c-1] && !grid[r][c-1];
        bool right = c < NUM_COLS - 1 && !grid[r+1][c+1] && !grid[r][c+1];
        if (left && right) { if (rng() & 1) grid[r+1][c-1] = grid[r][c]; else grid[r+1][c+1] = grid[r][c]; grid[r][c] = 0; moved = true; }
        else if (left)  { grid[r+1][c-1] = grid[r][c]; grid[r][c] = 0; moved = true; }
        else if (right) { grid[r+1][c+1] = grid[r][c]; grid[r][c] = 0; moved = true; }
      }
    }
    if (!moved) { if (++settled > 25) { for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++) grid[r][c]=0; settled = 0; rollSandColors(); } }
    else settled = 0;
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) {
      LedRGB g = (grid[r][c]==1) ? c1 : (grid[r][c]==2 ? c2 : LedRGB{0,0,0});
      setSquareLED(r, c, LedRGB{(uint8_t)(g.r*dim),(uint8_t)(g.g*dim),(uint8_t)(g.b*dim)});
    }
    showLEDs();
    animDelay(45);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doLava(std::atomic<bool>* stopFlag) {
  const int NB = 3;
  struct Blob { float x, y, vx, vy; } b[NB];
  uint32_t seed = 0x1A7A;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  auto rngf = [&]() -> float { return (float)(rng() & 0xFFFF) / 65535.0f; };
  for (int i = 0; i < NB; i++) { b[i].x=rngf()*(NUM_COLS-1); b[i].y=rngf()*(NUM_ROWS-1); b[i].vx=(rngf()-0.5f)*0.3f; b[i].vy=(rngf()-0.5f)*0.3f; }
  float baseHue = idleHue(15.0f); // lava orange-red default
  float sat = idleSaturation / 100.0f; if (sat < 0.2f) sat = 0.9f;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int i = 0; i < NB; i++) {
      b[i].x += b[i].vx; b[i].y += b[i].vy;
      if (b[i].x < 0) { b[i].x = 0; b[i].vx = -b[i].vx; } if (b[i].x > NUM_COLS-1) { b[i].x = NUM_COLS-1; b[i].vx = -b[i].vx; }
      if (b[i].y < 0) { b[i].y = 0; b[i].vy = -b[i].vy; } if (b[i].y > NUM_ROWS-1) { b[i].y = NUM_ROWS-1; b[i].vy = -b[i].vy; }
    }
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) {
      float field = 0;
      for (int i = 0; i < NB; i++) { float dx = c - b[i].x, dy = r - b[i].y; field += 1.6f / (dx*dx + dy*dy + 0.8f); }
      float v = field * 0.5f; if (v > 1) v = 1;
      setSquareLED(r, c, hsvToRgb(fmod(baseHue + field * 22.0f, 360.0f), sat, v * v * dim));
    }
    showLEDs();
    animDelay(40);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doRain(std::atomic<bool>* stopFlag) {
  float head[NUM_COLS], spd[NUM_COLS]; bool active[NUM_COLS];
  for (int c = 0; c < NUM_COLS; c++) { active[c] = false; head[c] = 0; spd[c] = 0; }
  uint32_t seed = 0x7A1D;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  auto rngf = [&]() -> float { return (float)(rng() & 0xFFFF) / 65535.0f; };
  float baseHue = idleHue(210.0f); // rain blue default
  float sat = idleSaturation / 100.0f; if (sat < 0.2f) sat = 0.7f;
  int lightning = 0, strikeTimer = 60 + (int)(rng() % 120);
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) setSquareLED(r, c, LedRGB{0,0,0});
    for (int c = 0; c < NUM_COLS; c++) {
      if (!active[c]) { if ((rng() % 100) < 12) { active[c] = true; head[c] = -(float)(rng() % 3); spd[c] = 0.6f + rngf() * 0.7f; } continue; }
      head[c] += spd[c];
      for (int t = 0; t <= 2; t++) { int r = (int)roundf(head[c]) - t; if (r < 0 || r >= NUM_ROWS) continue; float b = (t==0?0.9f:(t==1?0.4f:0.15f)); setSquareLED(r, c, hsvToRgb(baseHue, sat, b * dim)); }
      if ((int)roundf(head[c]) - 2 > NUM_ROWS) active[c] = false;
    }
    if (--strikeTimer <= 0) { lightning = 2; strikeTimer = 80 + (int)(rng() % 160); }
    if (lightning > 0) { float lb = (lightning == 2 ? 1.0f : 0.5f) * dim; for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++) setSquareLED(r, c, LedRGB{(uint8_t)(255*lb),(uint8_t)(255*lb),(uint8_t)(255*lb)}); lightning--; }
    showLEDs();
    animDelay(45);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doStarfield(std::atomic<bool>* stopFlag) {
  const float CX = (NUM_COLS-1)/2.0f, CY = (NUM_ROWS-1)/2.0f;
  const float MAXR = sqrtf(CX*CX + CY*CY) + 0.5f;
  const int NS = 14;
  struct Star { float a, rad; } st[NS];
  uint32_t seed = 0x57A2;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  auto rngf = [&]() -> float { return (float)(rng() & 0xFFFF) / 65535.0f; };
  for (int i = 0; i < NS; i++) { st[i].a = rngf() * 6.2832f; st[i].rad = rngf() * MAXR; }
  float tint = idleRandomColor ? (float)(esp_random() % 360) : (rgbSaturation(idleColor) > 0.15f ? rgbToHue(idleColor) : -1.0f);
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) setSquareLED(r, c, LedRGB{0,0,0});
    for (int i = 0; i < NS; i++) {
      st[i].rad += st[i].rad * 0.10f + 0.06f;
      if (st[i].rad > MAXR) { st[i].a = rngf() * 6.2832f; st[i].rad = 0.15f; }
      int c = (int)roundf(CX + cosf(st[i].a) * st[i].rad);
      int r = (int)roundf(CY + sinf(st[i].a) * st[i].rad);
      if (r < 0 || r >= NUM_ROWS || c < 0 || c >= NUM_COLS) continue;
      float b = st[i].rad / MAXR; if (b > 1) b = 1; b *= dim;
      setSquareLED(r, c, tint < 0 ? LedRGB{(uint8_t)(255*b),(uint8_t)(255*b),(uint8_t)(255*b)} : hsvToRgb(tint, 0.5f, b));
    }
    showLEDs();
    animDelay(40);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doWallclock(std::atomic<bool>* stopFlag) {
  // Round analog clock showing the real (NTP) time. 12 o'clock = up (row 0 side).
  const float CX = (NUM_COLS - 1) / 2.0f;
  const float CY = (NUM_ROWS - 1) / 2.0f;
  const float R  = 3.3f;
  const float TWO_PI_F = 6.2831853f;
  LedRGB handCol = rgbSaturation(idleColor)  > 0.15f ? idleColor  : LedRGB{255, 255, 255};
  LedRGB secCol  = rgbSaturation(idleColor2) > 0.15f ? idleColor2 : LedRGB{255, 40, 40};
  uint32_t fallback = 0;
  auto drawHand = [&](float ang, float len, LedRGB col, float bright, float dim) {
    for (float t = 0.4f; t <= len; t += 0.3f) {
      int c = (int)roundf(CX + sinf(ang) * t);
      int r = (int)roundf(CY - cosf(ang) * t);
      if (r >= 0 && r < NUM_ROWS && c >= 0 && c < NUM_COLS)
        strip->SetPixelColor(getPixelIndex(r, c), RgbColor((uint8_t)(col.r * bright * dim), (uint8_t)(col.g * bright * dim), (uint8_t)(col.b * bright * dim)));
    }
  };
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) strip->SetPixelColor(getPixelIndex(r, c), RgbColor(0));
    // round dial outline + 12/3/6/9 markers
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) {
      float d = sqrtf((c - CX) * (c - CX) + (r - CY) * (r - CY));
      if (fabsf(d - R) < 0.55f) strip->SetPixelColor(getPixelIndex(r, c), RgbColor((uint8_t)(12 * dim), (uint8_t)(12 * dim), (uint8_t)(20 * dim)));
    }
    for (int m = 0; m < 4; m++) {
      float a = m * (TWO_PI_F / 4.0f);
      int c = (int)roundf(CX + sinf(a) * R), r = (int)roundf(CY - cosf(a) * R);
      if (r >= 0 && r < NUM_ROWS && c >= 0 && c < NUM_COLS) strip->SetPixelColor(getPixelIndex(r, c), RgbColor((uint8_t)(40 * dim), (uint8_t)(40 * dim), (uint8_t)(60 * dim)));
    }
    int sec, minu, hour; struct tm ti;
    if (getLocalTime(&ti, 5)) { sec = ti.tm_sec; minu = ti.tm_min; hour = ti.tm_hour; }
    else { fallback++; sec = (fallback / 5) % 60; minu = (fallback / 300) % 60; hour = (int)((fallback / 18000) % 24); }
    float aHour = (((hour % 12) + minu / 60.0f) / 12.0f) * TWO_PI_F;
    float aMin  = ((minu + sec / 60.0f) / 60.0f) * TWO_PI_F;
    float aSec  = (sec / 60.0f) * TWO_PI_F;
    drawHand(aHour, 1.8f, handCol, 1.0f, dim);
    drawHand(aMin,  2.8f, handCol, 0.75f, dim);
    drawHand(aSec,  3.1f, secCol,  1.0f, dim);
    strip->SetPixelColor(getPixelIndex((int)roundf(CY), (int)roundf(CX)), RgbColor((uint8_t)(handCol.r * dim), (uint8_t)(handCol.g * dim), (uint8_t)(handCol.b * dim)));
    showLEDs();
    animDelay(200);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doText(std::atomic<bool>* stopFlag) {
  // 5x7 column-major font lives at file scope (TEXT_FONT) so the boot status
  // messages can reuse it. Local aliases keep the rendering code below unchanged.
  const auto& FONT = TEXT_FONT;
  auto glyphIndex = [&](char ch) -> int { return textGlyphIndex(ch); };
  const int CW = 6;                                 // 5px glyph + 1px spacing
  const int colOff = (NUM_COLS - 5) / 2;            // centre the 5px glyph for flash mode
  auto blitCol = [&](int c, uint8_t bits, bool inText, LedRGB fg, LedRGB bg, float dim) {
    for (int r = 0; r < NUM_ROWS; r++) {
      bool on = inText && r < 7 && (bits & (1 << r));
      LedRGB col = on ? fg : bg;
      strip->SetPixelColor(getPixelIndex(r, c), RgbColor((uint8_t)(col.r * dim), (uint8_t)(col.g * dim), (uint8_t)(col.b * dim)));
    }
  };
  if (textMode == 1) {
    // Flash each letter centred, one at a time (7 frames on, 2 frames blank).
    int idx = 0;
    while (!stopFlag || !stopFlag->load()) {
      int len = (int)strlen(idleText); if (len < 1) len = 1;
      const uint8_t* g = FONT[glyphIndex(idleText[idx % len])];
      for (int phase = 0; phase < 9 && (!stopFlag || !stopFlag->load()); phase++) {
        bool show = phase < 7;
        float dim = getAutoDimFactor();
        LedRGB fg = textColor, bg = textBg;
        for (int c = 0; c < NUM_COLS; c++) {
          int gc = c - colOff;
          bool inText = show && gc >= 0 && gc < 5;
          blitCol(c, inText ? g[gc] : 0, inText, fg, bg, dim);
        }
        showLEDs();
        animDelay(70);
      }
      idx = (idx + 1) % len;
    }
  } else {
    float scroll = -(float)NUM_COLS;                // start fully off the right edge
    while (!stopFlag || !stopFlag->load()) {
      int len = (int)strlen(idleText);
      int totalW = len * CW; if (totalW < 1) totalW = 1;
      LedRGB fg = textColor, bg = textBg;
      float dim = getAutoDimFactor();
      int sx = (int)scroll;
      for (int c = 0; c < NUM_COLS; c++) {
        int srcCol = c + sx;
        uint8_t bits = 0; bool inText = false;
        if (srcCol >= 0 && srcCol < totalW) {
          int colInChar = srcCol % CW;
          if (colInChar < 5) { bits = FONT[glyphIndex(idleText[srcCol / CW])][colInChar]; inText = true; }
        }
        blitCol(c, bits, inText, fg, bg, dim);
      }
      showLEDs();
      scroll += 1.0f;
      if (scroll >= (float)totalW) scroll = -(float)NUM_COLS;
      animDelay(90);
    }
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doPaint(std::atomic<bool>* stopFlag) {
  // Render the user-painted grid faithfully (no checkerboard dim). Loops so live
  // edits via the /paint endpoint appear and the stop flag is polled.
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < NUM_COLS; c++) {
        LedRGB col = paintGrids[paintSel][r][c];
        strip->SetPixelColor(getPixelIndex(r, c), RgbColor((uint8_t)(col.r * dim), (uint8_t)(col.g * dim), (uint8_t)(col.b * dim)));
      }
    showLEDs();
    animDelay(80);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doTetris(std::atomic<bool>* stopFlag) {
  // Auto-playing Tetris: pieces fall in a random column+rotation, lock, full lines clear.
  static const int8_t SHAPES[7][4][2] = {
    {{0,0},{0,1},{0,2},{0,3}}, // I
    {{0,0},{0,1},{1,0},{1,1}}, // O
    {{0,1},{1,0},{1,1},{1,2}}, // T
    {{0,0},{1,0},{1,1},{1,2}}, // J
    {{0,2},{1,0},{1,1},{1,2}}, // L
    {{0,1},{0,2},{1,0},{1,1}}, // S
    {{0,0},{0,1},{1,1},{1,2}}, // Z
  };
  static const float HUES[7] = {180, 60, 285, 240, 30, 120, 0}; // I O T J L S Z
  uint8_t stack[NUM_ROWS][NUM_COLS] = {};
  uint32_t seed = 0x7E751;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  int curShape = 0, curRot = 0, curR = 0, curC = 0;
  auto cell = [&](int shape, int rot, int k, int& orr, int& occ) {
    int dr = SHAPES[shape][k][0], dc = SHAPES[shape][k][1];
    for (int i = 0; i < rot; i++) { int ndr = dc, ndc = -dr; dr = ndr; dc = ndc; }
    orr = dr; occ = dc;
  };
  auto collides = [&](int shape, int rot, int r, int c) -> bool {
    for (int k = 0; k < 4; k++) { int dr, dc; cell(shape, rot, k, dr, dc); int rr = r + dr, cc = c + dc;
      if (cc < 0 || cc >= NUM_COLS || rr >= NUM_ROWS) return true;
      if (rr >= 0 && stack[rr][cc]) return true; }
    return false;
  };
  auto spawn = [&]() -> bool {
    curShape = rng() % 7; curRot = rng() % 4;
    int minr = 99, minc = 99, maxc = -99;
    for (int k = 0; k < 4; k++) { int dr, dc; cell(curShape, curRot, k, dr, dc); if (dr < minr) minr = dr; if (dc < minc) minc = dc; if (dc > maxc) maxc = dc; }
    int width = maxc - minc + 1;
    curC = (int)(rng() % (NUM_COLS - width + 1)) - minc;
    curR = -minr;
    return !collides(curShape, curRot, curR, curC);
  };
  auto clearLines = [&]() {
    for (int r = NUM_ROWS - 1; r >= 0; r--) {
      bool full = true;
      for (int c = 0; c < NUM_COLS; c++) if (!stack[r][c]) { full = false; break; }
      if (full) { for (int rr = r; rr > 0; rr--) for (int c = 0; c < NUM_COLS; c++) stack[rr][c] = stack[rr-1][c];
        for (int c = 0; c < NUM_COLS; c++) stack[0][c] = 0; r++; }
    }
  };
  bool alive = spawn();
  int dropTimer = 0;
  uint32_t lastSeq = gameSeq, lastRotMs = 0;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    // Player input (joystick / arrow keys): move left/right, soft drop, rotate.
    bool player = (millis() - gameInputMs) < 8000;
    uint32_t s = gameSeq;
    if (alive && player && s != lastSeq) {
      lastSeq = s;
      int d = gameDir;
      if (d == 1)      { if (!collides(curShape, curRot, curR, curC + 1)) curC++; }
      else if (d == 3) { if (!collides(curShape, curRot, curR, curC - 1)) curC--; }
      else if (d == 2) { if (!collides(curShape, curRot, curR + 1, curC)) curR++; }
      else if (d == 0 && millis() - lastRotMs > 250) {
        int nr = (curRot + 1) & 3;
        if (!collides(curShape, nr, curR, curC)) { curRot = nr; lastRotMs = millis(); }
      }
    }
    if (++dropTimer >= 3) {
      dropTimer = 0;
      if (!alive) {
        for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) stack[r][c] = 0;
        alive = spawn();
      } else if (!collides(curShape, curRot, curR + 1, curC)) {
        curR++;
      } else {
        for (int k = 0; k < 4; k++) { int dr, dc; cell(curShape, curRot, k, dr, dc); int rr = curR + dr, cc = curC + dc; if (rr >= 0 && rr < NUM_ROWS && cc >= 0 && cc < NUM_COLS) stack[rr][cc] = curShape + 1; }
        clearLines();
        alive = spawn();
      }
    }
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++)
      setSquareLED(r, c, stack[r][c] ? hsvToRgb(HUES[stack[r][c] - 1], 1.0f, dim) : LedRGB{0, 0, 0});
    if (alive) for (int k = 0; k < 4; k++) { int dr, dc; cell(curShape, curRot, k, dr, dc); int rr = curR + dr, cc = curC + dc; if (rr >= 0 && rr < NUM_ROWS && cc >= 0 && cc < NUM_COLS) setSquareLED(rr, cc, hsvToRgb(HUES[curShape], 1.0f, dim)); }
    showLEDs();
    gameDelay(80);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doAudio(std::atomic<bool>* stopFlag) {
  // Spectrum bars from the browser-mic bands pushed via /audio. Faithful colours.
  float bar[NUM_COLS] = {}, peak[NUM_COLS] = {};
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    bool fresh = (millis() - audioLastMs) < 600;
    for (int c = 0; c < NUM_COLS; c++) {
      float target = fresh ? (audioBands[c] / 255.0f) * NUM_ROWS : 0.0f;
      bar[c] = (target > bar[c]) ? (bar[c] * 0.4f + target * 0.6f) : (bar[c] * 0.78f + target * 0.22f);
      if (bar[c] > peak[c]) peak[c] = bar[c]; else { peak[c] -= 0.12f; if (peak[c] < 0) peak[c] = 0; }
    }
    for (int c = 0; c < NUM_COLS; c++) {
      int h = (int)(bar[c] + 0.5f), pk = (int)(peak[c] + 0.5f);
      for (int r = 0; r < NUM_ROWS; r++) {
        int fromBottom = NUM_ROWS - 1 - r;
        LedRGB col{0, 0, 0};
        if (fromBottom < h) {
          float frac = (NUM_ROWS > 1) ? (float)fromBottom / (NUM_ROWS - 1) : 0.0f;
          col = hsvToRgb(120.0f - frac * 120.0f, 1.0f, 1.0f);
        }
        if (pk > 0 && fromBottom == pk - 1 && fromBottom >= h) col = LedRGB{255, 255, 255};
        strip->SetPixelColor(getPixelIndex(r, c), RgbColor((uint8_t)(col.r * dim), (uint8_t)(col.g * dim), (uint8_t)(col.b * dim)));
      }
    }
    showLEDs();
    animDelay(33);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doSnake(std::atomic<bool>* stopFlag) {
  int8_t bodyR[NUM_ROWS * NUM_COLS], bodyC[NUM_ROWS * NUM_COLS];
  int len, heading = 1; int8_t foodR = 0, foodC = 0;
  uint32_t seed = 0x5A6E;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  auto onBody = [&](int r, int c) -> bool { for (int i = 0; i < len; i++) if (bodyR[i] == r && bodyC[i] == c) return true; return false; };
  auto placeFood = [&]() { int tries = 0; do { foodR = rng() % NUM_ROWS; foodC = rng() % NUM_COLS; } while (onBody(foodR, foodC) && ++tries < 200); };
  auto reset = [&]() { len = 2; bodyR[0] = NUM_ROWS/2; bodyC[0] = NUM_COLS/2; bodyR[1] = bodyR[0]; bodyC[1] = bodyC[0] - 1; heading = 1; placeFood(); };
  reset();
  static const int DRS[4] = {-1, 0, 1, 0}, DCS[4] = {0, 1, 0, -1};
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    bool player = (millis() - gameInputMs) < 8000; // recent arrow input → player steers, else autopilot
    if (player) {
      int want = gameDir;
      if ((want + 2) % 4 != heading) heading = want; // ignore 180° reversal
    } else {
      int best = -1, bestDist = 9999;
      for (int d = 0; d < 4; d++) {
        if ((d + 2) % 4 == heading) continue;
        int nr = bodyR[0] + DRS[d], nc = bodyC[0] + DCS[d];
        if (nr < 0 || nr >= NUM_ROWS || nc < 0 || nc >= NUM_COLS) continue;
        bool blocked = false;
        for (int i = 0; i < len - 1; i++) if (bodyR[i] == nr && bodyC[i] == nc) { blocked = true; break; }
        if (blocked) continue;
        int dist = abs(nr - foodR) + abs(nc - foodC);
        if (dist < bestDist) { bestDist = dist; best = d; }
      }
      if (best >= 0) heading = best;
    }
    int nr = bodyR[0] + DRS[heading], nc = bodyC[0] + DCS[heading];
    bool dead = (nr < 0 || nr >= NUM_ROWS || nc < 0 || nc >= NUM_COLS);
    if (!dead) for (int i = 0; i < len - 1; i++) if (bodyR[i] == nr && bodyC[i] == nc) { dead = true; break; }
    if (dead) reset();
    else {
      bool eat = (nr == foodR && nc == foodC);
      for (int i = (eat ? len : len - 1); i > 0; i--) { bodyR[i] = bodyR[i-1]; bodyC[i] = bodyC[i-1]; }
      bodyR[0] = nr; bodyC[0] = nc;
      if (eat) { if (len < NUM_ROWS * NUM_COLS) len++; placeFood(); }
      if (len >= NUM_ROWS * NUM_COLS) reset();
    }
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) setSquareLED(r, c, LedRGB{0,0,0});
    setSquareLED(foodR, foodC, hsvToRgb(0.0f, 1.0f, dim));
    for (int i = 0; i < len; i++) {
      float v = (i == 0) ? 1.0f : 0.4f + 0.5f * (1.0f - (float)i / len);
      setSquareLED(bodyR[i], bodyC[i], hsvToRgb(120.0f, 1.0f, v * dim));
    }
    showLEDs();
    gameDelay(140);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doPong(std::atomic<bool>* stopFlag) {
  const int PADH = 3;
  float bx = NUM_COLS / 2.0f, by = NUM_ROWS / 2.0f, vx = 0.34f, vy = 0.22f;
  float padL = NUM_ROWS / 2.0f, padR = NUM_ROWS / 2.0f;
  uint32_t seed = 0x9043;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  auto serve = [&](int dir) { bx = NUM_COLS / 2.0f; by = NUM_ROWS / 2.0f; vx = dir * (0.28f + (rng() % 10) * 0.01f); vy = ((rng() & 1) ? 1 : -1) * (0.18f + (rng() % 10) * 0.01f); };
  uint32_t lastSeq = gameSeq;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    bx += vx; by += vy;
    if (by < 0) { by = 0; vy = -vy; } if (by > NUM_ROWS - 1) { by = NUM_ROWS - 1; vy = -vy; }
    float tgt = by - PADH / 2.0f;
    bool player = (millis() - gameInputMs) < 8000;       // left paddle: player up/down, else AI
    uint32_t s = gameSeq;
    if (player) { if (s != lastSeq) { lastSeq = s; if (gameDir == 0) padL -= 1.0f; else if (gameDir == 2) padL += 1.0f; } }
    else padL += (tgt - padL) * 0.25f;
    padR += (tgt - padR) * 0.25f;                        // right paddle always AI
    if (padL < 0) padL = 0; if (padL > NUM_ROWS - PADH) padL = NUM_ROWS - PADH;
    if (padR < 0) padR = 0; if (padR > NUM_ROWS - PADH) padR = NUM_ROWS - PADH;
    if (bx <= 1.0f && vx < 0 && by >= padL - 0.5f && by <= padL + PADH - 0.5f) { vx = -vx; bx = 1.0f; }
    if (bx >= NUM_COLS - 2.0f && vx > 0 && by >= padR - 0.5f && by <= padR + PADH - 0.5f) { vx = -vx; bx = NUM_COLS - 2.0f; }
    if (bx < 0) serve(1); else if (bx > NUM_COLS - 1) serve(-1);
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) setSquareLED(r, c, LedRGB{0,0,0});
    for (int k = 0; k < PADH; k++) {
      int rl = (int)(padL + 0.5f) + k, rr = (int)(padR + 0.5f) + k;
      if (rl >= 0 && rl < NUM_ROWS) setSquareLED(rl, 0, hsvToRgb(210.0f, 1.0f, dim));
      if (rr >= 0 && rr < NUM_ROWS) setSquareLED(rr, NUM_COLS - 1, hsvToRgb(210.0f, 1.0f, dim));
    }
    int brr = (int)(by + 0.5f), bcc = (int)(bx + 0.5f);
    if (brr >= 0 && brr < NUM_ROWS && bcc >= 0 && bcc < NUM_COLS) setSquareLED(brr, bcc, hsvToRgb(50.0f, 1.0f, dim));
    showLEDs();
    gameDelay(60);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doLangton(std::atomic<bool>* stopFlag) {
  bool cell[NUM_ROWS][NUM_COLS] = {};
  int ar = NUM_ROWS / 2, ac = NUM_COLS / 2, adir = 0; // 0 up, 1 right, 2 down, 3 left
  int steps = 0;
  static const int DR[4] = {-1, 0, 1, 0}, DC[4] = {0, 1, 0, -1};
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    if (!cell[ar][ac]) { adir = (adir + 1) & 3; cell[ar][ac] = true; }
    else               { adir = (adir + 3) & 3; cell[ar][ac] = false; }
    ar = (ar + DR[adir] + NUM_ROWS) % NUM_ROWS;
    ac = (ac + DC[adir] + NUM_COLS) % NUM_COLS;
    if (++steps > 500) { for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) cell[r][c] = false; steps = 0; ar = NUM_ROWS / 2; ac = NUM_COLS / 2; adir = 0; }
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++)
      setSquareLED(r, c, cell[r][c] ? hsvToRgb(190.0f, 0.9f, 0.7f * dim) : LedRGB{0, 0, 0});
    setSquareLED(ar, ac, hsvToRgb(40.0f, 1.0f, dim));
    showLEDs();
    animDelay(90);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doBounce(std::atomic<bool>* stopFlag) {
  const int NB = 4;
  struct Ball { float x, y, vx, vy, hue; } b[NB];
  uint32_t seed = 0xB077;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  auto rngf = [&]() -> float { return (float)(rng() & 0xFFFF) / 65535.0f; };
  for (int i = 0; i < NB; i++) { b[i].x = rngf() * (NUM_COLS-1); b[i].y = rngf() * 2; b[i].vx = (rngf()-0.5f) * 0.45f; b[i].vy = 0; b[i].hue = rngf() * 360; }
  float buf[NUM_ROWS][NUM_COLS][3] = {};
  const float G = 0.06f;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++) { buf[r][c][0]*=0.55f; buf[r][c][1]*=0.55f; buf[r][c][2]*=0.55f; }
    for (int i = 0; i < NB; i++) {
      b[i].vy += G; b[i].x += b[i].vx; b[i].y += b[i].vy;
      if (b[i].x < 0) { b[i].x = 0; b[i].vx = -b[i].vx; }
      if (b[i].x > NUM_COLS-1) { b[i].x = NUM_COLS-1; b[i].vx = -b[i].vx; }
      if (b[i].y > NUM_ROWS-1) { b[i].y = NUM_ROWS-1; b[i].vy = -b[i].vy * 0.82f; if (fabsf(b[i].vy) < 0.25f) b[i].vy = -(0.7f + rngf()*0.6f); }
      if (b[i].y < 0) { b[i].y = 0; b[i].vy = -b[i].vy; }
      int rr = (int)roundf(b[i].y), cc = (int)roundf(b[i].x);
      if (rr>=0&&rr<NUM_ROWS&&cc>=0&&cc<NUM_COLS) { LedRGB col = hsvToRgb(b[i].hue, 1.0f, 1.0f); buf[rr][cc][0]=col.r; buf[rr][cc][1]=col.g; buf[rr][cc][2]=col.b; }
    }
    for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++)
      strip->SetPixelColor(getPixelIndex(r,c), RgbColor((uint8_t)(buf[r][c][0]*dim),(uint8_t)(buf[r][c][1]*dim),(uint8_t)(buf[r][c][2]*dim)));
    showLEDs();
    animDelay(33);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doSnow(std::atomic<bool>* stopFlag) {
  const int NF = 6;
  struct Flake { float x, y, vy, ph; } f[NF];
  int pile[NUM_COLS] = {};
  uint32_t seed = 0x5704;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  auto rngf = [&]() -> float { return (float)(rng() & 0xFFFF) / 65535.0f; };
  auto spawn = [&](int i) { f[i].x = rngf()*(NUM_COLS-1); f[i].y = 0; f[i].vy = 0.12f + rngf()*0.12f; f[i].ph = rngf()*6.28f; };
  for (int i = 0; i < NF; i++) { spawn(i); f[i].y = rngf()*NUM_ROWS; }
  float t = 0;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++) setSquareLED(r,c, LedRGB{0,0,0});
    for (int c=0;c<NUM_COLS;c++) for (int k=0;k<pile[c];k++) setSquareLED(NUM_ROWS-1-k, c, LedRGB{(uint8_t)(150*dim),(uint8_t)(175*dim),(uint8_t)(215*dim)});
    for (int i = 0; i < NF; i++) {
      f[i].y += f[i].vy; f[i].x += sinf(t + f[i].ph) * 0.06f;
      if (f[i].x < 0) f[i].x = 0; if (f[i].x > NUM_COLS-1) f[i].x = NUM_COLS-1;
      int c = (int)roundf(f[i].x);
      int landRow = NUM_ROWS - 1 - pile[c];
      if ((int)roundf(f[i].y) >= landRow) { if (pile[c] < NUM_ROWS) pile[c]++; spawn(i); }
      else { int rr=(int)roundf(f[i].y); if (rr>=0&&rr<NUM_ROWS) setSquareLED(rr, c, LedRGB{(uint8_t)(255*dim),(uint8_t)(255*dim),(uint8_t)(255*dim)}); }
    }
    int total = 0; for (int c=0;c<NUM_COLS;c++) total += pile[c];
    if (total >= NUM_ROWS*NUM_COLS - NUM_COLS) for (int c=0;c<NUM_COLS;c++) pile[c]=0;
    showLEDs();
    t += 0.06f;
    animDelay(45);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doBubbles(std::atomic<bool>* stopFlag) {
  const int NB = 6;
  struct Bub { float x, y, vy, ph; bool active; } b[NB] = {};
  uint32_t seed = 0xB0B1;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  auto rngf = [&]() -> float { return (float)(rng() & 0xFFFF) / 65535.0f; };
  int spawnTimer = 0; float t = 0;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++) setSquareLED(r,c, LedRGB{0,0,0});
    if (--spawnTimer <= 0) { for (int i=0;i<NB;i++) if (!b[i].active) { b[i].active=true; b[i].x=rngf()*(NUM_COLS-1); b[i].y=NUM_ROWS-1; b[i].vy=0.12f+rngf()*0.15f; b[i].ph=rngf()*6.28f; break; } spawnTimer = 5 + rng()%10; }
    for (int i = 0; i < NB; i++) {
      if (!b[i].active) continue;
      b[i].y -= b[i].vy; float x = b[i].x + sinf(t*2 + b[i].ph)*0.6f;
      if (b[i].y < -0.5f) { b[i].active = false; continue; }
      int rr=(int)roundf(b[i].y), cc=(int)roundf(x);
      if (rr>=0&&rr<NUM_ROWS&&cc>=0&&cc<NUM_COLS) setSquareLED(rr, cc, hsvToRgb(190.0f, 0.65f, dim));
    }
    showLEDs();
    t += 0.06f;
    animDelay(40);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doSort(std::atomic<bool>* stopFlag) {
  int val[NUM_COLS]; int i = 0, j = 0, pause = 0;
  uint32_t seed = 0x504F;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  auto shuffle = [&]() { for (int k=0;k<NUM_COLS;k++) val[k]=k+1; for (int k=NUM_COLS-1;k>0;k--){ int m=rng()%(k+1); int tmp=val[k]; val[k]=val[m]; val[m]=tmp; } i=0; j=0; };
  shuffle();
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    int hA = -1, hB = -1;
    if (pause > 0) pause--;
    else if (i < NUM_COLS - 1) {
      hA = j; hB = j + 1;
      if (val[j] > val[j+1]) { int tmp=val[j]; val[j]=val[j+1]; val[j+1]=tmp; }
      j++;
      if (j >= NUM_COLS - 1 - i) { j = 0; i++; }
    } else { pause = 25; shuffle(); }
    for (int c = 0; c < NUM_COLS; c++) {
      bool hl = (c == hA || c == hB);
      for (int r = 0; r < NUM_ROWS; r++) {
        int fromBottom = NUM_ROWS - 1 - r;
        LedRGB col{0, 0, 0};
        if (fromBottom < val[c]) col = hl ? LedRGB{255,255,255} : hsvToRgb(200.0f - (float)c/(NUM_COLS-1)*160.0f, 1.0f, 1.0f);
        strip->SetPixelColor(getPixelIndex(r,c), RgbColor((uint8_t)(col.r*dim),(uint8_t)(col.g*dim),(uint8_t)(col.b*dim)));
      }
    }
    showLEDs();
    animDelay(110);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doWave(std::atomic<bool>* stopFlag) {
  float t = 0;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int c = 0; c < NUM_COLS; c++) {
      float surface = (NUM_ROWS-1)/2.0f + sinf(c*0.7f + t)*1.6f + sinf(c*0.33f - t*1.4f)*0.8f;
      for (int r = 0; r < NUM_ROWS; r++) {
        LedRGB col{0,0,0};
        float d = r - surface;
        if (d >= -0.6f) {
          if (d < 0.6f) col = LedRGB{200,240,255};
          else { float v = 0.4f + 0.6f*(1.0f - (r - surface)/NUM_ROWS); if (v<0.1f) v=0.1f; if (v>1) v=1; col = hsvToRgb(205.0f, 0.9f, v); }
        }
        strip->SetPixelColor(getPixelIndex(r,c), RgbColor((uint8_t)(col.r*dim),(uint8_t)(col.g*dim),(uint8_t)(col.b*dim)));
      }
    }
    showLEDs();
    t += 0.12f;
    animDelay(45);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doRule30(std::atomic<bool>* stopFlag) {
  bool hist[NUM_ROWS][NUM_COLS] = {};
  bool row[NUM_COLS] = {};
  row[NUM_COLS/2] = true;
  int steps = 0, frameCnt = 0;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    if (++frameCnt >= 2) {
      frameCnt = 0;
      for (int r = NUM_ROWS-1; r > 0; r--) for (int c=0;c<NUM_COLS;c++) hist[r][c]=hist[r-1][c];
      for (int c=0;c<NUM_COLS;c++) hist[0][c]=row[c];
      bool nxt[NUM_COLS];
      for (int c=0;c<NUM_COLS;c++) { bool L=row[(c-1+NUM_COLS)%NUM_COLS], C=row[c], R=row[(c+1)%NUM_COLS]; nxt[c] = L ^ (C || R); }
      for (int c=0;c<NUM_COLS;c++) row[c]=nxt[c];
      if (++steps > 40) { for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++) hist[r][c]=false; for (int c=0;c<NUM_COLS;c++) row[c]=false; row[NUM_COLS/2]=true; steps=0; }
    }
    for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++)
      setSquareLED(r,c, hist[r][c] ? hsvToRgb(fmod(r*30.0f + 260.0f, 360.0f), 1.0f, dim) : LedRGB{0,0,0});
    showLEDs();
    animDelay(70);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doBreakout(std::atomic<bool>* stopFlag) {
  const int PADW = 3;
  uint8_t bricks[NUM_ROWS][NUM_COLS] = {};
  int brickCount = 0;
  auto setupBricks = [&]() {
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) bricks[r][c] = 0;
    for (int r = 0; r < 3; r++) for (int c = 0; c < NUM_COLS; c++) bricks[r][c] = r + 1;
    brickCount = 3 * NUM_COLS;
  };
  setupBricks();
  float bx = NUM_COLS / 2.0f, by = NUM_ROWS - 2.0f, vx = 0.34f, vy = -0.26f;
  int padCol = NUM_COLS / 2 - PADW / 2;
  uint32_t lastSeq = gameSeq;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    bool player = (millis() - gameInputMs) < 8000;
    uint32_t s = gameSeq;
    if (player && s != lastSeq) {
      lastSeq = s;
      if (gameDir == 3 && padCol > 0) padCol--;
      else if (gameDir == 1 && padCol + PADW < NUM_COLS) padCol++;
    }
    bx += vx; by += vy;
    if (bx < 0) { bx = 0; vx = -vx; }
    if (bx > NUM_COLS - 1) { bx = NUM_COLS - 1; vx = -vx; }
    if (by < 0) { by = 0; vy = -vy; }
    int by_i = (int)roundf(by), bx_i = (int)roundf(bx);
    // paddle bounce
    if (by > NUM_ROWS - 1.5f && vy > 0 && bx_i >= padCol && bx_i < padCol + PADW) {
      vy = -fabsf(vy); by = NUM_ROWS - 1.5f;
      vx += (bx_i - (padCol + PADW / 2.0f)) * 0.06f;
      if (vx > 0.55f) vx = 0.55f; if (vx < -0.55f) vx = -0.55f;
    }
    if (by > NUM_ROWS - 0.5f) { // miss → re-serve
      bx = NUM_COLS / 2.0f; by = NUM_ROWS - 2.0f; vx = 0.34f * ((bx_i & 1) ? 1 : -1); vy = -0.26f;
    }
    if (by_i >= 0 && by_i < NUM_ROWS && bx_i >= 0 && bx_i < NUM_COLS && bricks[by_i][bx_i]) {
      bricks[by_i][bx_i] = 0; brickCount--; vy = -vy;
    }
    if (brickCount <= 0) setupBricks();
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) {
      if (bricks[r][c]) { float hue = bricks[r][c] == 1 ? 0.0f : (bricks[r][c] == 2 ? 30.0f : 60.0f); setSquareLED(r, c, hsvToRgb(hue, 1.0f, dim)); }
      else setSquareLED(r, c, LedRGB{0, 0, 0});
    }
    for (int k = 0; k < PADW; k++) setSquareLED(NUM_ROWS - 1, padCol + k, hsvToRgb(200.0f, 0.9f, dim));
    if (by_i >= 0 && by_i < NUM_ROWS && bx_i >= 0 && bx_i < NUM_COLS) setSquareLED(by_i, bx_i, hsvToRgb(50.0f, 1.0f, dim));
    showLEDs();
    gameDelay(70);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doTron(std::atomic<bool>* stopFlag) {
  bool trail[NUM_ROWS][NUM_COLS] = {};
  int pr = 0, pc = 0, ph = 0, br_ = 0, bc = 0, bh = 0;
  auto reset = [&]() {
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) trail[r][c] = false;
    pr = NUM_ROWS - 1; pc = 1; ph = 0;            // player bottom-left, moving up
    br_ = 0; bc = NUM_COLS - 2; bh = 2;           // bot top-right, moving down
    trail[pr][pc] = true; trail[br_][bc] = true;
  };
  reset();
  static const int DRS[4] = {-1, 0, 1, 0}, DCS[4] = {0, 1, 0, -1};
  auto blockedAt = [&](int r, int c) -> bool { return r < 0 || r >= NUM_ROWS || c < 0 || c >= NUM_COLS || trail[r][c]; };
  uint32_t lastSeq = gameSeq;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    bool player = (millis() - gameInputMs) < 8000;
    uint32_t s = gameSeq;
    if (player && s != lastSeq) { lastSeq = s; int want = gameDir; if ((want + 2) % 4 != ph) ph = want; }
    // bot AI: forward, else turn
    int tryDir[4] = { bh, (bh + 1) & 3, (bh + 3) & 3, (bh + 2) & 3 };
    for (int i = 0; i < 4; i++) { int d = tryDir[i]; if (!blockedAt(br_ + DRS[d], bc + DCS[d])) { bh = d; break; } }
    int pnr = pr + DRS[ph], pnc = pc + DCS[ph];
    int bnr = br_ + DRS[bh], bnc = bc + DCS[bh];
    bool pdead = blockedAt(pnr, pnc), bdead = blockedAt(bnr, bnc);
    if (pdead || bdead || (pnr == bnr && pnc == bnc)) reset();
    else { pr = pnr; pc = pnc; trail[pr][pc] = true; br_ = bnr; bc = bnc; trail[br_][bc] = true; }
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) setSquareLED(r, c, trail[r][c] ? LedRGB{(uint8_t)(40 * dim),(uint8_t)(40 * dim),(uint8_t)(50 * dim)} : LedRGB{0, 0, 0});
    setSquareLED(pr, pc, hsvToRgb(120.0f, 1.0f, dim));
    setSquareLED(br_, bc, hsvToRgb(0.0f, 1.0f, dim));
    showLEDs();
    gameDelay(160);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doCatch(std::atomic<bool>* stopFlag) {
  const int PADW = 3;
  int padCol = NUM_COLS / 2 - PADW / 2;
  const int NI = 4;
  struct Item { float y; int8_t col; float hue; bool active; } it[NI] = {};
  uint32_t seed = 0xCA7C;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  int spawnTimer = 0, flash = 0;
  uint32_t lastSeq = gameSeq;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    bool player = (millis() - gameInputMs) < 8000;
    uint32_t s = gameSeq;
    if (player && s != lastSeq) {
      lastSeq = s;
      if (gameDir == 3 && padCol > 0) padCol--;
      else if (gameDir == 1 && padCol + PADW < NUM_COLS) padCol++;
    }
    if (--spawnTimer <= 0) {
      for (int i = 0; i < NI; i++) if (!it[i].active) { it[i].active = true; it[i].y = -1.0f; it[i].col = rng() % NUM_COLS; it[i].hue = (float)(rng() % 360); break; }
      spawnTimer = 6 + (rng() % 10);
    }
    for (int i = 0; i < NI; i++) {
      if (!it[i].active) continue;
      it[i].y += 0.32f;
      int r = (int)roundf(it[i].y);
      if (r >= NUM_ROWS - 1) {
        if (it[i].col >= padCol && it[i].col < padCol + PADW) flash = 3;
        it[i].active = false;
      }
    }
    for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) setSquareLED(r, c, LedRGB{0, 0, 0});
    for (int k = 0; k < PADW; k++) setSquareLED(NUM_ROWS - 1, padCol + k, hsvToRgb(flash > 0 ? 120.0f : 200.0f, 0.9f, dim));
    if (flash > 0) flash--;
    for (int i = 0; i < NI; i++) if (it[i].active) {
      int rr = (int)roundf(it[i].y);
      if (rr >= 0 && rr < NUM_ROWS) setSquareLED(rr, it[i].col, hsvToRgb(it[i].hue, 1.0f, dim));
    }
    showLEDs();
    gameDelay(90);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doSimon(std::atomic<bool>* stopFlag) {
  const int MAX_SEQ = 32;
  uint8_t seq[MAX_SEQ]; int seqLen = 0, playIdx = 0;
  int phase = 0;                                   // 0 = play sequence, 1 = await input
  uint32_t seed = 0x51AF;
  auto rng = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  static const float HUES[4] = { 0.0f, 120.0f, 220.0f, 50.0f }; // up red, right green, down blue, left yellow
  auto clearAll = [&]() { for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) setSquareLED(r, c, LedRGB{0, 0, 0}); };
  auto renderEdge = [&](int d, float v) {
    clearAll();
    LedRGB col = hsvToRgb(HUES[d], 1.0f, v);
    if (d == 0)      for (int c = 0; c < NUM_COLS; c++) setSquareLED(0, c, col);
    else if (d == 1) for (int r = 0; r < NUM_ROWS; r++) setSquareLED(r, NUM_COLS - 1, col);
    else if (d == 2) for (int c = 0; c < NUM_COLS; c++) setSquareLED(NUM_ROWS - 1, c, col);
    else             for (int r = 0; r < NUM_ROWS; r++) setSquareLED(r, 0, col);
  };
  auto reset = [&]() { seqLen = 0; seq[seqLen++] = rng() % 4; playIdx = 0; phase = 0; };
  reset();
  uint32_t lastSeq = gameSeq;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    if (phase == 0) {
      for (int i = 0; i < seqLen && (!stopFlag || !stopFlag->load()); i++) {
        renderEdge(seq[i], dim); showLEDs(); gameDelay(450);
        clearAll(); showLEDs(); gameDelay(180);
      }
      phase = 1; playIdx = 0; lastSeq = gameSeq;
    } else {
      // dim edge outline so the player sees the four areas
      clearAll();
      for (int c = 0; c < NUM_COLS; c++) { setSquareLED(0, c, hsvToRgb(HUES[0], 1.0f, 0.06f * dim)); setSquareLED(NUM_ROWS - 1, c, hsvToRgb(HUES[2], 1.0f, 0.06f * dim)); }
      for (int r = 0; r < NUM_ROWS; r++) { setSquareLED(r, 0, hsvToRgb(HUES[3], 1.0f, 0.06f * dim)); setSquareLED(r, NUM_COLS - 1, hsvToRgb(HUES[1], 1.0f, 0.06f * dim)); }
      showLEDs();
      uint32_t s = gameSeq;
      if (s != lastSeq) {
        lastSeq = s;
        int d = gameDir;
        if (d == seq[playIdx]) {
          renderEdge(d, dim); showLEDs(); gameDelay(180);
          playIdx++;
          if (playIdx >= seqLen) {
            if (seqLen < MAX_SEQ) seq[seqLen++] = rng() % 4;
            gameDelay(400);
            phase = 0;
          }
        } else {
          for (int k = 0; k < 3; k++) {
            for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_COLS; c++) setSquareLED(r, c, hsvToRgb(0.0f, 1.0f, dim));
            showLEDs(); gameDelay(150);
            clearAll(); showLEDs(); gameDelay(150);
          }
          reset();
        }
      } else gameDelay(50);
    }
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doMeteor(std::atomic<bool>* stopFlag) {
  const int MAX_M = 4;
  struct { float row, col, hue; bool active; } m[MAX_M] = {};
  float fade[NUM_ROWS][NUM_COLS] = {};
  uint32_t seed = 99887;
  auto rng  = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  float baseHue  = rgbToHue(idleColor);
  bool chromatic = !idleRandomColor && rgbSaturation(idleColor) > 0.15f;
  float sat      = idleSaturation / 100.0f;
  int spawnTimer = 0;
  while (!stopFlag || !stopFlag->load()) {
    for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++) fade[r][c] *= 0.75f;
    if (--spawnTimer <= 0) {
      for (int i=0;i<MAX_M;i++) if (!m[i].active) {
        m[i].active = true;
        m[i].row = (float)(rng()%NUM_ROWS); m[i].col = NUM_COLS-1;
        m[i].hue = chromatic ? fmod(baseHue+(int)(rng()%41)-20+360.0f,360.0f) : (float)(rng()%360);
        break;
      }
      spawnTimer = 3 + (int)(rng()%6);
    }
    for (int i=0;i<MAX_M;i++) {
      if (!m[i].active) continue;
      int r=(int)m[i].row, c=(int)m[i].col;
      if (r>=0&&r<NUM_ROWS&&c>=0&&c<NUM_COLS) fade[r][c] = 1.0f;
      m[i].row += 0.4f; m[i].col -= 0.4f;
      if (m[i].row>=NUM_ROWS||m[i].col<0) m[i].active=false;
    }
    float dim = getAutoDimFactor();
    for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++) {
      if (fade[r][c]>0.02f) {
        float h=baseHue;
        for(int i=0;i<MAX_M;i++) if(m[i].active&&(int)m[i].row==r&&(int)m[i].col==c) h=m[i].hue;
        LedRGB px = hsvToRgb(h, sat, fade[r][c]*dim);
        setSquareLED(r,c,px);
      } else setSquareLED(r,c,LedRGB{0,0,0});
    }
    showLEDs();
    animDelay(55);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doChessPulse(std::atomic<bool>* stopFlag) {
  float phase = 0.0f;
  float h1 = rgbToHue(idleColor);
  float h2 = rgbSaturation(idleColor2) > 0.05f ? rgbToHue(idleColor2) : fmod(h1+180.0f,360.0f);
  float sat = idleSaturation / 100.0f;
  while (!stopFlag || !stopFlag->load()) {
    float t   = (sinf(phase)+1.0f)*0.5f;
    float b1  = 0.08f + 0.92f*(t*t);
    float b2  = 0.08f + 0.92f*((1.0f-t)*(1.0f-t));
    float dim = getAutoDimFactor();
    for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++) {
      bool light = (r+c)%2==0;
      LedRGB px = light ? hsvToRgb(h1,sat,b1*dim) : hsvToRgb(h2,sat,b2*dim);
      setSquareLED(r,c,px);
    }
    showLEDs();
    phase += 0.025f;
    if (phase>=2.0f*M_PI) phase-=2.0f*M_PI;
    animDelay(20);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doColorCycle(std::atomic<bool>* stopFlag) {
  float hue = rgbToHue(idleColor);
  float sat = idleSaturation / 100.0f;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    LedRGB c  = hsvToRgb(hue, sat, dim);
    for (int r=0;r<NUM_ROWS;r++) for(int c2=0;c2<NUM_COLS;c2++) setSquareLED(r,c2,c);
    showLEDs();
    hue += 0.6f;
    if (hue>=360.0f) hue-=360.0f;
    animDelay(30);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::doScan(std::atomic<bool>* stopFlag) {
  float pos  = 0.0f;
  float dir  = 1.0f;
  float hue  = rgbToHue(idleColor);
  float sat  = idleSaturation / 100.0f;
  while (!stopFlag || !stopFlag->load()) {
    float dim = getAutoDimFactor();
    for (int r=0;r<NUM_ROWS;r++) for(int c=0;c<NUM_COLS;c++) {
      float dist = fabsf(r - pos);
      float bri  = fmax(0.0f, 1.0f - dist*0.6f) * dim;
      setSquareLED(r, c, bri>0.02f ? hsvToRgb(hue,sat,bri) : LedRGB{0,0,0});
    }
    showLEDs();
    pos += dir*0.25f;
    if (pos>=NUM_ROWS-1) dir=-1.0f;
    if (pos<=0.0f)       dir= 1.0f;
    animDelay(28);
  }
  clearAllLEDs(); delete stopFlag;
}

void BoardDriver::setIdleColor(LedRGB color) {
  idleColor = color;
  animRestartPending.store(true);
}

bool BoardDriver::consumeAnimationRestartRequest() {
  return animRestartPending.exchange(false);
}

void BoardDriver::animDelay(uint32_t baseMs) {
  uint32_t ms = (uint32_t)(baseMs / animSpeed);
  if (ms < 5) ms = 5;
  // Release the LED mutex during the inter-frame delay. Idle animations run their
  // whole loop inside executeAnimation() while the worker holds ledMutex; without
  // this, a web handler calling acquireLEDs() (e.g. /debug-testleds) would block
  // forever and stall the single async-webserver task → the whole UI hangs.
  // animDelay() is only ever called from animation code that holds the mutex.
  xSemaphoreGive(ledMutex);
  vTaskDelay(pdMS_TO_TICKS(ms));
  xSemaphoreTake(ledMutex, portMAX_DELAY);
}

void BoardDriver::gameDelay(uint32_t ms) {
  // Same mutex-release dance as animDelay(), but a fixed cadence so the idle
  // games keep their own tempo regardless of the user's animSpeed slider.
  if (ms < 5) ms = 5;
  xSemaphoreGive(ledMutex);
  vTaskDelay(pdMS_TO_TICKS(ms));
  xSemaphoreTake(ledMutex, portMAX_DELAY);
}

float BoardDriver::idleHue(float fallback) {
  if (idleRandomColor) return (float)(esp_random() % 360);
  return rgbSaturation(idleColor) > 0.15f ? rgbToHue(idleColor) : fallback;
}

void BoardDriver::savePaintGrid() { savePaintSlot(paintSel); }

void BoardDriver::savePaintSlot(int slot) {
  if (slot < 0 || slot >= PAINT_SLOTS) return;
  if (!ChessUtils::ensureNvsInitialized()) return;
  Preferences prefs;
  prefs.begin("ledSettings", false);
  char key[8];
  snprintf(key, sizeof(key), "paint%d", slot);
  prefs.putBytes(key, paintGrids[slot], sizeof(paintGrids[slot]));
  prefs.end();
}

void BoardDriver::setActivePaintSlot(uint8_t s) {
  if (s >= PAINT_SLOTS) return;
  paintSel = s;
  animRestartPending.store(true);
}

void BoardDriver::setPaintName(int slot, const char* n) {
  if (slot < 0 || slot >= PAINT_SLOTS || !n) return;
  strncpy(paintNames[slot], n, sizeof(paintNames[slot]) - 1);
  paintNames[slot][sizeof(paintNames[slot]) - 1] = '\0';
}

void BoardDriver::savePaintMeta() {
  if (!ChessUtils::ensureNvsInitialized()) return;
  Preferences prefs;
  prefs.begin("ledSettings", false);
  prefs.putUChar("paintSel", paintSel);
  char key[8];
  for (int i = 0; i < PAINT_SLOTS; i++) {
    snprintf(key, sizeof(key), "paintN%d", i);
    prefs.putString(key, paintNames[i]);
  }
  prefs.end();
}

void BoardDriver::setAudioBands(const uint8_t* vals, int n) {
  for (int i = 0; i < n && i < NUM_COLS; i++) audioBands[i] = vals[i];
  audioLastMs = millis();
}

void BoardDriver::setGameInput(int dir) {
  if (dir < 0 || dir > 3) return;
  gameDir = dir;
  gameInputMs = millis();
  gameSeq++;
}

float BoardDriver::getAutoDimFactor() const {
  if (!autoDim) return 1.0f;
  struct tm ti;
  if (!getLocalTime(&ti)) return 1.0f;
  int h = ti.tm_hour;
  bool dim = (autoDimStart <= autoDimEnd)
    ? (h >= autoDimStart && h < autoDimEnd)
    : (h >= autoDimStart || h < autoDimEnd);
  if (dim && nightOff) return 0.0f;                   // hard-off during night window
  return dim ? (autoDimBrightness / 255.0f) : 1.0f;
}

// LED settings methods
void BoardDriver::setBrightness(uint8_t value) {
  brightness = value > 255 ? 255 : (value < 10 ? 10 : value);
  if (!calibrating.load()) {
    strip->SetLuminance(brightness);
    showLEDs();
  }
}

void BoardDriver::setDimMultiplier(uint8_t value) {
  dimMultiplier = value > 100 ? 100 : (value < 20 ? 20 : value);
  if (!calibrating.load()) {
    // Re-apply all current colors with new dim multiplier
    for (int row = 0; row < NUM_ROWS; row++)
      for (int col = 0; col < NUM_COLS; col++)
        setSquareLED(row, col, currentColors[row][col]);
    showLEDs();
  }
}

void BoardDriver::loadLedSettings() {
  if (!ChessUtils::ensureNvsInitialized()) {
    Serial.println("NVS init failed - LED settings not loaded");
    return;
  }
  Preferences prefs;
  prefs.begin("ledSettings", false);
  brightness = prefs.getUChar("brightness", BRIGHTNESS);
  dimMultiplier = prefs.getUChar("dimMult", 70);
  idleAnimation = (IdleAnimation)prefs.getUChar("idleAnim", (uint8_t)IdleAnimation::RAINBOW);
  idleColor.r  = prefs.getUChar("idleColorR",  255);
  idleColor.g  = prefs.getUChar("idleColorG",  255);
  idleColor.b  = prefs.getUChar("idleColorB",  255);
  idleColor2.r = prefs.getUChar("idleColor2R", 255);
  idleColor2.g = prefs.getUChar("idleColor2G", 128);
  idleColor2.b = prefs.getUChar("idleColor2B",   0);
  animSpeed        = prefs.getFloat("animSpeed",  1.0f);
  idleSaturation   = prefs.getUChar("idleSat",   100);
  idleRandomColor  = prefs.getBool("randCol",     false);
  textColor.r = prefs.getUChar("txtR", 255); textColor.g = prefs.getUChar("txtG", 255); textColor.b = prefs.getUChar("txtB", 255);
  textBg.r    = prefs.getUChar("txtBgR", 0); textBg.g    = prefs.getUChar("txtBgG", 0); textBg.b    = prefs.getUChar("txtBgB", 0);
  { String t = prefs.getString("idleText", "PRISTON CHESS"); strncpy(idleText, t.c_str(), sizeof(idleText) - 1); idleText[sizeof(idleText) - 1] = '\0'; }
  textMode = prefs.getUChar("txtMode", 0);
  {
    // Migration: copy legacy single "paint" key into slot 0 if present.
    if (prefs.getBytesLength("paint") == sizeof(paintGrids[0]))
      prefs.getBytes("paint", paintGrids[0], sizeof(paintGrids[0]));
    char key[8];
    for (int s = 0; s < PAINT_SLOTS; s++) {
      snprintf(key, sizeof(key), "paint%d", s);
      if (prefs.getBytesLength(key) == sizeof(paintGrids[s]))
        prefs.getBytes(key, paintGrids[s], sizeof(paintGrids[s]));
    }
    paintSel = prefs.getUChar("paintSel", 0);
    if (paintSel >= PAINT_SLOTS) paintSel = 0;
    for (int s = 0; s < PAINT_SLOTS; s++) {
      snprintf(key, sizeof(key), "paintN%d", s);
      String n = prefs.getString(key, "");
      strncpy(paintNames[s], n.c_str(), sizeof(paintNames[s]) - 1);
      paintNames[s][sizeof(paintNames[s]) - 1] = '\0';
    }
  }
  autoDim          = prefs.getBool("autoDim",     false);
  nightOff         = prefs.getBool("nightOff",    false);
  autoDimStart     = prefs.getUChar("dimStart",   22);
  autoDimEnd       = prefs.getUChar("dimEnd",      7);
  autoDimBrightness = prefs.getUChar("dimBri",    60);
  // Per-player chess-animation tuning (path colour + speed multiplier).
  animPathColorW = {prefs.getUChar("apcWR",  40),  prefs.getUChar("apcWG", 220), prefs.getUChar("apcWB", 255)};
  animPathColorB = {prefs.getUChar("apcBR", 255),  prefs.getUChar("apcBG", 170), prefs.getUChar("apcBB",  60)};
  animChessSpeedW = prefs.getFloat("acsW", 1.0f);
  animChessSpeedB = prefs.getFloat("acsB", 1.0f);
  knightPathPct    = prefs.getUChar("kPathPct",    5);
  pickupPulseMs    = prefs.getUShort("puPulseMs", 90);
  walkBudgetMs     = prefs.getUShort("walkBudgMs", 380);
  replayIntervalMs = prefs.getUShort("rpIntMs",   2000);
  replayOverlayPct = prefs.getUChar("rpOvlyPct",  95);
  dimOthersOnPickup = prefs.getBool("dimOthrs",   false);
  dimOthersPct      = prefs.getUChar("dimOthrPct", 50);
  prefs.end();
  Serial.printf("LED settings loaded: brightness=%d, idleAnim=%d, idleColor=#%02x%02x%02x speed=%.1f sat=%d\n",
    brightness, (uint8_t)idleAnimation, idleColor.r, idleColor.g, idleColor.b, animSpeed, idleSaturation);
}

void BoardDriver::saveLedSettings() {
  if (!ChessUtils::ensureNvsInitialized()) {
    Serial.println("NVS init failed - LED settings not saved");
    return;
  }
  Preferences prefs;
  prefs.begin("ledSettings", false);
  prefs.putUChar("brightness", brightness);
  prefs.putUChar("dimMult", dimMultiplier);
  prefs.putUChar("idleAnim", (uint8_t)idleAnimation);
  prefs.putUChar("idleColorR",  idleColor.r);
  prefs.putUChar("idleColorG",  idleColor.g);
  prefs.putUChar("idleColorB",  idleColor.b);
  prefs.putUChar("idleColor2R", idleColor2.r);
  prefs.putUChar("idleColor2G", idleColor2.g);
  prefs.putUChar("idleColor2B", idleColor2.b);
  prefs.putFloat("animSpeed",   animSpeed);
  prefs.putUChar("idleSat",     idleSaturation);
  prefs.putBool("randCol",      idleRandomColor);
  prefs.putUChar("txtR", textColor.r); prefs.putUChar("txtG", textColor.g); prefs.putUChar("txtB", textColor.b);
  prefs.putUChar("txtBgR", textBg.r);  prefs.putUChar("txtBgG", textBg.g);  prefs.putUChar("txtBgB", textBg.b);
  prefs.putString("idleText", idleText);
  prefs.putUChar("txtMode", textMode);
  {
    char key[8];
    for (int s = 0; s < PAINT_SLOTS; s++) {
      snprintf(key, sizeof(key), "paint%d", s);
      prefs.putBytes(key, paintGrids[s], sizeof(paintGrids[s]));
      snprintf(key, sizeof(key), "paintN%d", s);
      prefs.putString(key, paintNames[s]);
    }
    prefs.putUChar("paintSel", paintSel);
  }
  prefs.putBool("autoDim",      autoDim);
  prefs.putBool("nightOff",     nightOff);
  prefs.putUChar("dimStart",    autoDimStart);
  prefs.putUChar("dimEnd",      autoDimEnd);
  prefs.putUChar("dimBri",      autoDimBrightness);
  prefs.putUChar("apcWR", animPathColorW.r); prefs.putUChar("apcWG", animPathColorW.g); prefs.putUChar("apcWB", animPathColorW.b);
  prefs.putUChar("apcBR", animPathColorB.r); prefs.putUChar("apcBG", animPathColorB.g); prefs.putUChar("apcBB", animPathColorB.b);
  prefs.putFloat("acsW", animChessSpeedW);
  prefs.putFloat("acsB", animChessSpeedB);
  prefs.putUChar("kPathPct",   knightPathPct);
  prefs.putUShort("puPulseMs", pickupPulseMs);
  prefs.putUShort("walkBudgMs", walkBudgetMs);
  prefs.putUShort("rpIntMs",   replayIntervalMs);
  prefs.putUChar("rpOvlyPct",  replayOverlayPct);
  prefs.putBool("dimOthrs",    dimOthersOnPickup);
  prefs.putUChar("dimOthrPct", dimOthersPct);
  prefs.end();
  Serial.printf("LED settings saved: brightness=%d, idleAnim=%d, idleColor=#%02x%02x%02x speed=%.1f sat=%d\n",
    brightness, (uint8_t)idleAnimation, idleColor.r, idleColor.g, idleColor.b, animSpeed, idleSaturation);
}

void BoardDriver::triggerCalibration() {
  // Mark calibration as needed by clearing the saved calibration
  if (!ChessUtils::ensureNvsInitialized()) {
    Serial.println("NVS init failed - cannot trigger calibration");
    return;
  }
  Preferences prefs;
  prefs.begin("boardCal", false);
  prefs.clear();
  prefs.end();
  Serial.println("Board calibration cleared - rebooting ...");
  // Delay restart so the HTTP response can be sent first
  xTaskCreate([](void*) {
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP.restart();
    vTaskDelete(nullptr);
  }, "reboot", 1024, nullptr, 1, nullptr);
}

void BoardDriver::loadHardwareConfig() {
  hwConfig = HardwareConfig::defaults();
  if (!ChessUtils::ensureNvsInitialized()) return;

  Preferences prefs;
  prefs.begin("hwConfig", false);
  if (!prefs.isKey("ver")) {
    // First boot (or NVS wiped): persist the compile-time defaults so future
    // boots load them like any saved config — no more "no saved config" log.
    prefs.putUChar("ver", 1);
    prefs.putUChar("ledPin", hwConfig.ledPin);
    prefs.putUChar("srClk", hwConfig.srClkPin);
    prefs.putUChar("srLatch", hwConfig.srLatchPin);
    prefs.putUChar("srData", hwConfig.srDataPin);
    prefs.putBool("srInvert", hwConfig.srInvertOutputs);
    prefs.putBytes("rowPins", hwConfig.rowPins, sizeof(hwConfig.rowPins));
    prefs.end();
    Serial.println("Hardware config initialized from compile-time defaults");
    return;
  }
  hwConfig.ledPin = prefs.getUChar("ledPin", LED_PIN);
  hwConfig.srClkPin = prefs.getUChar("srClk", SR_CLK_PIN);
  hwConfig.srLatchPin = prefs.getUChar("srLatch", SR_LATCH_PIN);
  hwConfig.srDataPin = prefs.getUChar("srData", SR_SER_DATA_PIN);
  hwConfig.srInvertOutputs = prefs.getBool("srInvert", SR_INVERT_OUTPUTS != 0);
  size_t len = prefs.getBytesLength("rowPins");
  if (len == sizeof(hwConfig.rowPins))
    prefs.getBytes("rowPins", hwConfig.rowPins, sizeof(hwConfig.rowPins));
  prefs.end();
  Serial.printf("Hardware config loaded: LED=%d, SR_CLK=%d, SR_LATCH=%d, SR_DATA=%d, SR_INVERT=%d\n", hwConfig.ledPin, hwConfig.srClkPin, hwConfig.srLatchPin, hwConfig.srDataPin, hwConfig.srInvertOutputs);
}

void BoardDriver::saveHardwareConfig(const HardwareConfig& config) {
  if (!ChessUtils::ensureNvsInitialized()) {
    Serial.println("NVS init failed - hardware config not saved");
    return;
  }
  Preferences prefs;
  prefs.begin("hwConfig", false);
  prefs.putUChar("ver", 1);
  prefs.putUChar("ledPin", config.ledPin);
  prefs.putUChar("srClk", config.srClkPin);
  prefs.putUChar("srLatch", config.srLatchPin);
  prefs.putUChar("srData", config.srDataPin);
  prefs.putBool("srInvert", config.srInvertOutputs);
  prefs.putBytes("rowPins", config.rowPins, sizeof(config.rowPins));
  prefs.end();
  // Don't update hwConfig in memory, the new config takes effect after reboot. Modifying it here would race with calibration or gameplay reading the pins.
  Serial.println("Hardware config saved to NVS");
}
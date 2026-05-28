#include "board_driver.h"
#include "chess_utils.h"
#include "led_colors.h"
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>

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

BoardDriver::BoardDriver() : strip(nullptr), lastEnabledCol(-2), brightness(BRIGHTNESS), dimMultiplier(70), idleAnimation(IdleAnimation::RAINBOW), animSpeed(1.0f), idleSaturation(100), autoDim(false), autoDimStart(22), autoDimEnd(7), autoDimBrightness(60), swapAxes(0), hwConfig(HardwareConfig::defaults()), calibrating(false), animRestartPending(false), calibrated(false), debugHeldCol(-1) {
  idleColor  = {255, 255, 255};
  idleColor2 = {255, 128, 0};
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
  xTaskCreatePinnedToCore(animationWorkerTask, "AnimWorker", 4096, nullptr, 2, &animationTaskHandle, 1);
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
    if (!wasSkipped)
      saveCalibration();
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
  unsigned long stableStart = 0;

  while (true) {
    readRawSensors(rawState);
    int pressedCount = 0;
    for (int row = 0; row < NUM_ROWS; row++)
      for (int col = 0; col < NUM_COLS; col++)
        if (rawState[row][col])
          pressedCount++;

    if (pressedCount == 0) {
      if (stableStart == 0)
        stableStart = millis();
      if (millis() - stableStart >= stableMs)
        return true;
    } else {
      stableStart = 0;
      unsigned long now = millis();
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
  if ((row + col) % 2 == 1)
    multiplier = dimMultiplier / 100.0f; // Dim dark squares based on user setting
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

void BoardDriver::doCapture(int centerRow, int centerCol) {
  float centerX = centerCol + 0.5f;
  float centerY = centerRow + 0.5f;

  // Wave animation with multiple expanding rings in 2 colors
  const int numWaves = 3;       // Number of concurrent wave rings
  const int totalFrames = 20;   // Total animation frames
  const float waveSpeed = 0.4f; // How fast waves expand per frame
  const float waveWidth = 1.2f; // Thickness of each wave ring

  clearAllLEDs(false);
  for (int frame = 0; frame < totalFrames; frame++) {
    for (int row = 0; row < NUM_ROWS; row++) {
      for (int col = 0; col < NUM_COLS; col++) {
        float dx = col - centerX;
        float dy = row - centerY;
        float dist = sqrt(dx * dx + dy * dy);

        uint8_t finalR = 0, finalG = 0, finalB = 0;

        // Check each wave ring
        for (int w = 0; w < numWaves; w++) {
          // Stagger wave starts so they trail each other
          float waveRadius = (frame - w * 4) * waveSpeed;
          if (waveRadius < 0) continue;

          // Distance from this pixel to the wave ring
          float distToWave = fabs(dist - waveRadius);

          if (distToWave < waveWidth) {
            // Intensity based on how close to wave center (smooth falloff)
            float intensity = 1.0f - (distToWave / waveWidth);
            intensity = intensity * intensity; // Quadratic falloff for smoother look

            // Fade out as wave expands
            float fadeOut = 1.0f - (waveRadius / 6.0f);
            if (fadeOut < 0) fadeOut = 0;
            intensity *= fadeOut;

            // Alternate colors between waves
            if (w % 2 == 0) {
              finalR = max(finalR, (uint8_t)(LedColors::Red.r * intensity));
              finalG = max(finalG, (uint8_t)(LedColors::Red.g * intensity));
              finalB = max(finalB, (uint8_t)(LedColors::Red.b * intensity));
            } else {
              finalR = max(finalR, (uint8_t)(LedColors::Yellow.r * intensity));
              finalG = max(finalG, (uint8_t)(LedColors::Yellow.g * intensity));
              finalB = max(finalB, (uint8_t)(LedColors::Yellow.b * intensity));
            }
          }
        }
        setSquareLED(row, col, LedRGB{finalR, finalG, finalB});
      }
    }
    setSquareLED(centerRow, centerCol, LedColors::Red);
    showLEDs();
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  clearAllLEDs();
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

void BoardDriver::fillUpAnimation(LedRGB color, int msPerLed) {
  acquireLEDs();
  clearAllLEDs(false);
  for (int row = 0; row < NUM_ROWS; row++) {
    for (int col = 0; col < NUM_COLS; col++) {
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
  float baseHue = rgbToHue(idleColor);
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
  bool isChromatic = rgbSaturation(idleColor) > 0.15f;
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
  float baseHue = rgbSaturation(idleColor) > 0.15f ? rgbToHue(idleColor) : 0.0f;
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
  float hueOff = rgbToHue(idleColor);
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

void BoardDriver::doMeteor(std::atomic<bool>* stopFlag) {
  const int MAX_M = 4;
  struct { float row, col, hue; bool active; } m[MAX_M] = {};
  float fade[NUM_ROWS][NUM_COLS] = {};
  uint32_t seed = 99887;
  auto rng  = [&]() -> uint32_t { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
  float baseHue  = rgbToHue(idleColor);
  bool chromatic = rgbSaturation(idleColor) > 0.15f;
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
  vTaskDelay(pdMS_TO_TICKS(ms < 5 ? 5 : ms));
}

float BoardDriver::getAutoDimFactor() const {
  if (!autoDim) return 1.0f;
  struct tm ti;
  if (!getLocalTime(&ti)) return 1.0f;
  int h = ti.tm_hour;
  bool dim = (autoDimStart <= autoDimEnd)
    ? (h >= autoDimStart && h < autoDimEnd)
    : (h >= autoDimStart || h < autoDimEnd);
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
  autoDim          = prefs.getBool("autoDim",     false);
  autoDimStart     = prefs.getUChar("dimStart",   22);
  autoDimEnd       = prefs.getUChar("dimEnd",      7);
  autoDimBrightness = prefs.getUChar("dimBri",    60);
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
  prefs.putBool("autoDim",      autoDim);
  prefs.putUChar("dimStart",    autoDimStart);
  prefs.putUChar("dimEnd",      autoDimEnd);
  prefs.putUChar("dimBri",      autoDimBrightness);
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
  ESP.restart();
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
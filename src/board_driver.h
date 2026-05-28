#ifndef BOARD_DRIVER_H
#define BOARD_DRIVER_H

#include "led_colors.h"
#include <NeoPixelBusLg.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ---------------------------
// Default Hardware Configuration
// These defaults are used if no pin configuration is saved in NVS.
// Users with pre-built firmware can change pins via the web UI at runtime.
// ---------------------------

// ---------------------------
// WS2812B LED Data IN GPIO Pin
// The strip doesn't need to have a specific layout, calibration will map it correctly
// ---------------------------
#define LED_PIN 32
#define NUM_ROWS 8
#define NUM_COLS 8
#define LED_COUNT (NUM_ROWS * NUM_COLS)
#define BRIGHTNESS 255 // Default LED brightness: 0-255 (0=off, 255=max). (can be changed later from webUI)

// ---------------------------
// Shift Register (74HC595) Pins
// ---------------------------
// Pin 10 (SRCLR') 5V = don't clear the register
// Pin 13 (OE') GND = always enabled
// Pin 11 (SRCLK) GPIO = Shift Register Clock
#define SR_CLK_PIN 14
// Pin 12 (RCLK) GPIO = Latch Clock
#define SR_LATCH_PIN 26
// Pin 14 (SER) GPIO = Serial data input
#define SR_SER_DATA_PIN 33
// Set to 1 if the shift register outputs drive PNP transistors
// Hardware is PNP high-side column drivers (see schematic) → 1.
#define SR_INVERT_OUTPUTS 1

// ---------------------------
// Row and column pins don't need to be in any particular order, calibration will map them correctly
// ---------------------------

// ---------------------------
// Row Input Pins
// NOTE: Defaults moved off of GPIOs 4/16/17/18/23 because those are reserved
// for the Waveshare 2.13" e-Paper (BUSY/RST/DC + VSPI SCK/MOSI). See e-Paper
// pin block below. Pins 34/35 are input-only — fine for A3144 with external
// pull-up. All values can still be overridden at runtime from the web UI.
// ---------------------------
#define ROW_PIN_0 13
#define ROW_PIN_1 25
#define ROW_PIN_2 27
#define ROW_PIN_3 34
#define ROW_PIN_4 4   // GPIO 12 ist Strapping-Pin (HIGH=1.8V Flash) → GPIO 4
#define ROW_PIN_5 21
#define ROW_PIN_6 22
#define ROW_PIN_7 19

#define NUM_ALL_ROW_PINS 8
static constexpr uint8_t ALL_ROW_PINS[NUM_ALL_ROW_PINS] = {
  ROW_PIN_0, ROW_PIN_1, ROW_PIN_2, ROW_PIN_3,
  ROW_PIN_4, ROW_PIN_5, ROW_PIN_6, ROW_PIN_7
};

// ---------------------------
// Runtime hardware configuration (loaded from NVS, editable via web UI)
// Falls back to the #define defaults above if no NVS data exists.
// ---------------------------
struct HardwareConfig {
  uint8_t ledPin;
  uint8_t srClkPin;
  uint8_t srLatchPin;
  uint8_t srDataPin;
  bool srInvertOutputs;
  uint8_t rowPins[NUM_ROWS];

  // Initialize with compile-time defaults
  static HardwareConfig defaults() {
    return {LED_PIN, SR_CLK_PIN, SR_LATCH_PIN, SR_SER_DATA_PIN, SR_INVERT_OUTPUTS != 0,
            {ROW_PIN_0, ROW_PIN_1, ROW_PIN_2, ROW_PIN_3, ROW_PIN_4, ROW_PIN_5, ROW_PIN_6, ROW_PIN_7}};
  }
};

// ---------------------------
// Sensor Polling Delay and Debounce
// ---------------------------
#define SENSOR_READ_DELAY_MS 40
#define DEBOUNCE_MS 125
#define CALIBRATION_WARNING_INTERVAL_MS 4000

// Idle animation modes (persisted in NVS)
enum class IdleAnimation : uint8_t {
  RAINBOW = 0, BREATHING = 1, CHASE = 2, TWINKLE = 3, SOLID = 4, OFF = 5,
  FIRE = 6, PLASMA = 7, METEOR = 8, CHESS_PULSE = 9, COLOR_CYCLE = 10, SCAN = 11
};

// Animation job types for async queue
enum class AnimationType : uint8_t {
  CAPTURE, PROMOTION, BLINK, WAITING, THINKING, FIREWORK, FLASH,
  RAINBOW, BREATHING, CHASE, TWINKLE, SOLID,
  FIRE, PLASMA, METEOR, CHESS_PULSE, COLOR_CYCLE, SCAN
};

// Animation job with parameters union for queue
struct AnimationJob {
  AnimationType type;
  std::atomic<bool>* stopFlag; // For cancellable animations
  union {
    struct {
      int row, col;
    } capture;
    struct {
      int row, col;
    } promotion;
    struct {
      int row, col;
      LedRGB color;
      int times;
      bool clearAfter;
    } blink;
    struct {
      LedRGB color;
      int times;
    } flash;
    struct {
      LedRGB color;
    } firework;
  } params;
};

// ---------------------------
// Board Driver Class
// Logical board coordinates: row 0 = rank 8, column 0 = file a
// ---------------------------
class BoardDriver {
 private:
  NeoPixelBusLg<NeoGrbFeature, NeoEsp32Rmt1Ws2812xMethod, NeoGammaNullMethod>* strip;

  // Animation queue system
  static QueueHandle_t animationQueue;
  static TaskHandle_t animationTaskHandle;
  static SemaphoreHandle_t ledMutex;
  static BoardDriver* instance;
  static void animationWorkerTask(void* param);
  void executeAnimation(const AnimationJob& job);
  void doCapture(int row, int col);
  void doRainbow(std::atomic<bool>* stopFlag);
  void doBreathing(std::atomic<bool>* stopFlag);
  void doChase(std::atomic<bool>* stopFlag);
  void doTwinkle(std::atomic<bool>* stopFlag);
  void doSolid(std::atomic<bool>* stopFlag);
  void doFire(std::atomic<bool>* stopFlag);
  void doPlasma(std::atomic<bool>* stopFlag);
  void doMeteor(std::atomic<bool>* stopFlag);
  void doChessPulse(std::atomic<bool>* stopFlag);
  void doColorCycle(std::atomic<bool>* stopFlag);
  void doScan(std::atomic<bool>* stopFlag);
  void doPromotion(int row, int col);
  void animDelay(uint32_t baseMs);
  float getAutoDimFactor() const;
  void doBlink(int row, int col, LedRGB color, int times, bool clearAfter);
  void doWaiting(std::atomic<bool>* stopFlag);
  void doThinking(std::atomic<bool>* stopFlag);
  void doFirework(LedRGB color);
  void doFlash(LedRGB color, int times);
  bool sensorState[NUM_ROWS][NUM_COLS];
  bool sensorPrev[NUM_ROWS][NUM_COLS];
  bool sensorRaw[NUM_ROWS][NUM_COLS];
  unsigned long sensorDebounceTime[NUM_ROWS][NUM_COLS];
  int lastEnabledCol; // Tracks last enabled column for efficient sequential shifting

  enum Axis {
    RowsAxis = 0,
    ColsAxis = 1,
    UnknownAxis = 2,
  };
  // LED settings (persisted in NVS)
  uint8_t brightness;
  uint8_t dimMultiplier;
  IdleAnimation idleAnimation;
  LedRGB idleColor;
  LedRGB idleColor2;
  float animSpeed;           // 0.25–4.0, 1.0 = normal
  uint8_t idleSaturation;    // 0–100
  bool autoDim;
  uint8_t autoDimStart;      // 0–23
  uint8_t autoDimEnd;        // 0–23
  uint8_t autoDimBrightness; // 10–255
  LedRGB currentColors[NUM_ROWS][NUM_COLS]; // Track current colors for dim multiplier updates

  // Runtime hardware pin configuration (persisted in NVS)
  HardwareConfig hwConfig;

  // Debug: hält eine Spalte dauerhaft aktiv (-1 = normaler Scan)
  std::atomic<int> debugHeldCol;

  // True while interactive calibration is running (guards concurrent web handler access)
  std::atomic<bool> calibrating;
  std::atomic<bool> animRestartPending;
  bool calibrated;

  // Calibration data
  uint8_t swapAxes;
  uint8_t toLogicalRow[NUM_ROWS];
  uint8_t toLogicalCol[NUM_COLS];
  uint8_t ledIndexMap[NUM_ROWS][NUM_COLS];

  // Move-highlight colours (editable via web, persisted in NVS).
  LedRGB hlSource{0, 255, 255};    // Cyan  — picked-up piece's square
  LedRGB hlTarget{255, 255, 255};  // White — legal empty destination
  LedRGB hlCapture{255, 0, 0};     // Red   — capture destination
  LedRGB hlValid{0, 255, 0};       // Green — confirmed move
  LedRGB hlInvalid{255, 0, 0};     // Red   — unreachable square
  void loadHighlightColors();

  bool loadCalibration();
  void saveCalibration();
  bool runCalibration();
  void loadLedSettings();
  void loadHardwareConfig();
  void readRawSensors(bool rawState[NUM_ROWS][NUM_COLS]);
  bool waitForBoardEmpty(unsigned long stableMs = 500);
  bool waitForSingleRawPress(int& rawRow, int& rawCol, unsigned long stableMs = 500);
  void showCalibrationError();
  bool calibrateAxis(Axis axis, uint8_t* axisPinsOrder, size_t NUM_PINS, bool firstAxisSwapped);
  String axisToChessRankFile(Axis axis) const { return (axis == RowsAxis) ? "Rank" : ((axis == ColsAxis) ? "File" : "Unknown"); };

  void loadShiftRegister(byte data, int bits = 8);
  void disableAllCols();
  void enableCol(int col);
  int getPixelIndex(int row, int col);

 public:
  BoardDriver();
  void beginHardware();
  void checkCalibration();
  void readSensors();
  void debugHoldCol(int col);  // Hält eine Spalte dauerhaft aktiv (nur für Debug)
  bool getSensorState(int row, int col);
  bool getSensorRaw(int row, int col) { return (row>=0&&row<NUM_ROWS&&col>=0&&col<NUM_COLS) ? sensorRaw[row][col] : false; }
  bool getSensorPrev(int row, int col);
  void updateSensorPrev();

  // LED Control (use acquireLEDs/releaseLEDs for multi-call sequences)
  void acquireLEDs(); // Block until LED strip available
  void releaseLEDs(); // Release LED strip
  void clearAllLEDs(bool show = true);
  void setSquareLED(int row, int col, LedRGB color);
  void showLEDs();
  // Configurable move-highlight colours (shared by sim + real game).
  LedRGB hlSourceColor()  const { return hlSource; }
  LedRGB hlTargetColor()  const { return hlTarget; }
  LedRGB hlCaptureColor() const { return hlCapture; }
  LedRGB hlValidColor()   const { return hlValid; }
  LedRGB hlInvalidColor() const { return hlInvalid; }
  void setHighlightColors(LedRGB s, LedRGB t, LedRGB c, LedRGB v, LedRGB i);

  // Animation Functions (queued for async execution)
  void fireworkAnimation(LedRGB color = LedColors::White);
  void captureAnimation(int row, int col);
  void promotionAnimation(int row, int col);
  void blinkSquare(int row, int col, LedRGB color, int times = 3, bool clearAfter = true);
  void showConnectingAnimation();
  void flashBoardAnimation(LedRGB color, int times = 3);
  void fillUpAnimation(LedRGB color, int msPerLed = 50);

  // Start a cancellable animation. Returns a non-owning pointer to a stop flag.
  // Ownership: the animation task owns and deletes the flag after the animation loop exits.
  // Caller must ONLY call store(true) to signal stop, never delete the pointer.
  std::atomic<bool>* startThinkingAnimation();
  std::atomic<bool>* startWaitingAnimation();
  std::atomic<bool>* startIdleAnimation();
  IdleAnimation getIdleAnimation() const { return idleAnimation; }
  void setIdleAnimation(IdleAnimation anim) { idleAnimation = anim; animRestartPending.store(true); }
  LedRGB getIdleColor() const { return idleColor; }
  void setIdleColor(LedRGB color);
  LedRGB getIdleColor2() const { return idleColor2; }
  void setIdleColor2(LedRGB color) { idleColor2 = color; }
  float getAnimSpeed() const { return animSpeed; }
  void setAnimSpeed(float s) { animSpeed = s < 0.25f ? 0.25f : (s > 4.0f ? 4.0f : s); }
  uint8_t getIdleSaturation() const { return idleSaturation; }
  void setIdleSaturation(uint8_t s) { idleSaturation = s > 100 ? 100 : s; }
  bool getAutoDim() const { return autoDim; }
  void setAutoDim(bool v) { autoDim = v; }
  uint8_t getAutoDimStart() const { return autoDimStart; }
  void setAutoDimStart(uint8_t h) { autoDimStart = h % 24; }
  uint8_t getAutoDimEnd() const { return autoDimEnd; }
  void setAutoDimEnd(uint8_t h) { autoDimEnd = h % 24; }
  uint8_t getAutoDimBrightness() const { return autoDimBrightness; }
  void setAutoDimBrightness(uint8_t b) { autoDimBrightness = b < 10 ? 10 : b; }
  bool consumeAnimationRestartRequest();

  // Board settings
  uint8_t getBrightness() const { return brightness; }
  uint8_t getDimMultiplier() const { return dimMultiplier; }
  void setBrightness(uint8_t value);
  void setDimMultiplier(uint8_t value);
  void saveLedSettings();
  void triggerCalibration();
  bool isCalibrated() const { return calibrated; }
  LedRGB getLEDColor(int row, int col) const {
    if (row < 0 || row >= NUM_ROWS || col < 0 || col >= NUM_COLS) return {0,0,0};
    return currentColors[row][col];
  }

  // Hardware pin configuration (runtime, persisted in NVS)
  const HardwareConfig& getHardwareConfig() const { return hwConfig; }
  void saveHardwareConfig(const HardwareConfig& config);
};

#endif // BOARD_DRIVER_H

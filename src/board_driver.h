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
// NOTE: Defaults stay off GPIOs 4/16/17/18/23 — those were reserved for the
// e-Paper add-on in the original design (now removed) and are kept free to
// avoid churn. Pins 34/35 are input-only — fine for A3144 with external
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
  FIRE = 6, PLASMA = 7, METEOR = 8, CHESS_PULSE = 9, COLOR_CYCLE = 10, SCAN = 11,
  FIREWORKS = 12, RIPPLE = 13, MATRIX = 14, AURORA = 15, SPIRAL = 16, STANDOFF = 17,
  KNIGHT_TOUR = 18, REPLAY = 19, ATTACK = 20, TOUCH = 21, LIFE = 22, SAND = 23,
  LAVA = 24, RAIN = 25, STARFIELD = 26, WALLCLOCK = 27, TEXT = 28, PAINT = 29, TETRIS = 30,
  AUDIO = 31, SNAKE = 32, PONG = 33, LANGTON = 34,
  BOUNCE = 35, SNOW = 36, BUBBLES = 37, SORT = 38, WAVE = 39, RULE30 = 40,
  BREAKOUT = 41, TRON = 42, CATCH = 43, SIMON = 44,
  CHARGE = 45, SHIELDWALL = 46, COURT = 47
};

// Animation job types for async queue
enum class AnimationType : uint8_t {
  CAPTURE, PROMOTION, BLINK, WAITING, THINKING, FIREWORK, FLASH,
  RAINBOW, BREATHING, CHASE, TWINKLE, SOLID,
  FIRE, PLASMA, METEOR, CHESS_PULSE, COLOR_CYCLE, SCAN, FIREWORKS,
  RIPPLE, MATRIX, AURORA, SPIRAL, STANDOFF,
  KNIGHT_TOUR, REPLAY, ATTACK, TOUCH, LIFE, SAND, LAVA, RAIN, STARFIELD, WALLCLOCK,
  TEXT, PAINT, TETRIS, AUDIO, SNAKE, PONG, LANGTON,
  BOUNCE, SNOW, BUBBLES, SORT, WAVE, RULE30,
  BREAKOUT, TRON, CATCH, SIMON,
  CHECK_BORDER,
  CHARGE, SHIELDWALL, COURT,
  MOVE_PATH,
  MULTI_PATH_HL,
  CHECKMATE_SPIRAL,
  CHECKMATE_ANIM,
  MODE_CHANGE_TV,
  CELL_PULSE,
  HEARTBEAT
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
    struct {
      int fromRow, fromCol, toRow, toCol;
      char player;
      LedRGB color;       // explicit trail colour (used when useColor is true)
      bool useColor;      // true → use `color`; false → effectivePathColor(player)
    } path;
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
  void doFireworks(std::atomic<bool>* stopFlag);
  void doRipple(std::atomic<bool>* stopFlag);
  void doMatrixRain(std::atomic<bool>* stopFlag);
  void doAurora(std::atomic<bool>* stopFlag);
  void doSpiral(std::atomic<bool>* stopFlag);
  void doStandoff(std::atomic<bool>* stopFlag);
  void doKnightTour(std::atomic<bool>* stopFlag);
  void doReplay(std::atomic<bool>* stopFlag);
  void doAttack(std::atomic<bool>* stopFlag);
  void doTouch(std::atomic<bool>* stopFlag);
  void doLife(std::atomic<bool>* stopFlag);
  void doSand(std::atomic<bool>* stopFlag);
  void doLava(std::atomic<bool>* stopFlag);
  void doRain(std::atomic<bool>* stopFlag);
  void doStarfield(std::atomic<bool>* stopFlag);
  void doWallclock(std::atomic<bool>* stopFlag);
  void doText(std::atomic<bool>* stopFlag);
  void doPaint(std::atomic<bool>* stopFlag);
  void doTetris(std::atomic<bool>* stopFlag);
  void doAudio(std::atomic<bool>* stopFlag);
  void doSnake(std::atomic<bool>* stopFlag);
  void doPong(std::atomic<bool>* stopFlag);
  void doLangton(std::atomic<bool>* stopFlag);
  void doBounce(std::atomic<bool>* stopFlag);
  void doSnow(std::atomic<bool>* stopFlag);
  void doBubbles(std::atomic<bool>* stopFlag);
  void doSort(std::atomic<bool>* stopFlag);
  void doWave(std::atomic<bool>* stopFlag);
  void doRule30(std::atomic<bool>* stopFlag);
  void doBreakout(std::atomic<bool>* stopFlag);
  void doTron(std::atomic<bool>* stopFlag);
  void doCatch(std::atomic<bool>* stopFlag);
  void doSimon(std::atomic<bool>* stopFlag);
  void doCharge(std::atomic<bool>* stopFlag);
  void doShieldwall(std::atomic<bool>* stopFlag);
  void doCourt(std::atomic<bool>* stopFlag);
  void doPromotion(int row, int col);
  void animDelay(uint32_t baseMs);
  void gameDelay(uint32_t ms);   // like animDelay but NOT scaled by animSpeed (for idle games)
  float getAutoDimFactor() const;
  float idleHue(float fallback); // random hue if idleRandomColor, else primary hue / fallback
  void doBlink(int row, int col, LedRGB color, int times, bool clearAfter);
  void doWaiting(std::atomic<bool>* stopFlag);
  void doThinking(std::atomic<bool>* stopFlag);
  void doFirework(LedRGB color);
  void doFlash(LedRGB color, int times);
  void doCheckBorderFlash();
  void doCheckmateSpiral();
  void doCheckmateAnimation();
  void doCellPulse(int row, int col, LedRGB color);
  void doHeartbeat(int row, int col, LedRGB peak);
  // Stored params for the queued doCheckmateAnimation() job.
  char cmBoardState[8][8];
  char cmWinner;
  LedRGB cmWinnerColor;
  LedRGB cmLoserColor;
  char cmWinnerName[32];
  void doModeChangeTransition();
  void doMovePath(int fromRow, int fromCol, int toRow, int toCol);
  void doMultiPathHighlight();

  // Storage for the multi-path pickup highlight — set by multiPathHighlight(),
  // read by doMultiPathHighlight() when the job is dequeued. 28 covers a queen
  // on an open board (27 squares + buffer).
  static constexpr int MPH_MAX = 28;
  uint8_t mphSrcR, mphSrcC, mphCount;
  uint8_t mphR[MPH_MAX], mphC[MPH_MAX], mphCapture[MPH_MAX];
  std::atomic<bool> mphAbort{false};       // set true to stop the replay loop early
  std::atomic<bool> endgameAbort{false};   // set true to cut the checkmate cinematic short
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
  LedRGB textColor;          // scrolling-text foreground
  LedRGB textBg;             // scrolling-text background
  uint8_t textMode;          // 0 = scroll marquee, 1 = flash each letter
  char idleText[32];         // scrolling-text content
  static constexpr int PAINT_SLOTS = 8;
  LedRGB paintGrids[PAINT_SLOTS][NUM_ROWS][NUM_COLS]; // 8 user-painted slots
  uint8_t paintSel;                                   // active slot (0..PAINT_SLOTS-1)
  char paintNames[PAINT_SLOTS][12];                   // short slot labels
  uint8_t audioBands[NUM_COLS]; // latest spectrum bands pushed by the browser mic
  uint32_t audioLastMs;         // millis() of last audio push (for decay)
  int gameDir;                  // last game input: 0 up, 1 right, 2 down, 3 left
  uint32_t gameInputMs;         // millis() of last game input (player vs autopilot)
  uint32_t gameSeq;             // increments per input, for edge detection
  float animSpeed;           // 0.25–4.0, 1.0 = normal
  uint8_t idleSaturation;    // 0–100
  bool idleRandomColor;      // pick a fresh random colour each animation pass
  bool autoDim;
  bool nightOff;             // hard-off at night (overrides dim)
  uint8_t autoDimStart;      // 0–23
  uint8_t autoDimEnd;        // 0–23
  uint8_t autoDimBrightness; // 10–255
  LedRGB currentColors[NUM_ROWS][NUM_COLS]; // Track current colors for dim multiplier updates

  // Runtime hardware pin configuration (persisted in NVS)
  HardwareConfig hwConfig;

  // Debug: hält eine Spalte dauerhaft aktiv (-1 = normaler Scan)
  std::atomic<int> debugHeldCol;
public:
  struct PhantomEntry { uint32_t ms; uint8_t row; uint8_t col; bool low; };
private:
  // Phantom-press ring buffer (debug-only).
  static constexpr int PHANTOM_CAP = 64;
  PhantomEntry phantomBuf[PHANTOM_CAP];
  volatile uint16_t phantomHead{0}, phantomTail{0};
  std::atomic<bool> phantomActive{false};
  uint32_t phantomDeadlineMs{0};
  bool phantomLast[NUM_ROWS][NUM_COLS]{};
public:
  void phantomTick();    // called every loop; samples + appends changes
private:

  // True while interactive calibration is running (guards concurrent web handler access)
  std::atomic<bool> calibrating;
  std::atomic<bool> animRestartPending;
  bool calibrated;

  // Calibration data
  uint8_t swapAxes;
  uint8_t toLogicalRow[NUM_ROWS];
  uint8_t toLogicalCol[NUM_COLS];
  uint8_t ledIndexMap[NUM_ROWS][NUM_COLS];
  // LED-map recovery diagnostic: -2 inactive, -1 all-off, 0..LED_COUNT-1 light
  // that RAW strip pixel (bypassing the map). Rendered from the main loop so it
  // doesn't race the idle-animation worker on the other core.
  volatile int diagRawPixel = -2;

  // Move-highlight colours (editable via web, persisted in NVS).
  LedRGB hlSource{0, 255, 255};    // Cyan  — picked-up piece's square
  LedRGB hlTarget{255, 255, 255};  // White — legal empty destination
  LedRGB hlCapture{255, 0, 0};     // Red   — capture destination
  LedRGB hlValid{0, 255, 0};       // Green — confirmed move
  LedRGB hlInvalid{255, 0, 0};     // Red   — unreachable square
  void loadHighlightColors();

  // Per-player chess animation tuning (move-path trail colour + frame-rate
  // multiplier). Configured from /board-settings, persisted in NVS.
  LedRGB animPathColorW{40, 220, 255};   // White-side trail colour (cyan default)
  LedRGB animPathColorB{255, 170, 60};   // Black-side trail colour (amber default)
  float  animChessSpeedW{1.0f};           // 0.25–3.0 multiplier; >1 is faster
  float  animChessSpeedB{1.0f};
  char   mphPlayer{'w'};                  // player whose pickup is being animated
  LedRGB mphColor{0, 0, 0};               // explicit walk-trail colour (player profile colour)
  bool   mphUseColor{false};              // true → use mphColor instead of effectivePathColor

  // Exposed tuning values previously hard-coded in animations. All slider-
  // sized integers so they survive a NVS round-trip cleanly.
  uint8_t knightPathPct{5};         // 0–30 — knight L-intermediate brightness
  uint16_t pickupPulseMs{90};       // 0–300 — source pulse before the walk
  uint16_t walkBudgetMs{380};       // 120–1200 — total walk duration before speed
  uint16_t replayIntervalMs{2000};  // 500–5000 — gap between replay walks
  uint8_t replayOverlayPct{95};     // 0–100 — replay pulse white-blend strength
  bool dimOthersOnPickup{false};    // when set, non-path cells dim during pickup
  uint8_t dimOthersPct{50};         // 5–100 — dimmed brightness floor in %
  uint16_t revealRingMs{220};       // 30–2000 — pickup reveal: time per distance ring
  uint16_t revealPauseMs{700};      // 0–5000 — pickup reveal: pause before the loop repeats
  bool     revealSequential{false}; // "noob" mode: reveal each possible move one after another
  uint16_t noobBudgetMs{100};       // 20–2000 — "noob" mode: STAGGER between option starts
                                    // (option i+1's point launches this many ms after option i)
  uint16_t afterWalkMs{180};        // 40–800 — after-walk placement fade: per-square fade duration (lower = faster)
  uint8_t  rayGhostVal{15};         // 0–255 — pickup reveal: raw brightness of squares BEHIND a blocker on a slider's ray (0 = off)
  uint16_t shinySweepMs{1430};      // 300–5000 — turn-indicator "shiny" glint: DURATION of one diagonal sweep
  uint16_t shinyPauseMs{0};         // 0–8000 — pause (timeout) between successive shiny sweeps
  LedRGB   shinyColor{255, 255, 255}; // turn-indicator glint highlight colour (player glow blends toward this)
  bool simColorsActive{false};      // simulation mode overrides per-player colours
  LedRGB simColorW{40, 110, 255};   // sim white = blue
  LedRGB simColorB{60, 220, 90};    // sim black = green

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

  // === Hardware diagnostic helpers (used by /debug/* endpoints) ===
  // Read all 8 row pins after disabling the shift register. Returns a
  // bitmask: bit r = current digitalRead value (0 = LOW, 1 = HIGH). A
  // properly-wired board with pull-ups should return 0xFF.
  uint8_t pullupSanityCheck();
  // Toggle / read the PNP-invert flag and persist immediately. Returns the
  // new value.
  bool toggleSrInvert();
  bool getSrInvert() const { return hwConfig.srInvertOutputs; }
  // Drive a raw 8-bit pattern to the shift register and leave it latched.
  // Pass 0 to release. Used for the SR-bit-pattern multimeter test.
  void debugSrPattern(uint8_t pattern, bool hold);
  // Classify each (row, col) pin's behaviour over `durationMs` ms. The grid
  // returned encodes:
  //   0 = always HIGH (idle / unused)
  //   1 = always LOW  (stuck / bad pull-up / always-triggered)
  //   2 = toggled cleanly (working)
  //   3 = flickered   (intermittent — wackler / EMI)
  // Result is written into `out[8][8]`; blocks until finished.
  void classifyPins(uint32_t durationMs, uint8_t out[8][8]);
  // Background phantom-press logger — call start() to arm, poll fetch() to
  // pull entries off the ring buffer.
  void phantomLogStart();
  size_t phantomLogFetch(PhantomEntry* out, size_t maxN);
  // One-shot LED-walking and LED-RGB-sequence tests. They paint the strip
  // synchronously on the calling thread (held mutex), no animation queue.
  void ledWalkTest();
  void ledRgbSequenceTest();
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
  // Reset the LED row/col -> pixel-index map to the default straight wiring and
  // persist it. Use when a sensor-driven calibration produced a wrong (offset)
  // LED map — e.g. flaky Hall sensors mis-assigned squares to neighbouring LEDs.
  void resetLedMapToDefault();
  // --- LED-map recovery tooling (no sensors needed) ---
  void setDiagRawPixel(int i) { diagRawPixel = i; }   // -2 inactive, -1 off, 0..63 raw pixel
  int  getDiagRawPixel() const { return diagRawPixel; }
  void renderDiagPixel();                              // light only the raw diag pixel (bypasses map)
  bool setLedMap(const uint8_t* flat, int n);         // validate permutation of 0..LED_COUNT-1, set + persist
  // Brightness headroom test: show all-white at a given luminance (bypasses the
  // MAX_SAFE_BRIGHTNESS cap) so you can find how bright you can go without a
  // brown-out on this strip. -1 = off. Rendered from the main loop (race-free).
  volatile int brightnessTestLum = -1;
  void setBrightnessTest(int lum) { brightnessTestLum = lum; }
  int  getBrightnessTest() const { return brightnessTestLum; }
  void renderBrightnessTest();                         // all-white at brightnessTestLum
  void applyBrightness() { if (strip) strip->SetLuminance(brightness); }  // restore capped luminance
  // Live settings preview: play the pickup-reveal effect on a sample layout with
  // the CURRENT settings (reveal timing, dim, colours) so the user sees changes
  // while adjusting sliders — no piece move needed. Triggered from the web.
  volatile bool previewReq = false;
  void requestRevealPreview() { previewReq = true; }
  bool consumeRevealPreview() { if (previewReq) { previewReq = false; return true; } return false; }
  void playRevealPreview();
  // Live preview of the turn-indicator "shiny" effect on a sample layout. Unlike
  // the one-shot reveal preview, this is a TOGGLE: it runs until switched off, so
  // the user can compare settings while it loops.
  volatile bool shinyPreviewOn = false;
  void setShinyPreview(bool on) { shinyPreviewOn = on; }
  bool isShinyPreviewOn() const { return shinyPreviewOn; }
  void playShinyPreview();
  // Reveal/noob preview as a TOGGLE: loops the pickup-reveal effect until off.
  volatile bool revealLoopOn = false;
  void setRevealLoop(bool on) { revealLoopOn = on; }
  bool isRevealLoopOn() const { return revealLoopOn; }
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
  void checkBorderFlash();   // Red border blink when a king is put into check
  void checkmateSpiral();    // [legacy] outside-in red spiral
  void checkmateAnimation(const char board[8][8], char winner, LedRGB winnerColor, LedRGB loserColor, const char* winnerName = nullptr);
  void modeChangeTransition();  // TV-style off/on between animations
  void movePathAnimation(int fromRow, int fromCol, int toRow, int toCol, char player = 'w',
                         LedRGB color = LedRGB{0, 0, 0}, bool useColor = false);
  void cellPulse(int row, int col, LedRGB color);   // soft pulse on a just-placed square
  void heartbeatSquare(int row, int col, LedRGB peak); // quick lub-dub heartbeat on a just-placed square
  void multiPathHighlight(int srcR, int srcC,
                          const uint8_t* tgtR, const uint8_t* tgtC,
                          const uint8_t* tgtCapture, int nTargets,
                          char player = 'w');
  // Signal any in-flight pickup highlight (initial walk OR replay loop) to
  // exit at its next safe point. Use this from outside the animation system
  // when a move is committed — without it renderBoardLEDs blocks for seconds.
  void abortPickupHighlight() { mphAbort.store(true); }
  // Signal the in-flight checkmate / end-game animation to bail out at its
  // next safe point. Used when a new game starts so the user isn't stuck
  // waiting for the 15 s win cinematic to finish.
  void abortEndgameAnimation() { endgameAbort.store(true); }

  LedRGB animPathColorWhite() const { return animPathColorW; }
  LedRGB animPathColorBlack() const { return animPathColorB; }
  float  animChessSpeedWhite() const { return animChessSpeedW; }
  float  animChessSpeedBlack() const { return animChessSpeedB; }
  void setAnimPathColors(LedRGB w, LedRGB b) { animPathColorW = w; animPathColorB = b; }
  void setAnimChessSpeeds(float w, float b) {
    if (w < 0.25f) w = 0.25f; if (w > 3.0f) w = 3.0f;
    if (b < 0.25f) b = 0.25f; if (b > 3.0f) b = 3.0f;
    animChessSpeedW = w; animChessSpeedB = b;
  }

  uint8_t  getKnightPathPct()    const { return knightPathPct; }
  uint16_t getPickupPulseMs()    const { return pickupPulseMs; }
  uint16_t getWalkBudgetMs()     const { return walkBudgetMs; }
  uint16_t getReplayIntervalMs() const { return replayIntervalMs; }
  uint8_t  getReplayOverlayPct() const { return replayOverlayPct; }
  void setKnightPathPct(uint8_t v)        { if (v > 30) v = 30;    knightPathPct = v; }
  void setPickupPulseMs(uint16_t v)       { if (v > 300) v = 300;  pickupPulseMs = v; }
  void setWalkBudgetMs(uint16_t v)        { if (v < 120) v = 120;  if (v > 1200) v = 1200; walkBudgetMs = v; }
  void setReplayIntervalMs(uint16_t v)    { if (v < 500) v = 500;  if (v > 5000) v = 5000; replayIntervalMs = v; }
  void setReplayOverlayPct(uint8_t v)     { if (v > 100) v = 100;  replayOverlayPct = v; }
  bool getDimOthersOnPickup() const       { return dimOthersOnPickup; }
  void setDimOthersOnPickup(bool v)       { dimOthersOnPickup = v; }
  uint8_t getDimOthersPct() const         { return dimOthersPct; }
  void setDimOthersPct(uint8_t v)         { if (v < 5) v = 5; if (v > 100) v = 100; dimOthersPct = v; }
  uint16_t getRevealRingMs() const        { return revealRingMs; }
  void setRevealRingMs(uint16_t v)        { if (v < 30) v = 30; if (v > 2000) v = 2000; revealRingMs = v; }
  uint16_t getRevealPauseMs() const       { return revealPauseMs; }
  void setRevealPauseMs(uint16_t v)       { if (v > 5000) v = 5000; revealPauseMs = v; }
  bool getRevealSequential() const        { return revealSequential; }
  void setRevealSequential(bool v)        { revealSequential = v; }
  uint16_t getNoobBudgetMs() const        { return noobBudgetMs; }
  void setNoobBudgetMs(uint16_t v)        { if (v < 20) v = 20; if (v > 2000) v = 2000; noobBudgetMs = v; }
  uint16_t getAfterWalkMs() const         { return afterWalkMs; }
  void setAfterWalkMs(uint16_t v)         { if (v < 40) v = 40; if (v > 800) v = 800; afterWalkMs = v; }
  uint8_t getRayGhostVal() const          { return rayGhostVal; }
  void setRayGhostVal(uint8_t v)          { rayGhostVal = v; }   // 0–255 raw brightness
  uint16_t getShinySweepMs() const        { return shinySweepMs; }
  void setShinySweepMs(uint16_t v)        { if (v < 100) v = 100; if (v > 5000) v = 5000; shinySweepMs = v; }
  uint16_t getShinyPauseMs() const        { return shinyPauseMs; }
  void setShinyPauseMs(uint16_t v)        { if (v > 8000) v = 8000; shinyPauseMs = v; }
  // True while the shiny glint is mid-sweep (vs. paused) — the resting render
  // bursts at a high frame rate during this window so a fast sweep stays smooth.
  bool isShinySweeping() const {
    uint32_t cyc = (uint32_t)shinySweepMs + (uint32_t)shinyPauseMs;
    if (cyc == 0) return false;
    return ((uint32_t)millis() % cyc) < shinySweepMs;
  }
  LedRGB getShinyColor() const            { return shinyColor; }
  void setShinyColor(LedRGB c)            { shinyColor = c; }
  void setSimColorsActive(bool v)         { simColorsActive = v; }
  bool getSimColorsActive() const         { return simColorsActive; }
  LedRGB simPathColorWhite() const        { return simColorW; }
  LedRGB simPathColorBlack() const        { return simColorB; }
  // Effective path colour for a given side — switches between sim's fixed
  // blue/green and the user-configured chess-animation settings depending on
  // simColorsActive. Used by both the multi-path pickup highlight and the
  // post-move walk trail so the SIM stays consistently blue/green.
  LedRGB effectivePathColor(char player) const {
    if (simColorsActive) return (player == 'b') ? simColorB : simColorW;
    return (player == 'b') ? animPathColorB : animPathColorW;
  }
  void waitForAnimationQueue(uint32_t maxWaitMs = 5000);   // block until queue drains
  void blinkSquare(int row, int col, LedRGB color, int times = 3, bool clearAfter = true);
  void showConnectingAnimation();
  // Boot/status feedback on the matrix: scroll a short message once (blocking,
  // caller's thread) and a non-destructive green pulse when a device joins the AP.
  void showBootMessage(const char* msg, LedRGB color, uint16_t stepMs = 110, uint8_t repeats = 3);
  void pulseUserConnected();
  void flashBoardAnimation(LedRGB color, int times = 3);
  void fillUpAnimation(LedRGB color, int msPerLed = 50);
  void startupAnimation();   // graceful boot sequence — replaces fillUp at setup()

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
  bool getIdleRandomColor() const { return idleRandomColor; }
  void setIdleRandomColor(bool v) { idleRandomColor = v; animRestartPending.store(true); }
  const char* getIdleText() const { return idleText; }
  void setIdleText(const char* t) { strncpy(idleText, t, sizeof(idleText) - 1); idleText[sizeof(idleText) - 1] = '\0'; animRestartPending.store(true); }
  LedRGB getTextColor() const { return textColor; }
  void setTextColor(LedRGB c) { textColor = c; }
  LedRGB getTextBg() const { return textBg; }
  void setTextBg(LedRGB c) { textBg = c; }
  uint8_t getTextMode() const { return textMode; }
  void setTextMode(uint8_t m) { textMode = m > 1 ? 1 : m; animRestartPending.store(true); }
  LedRGB getPaintPixel(int r, int c) const { return (r >= 0 && r < NUM_ROWS && c >= 0 && c < NUM_COLS) ? paintGrids[paintSel][r][c] : LedRGB{0, 0, 0}; }
  void setPaintPixel(int r, int c, LedRGB col) { if (r >= 0 && r < NUM_ROWS && c >= 0 && c < NUM_COLS) paintGrids[paintSel][r][c] = col; }
  LedRGB getPaintPixel(int slot, int r, int c) const { return (slot >= 0 && slot < PAINT_SLOTS && r >= 0 && r < NUM_ROWS && c >= 0 && c < NUM_COLS) ? paintGrids[slot][r][c] : LedRGB{0, 0, 0}; }
  void setPaintPixel(int slot, int r, int c, LedRGB col) { if (slot >= 0 && slot < PAINT_SLOTS && r >= 0 && r < NUM_ROWS && c >= 0 && c < NUM_COLS) paintGrids[slot][r][c] = col; }
  void savePaintGrid();                              // saves the active slot
  void savePaintSlot(int slot);                      // saves a specific slot
  uint8_t getActivePaintSlot() const { return paintSel; }
  void setActivePaintSlot(uint8_t s);                // sets active + triggers anim restart
  const char* getPaintName(int slot) const { return (slot >= 0 && slot < PAINT_SLOTS) ? paintNames[slot] : ""; }
  void setPaintName(int slot, const char* n);
  void savePaintMeta();                              // persists name table + paintSel
  static constexpr int paintSlotCount() { return PAINT_SLOTS; }
  void setAudioBands(const uint8_t* vals, int n);
  void setGameInput(int dir);
  bool getAutoDim() const { return autoDim; }
  void setAutoDim(bool v) { autoDim = v; }
  bool getNightOff() const { return nightOff; }
  void setNightOff(bool v) { nightOff = v; }
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

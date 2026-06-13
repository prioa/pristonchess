#include "sensor_test.h"
#include "chess_utils.h"
#include <Arduino.h>
#include "serial_tee.h"  // must be last: redefines Serial -> tee

SensorTest::SensorTest(BoardDriver* bd) : boardDriver(bd), complete(false) {
  memset(visited, false, sizeof(visited));
}

void SensorTest::begin() {
  Serial.println("Sensor Test: Visit all squares with a piece to complete the test!");
  complete = false;
  memset(visited, false, sizeof(visited));
  boardDriver->clearAllLEDs();
}

void SensorTest::update() {
  if (complete) return;

  boardDriver->readSensors();
  boardDriver->clearAllLEDs(false);

  int visitedCount = 0;
  for (int row = 0; row < NUM_ROWS; row++) {
    for (int col = 0; col < NUM_COLS; col++) {
      if (boardDriver->getSensorState(row, col))
        visited[row][col] = true;
      if (visited[row][col]) {
        boardDriver->setSquareLED(row, col, LedColors::White);
        visitedCount++;
      }
    }
  }
  boardDriver->showLEDs();
  if (visitedCount == NUM_ROWS * NUM_COLS) {
    complete = true;
    Serial.println("Sensor Test complete! All squares verified");
    boardDriver->fireworkAnimation();
  }
}
// A3144 Einzelsensor-Test — standalone, unabhaengig von der PristonChess-Firmware
//
// Verdrahtung (A3144, bedruckte Seite zu dir, Beine unten):
//   links  VCC  -> 5V        (A3144 braucht >=4,5V, NICHT 3,3V)
//   Mitte  GND  -> GND       (gemeinsame Masse mit ESP32)
//   rechts OUT  -> GPIO PIN  (interner Pull-up an 3,3V wird aktiviert)
//
// OUT ist Open-Collector -> zieht nur gegen GND, treibt nie hoch.
// Mit Pull-up an 3,3V sieht der GPIO max. 3,3V -> ESP32-sicher.
//
// Monitor: 115200 Baud

const int PIN = 13;   // OUT des A3144 hier anschliessen (GPIO13 hat internen Pull-up)

void setup() {
  Serial.begin(115200);
  pinMode(PIN, INPUT_PULLUP);
  Serial.println();
  Serial.println("A3144-Test laeuft. Magnet annaehern (beide Pole testen)...");
}

void loop() {
  static int last = -1;
  int v = digitalRead(PIN);   // LOW = Magnet erkannt, HIGH = kein Magnet
  if (v != last) {
    Serial.println(v == LOW ? ">>> MAGNET erkannt (LOW)" : "    kein Magnet  (HIGH)");
    last = v;
  }
  delay(20);
}

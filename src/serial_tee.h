#ifndef SERIAL_TEE_H
#define SERIAL_TEE_H

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Serial "tee": mirrors every Serial.print*/println/printf call to BOTH the
// real UART0 (USB serial monitor) AND an in-RAM ring buffer that the web UI
// polls via GET /debug/log. The ESP32 Arduino core already defines `Serial`
// as a macro for the real `Serial0` object, so we simply redefine that macro
// to point at our tee — every existing Serial.* call across the firmware is
// captured with zero call-site rewrites. Include this header LAST in each .cpp
// (after Arduino.h and any library headers) so the macro only affects our own
// code, never library internals.
// ---------------------------------------------------------------------------
class SerialTee : public Stream {
 public:
  void begin(unsigned long baud) { Serial0.begin(baud); }

  // Print/Stream interface. Write side tees to UART + ring buffer; read side
  // (available/read/peek) delegates straight to the real UART0 so the boot
  // calibration's Serial.readStringUntil()/available() keep working.
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buf, size_t size) override;
  int available() override { return Serial0.available(); }
  int read() override { return Serial0.read(); }
  int peek() override { return Serial0.peek(); }
  void flush() override { Serial0.flush(); }
  operator bool() const { return true; }
};

extern SerialTee SerialTeeInstance;

// Copy log bytes with sequence >= afterSeq into `out` (NUL-terminated, at most
// maxLen-1 bytes; if more is available only the newest maxLen-1 bytes are
// returned). Writes the number of bytes copied to *outLen (may be null) and
// returns the sequence number the caller should pass as `after` next time.
// Thread-safe. If the client has fallen behind the ring window, it silently
// jumps to the oldest still-buffered byte.
uint32_t serialLogFetch(uint32_t afterSeq, char* out, size_t maxLen, size_t* outLen);

// Current head sequence number (total bytes ever written, monotonic).
uint32_t serialLogSeq();

#undef Serial
#define Serial SerialTeeInstance

#endif  // SERIAL_TEE_H

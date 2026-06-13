#include "serial_tee.h"

// The single tee instance that `Serial` resolves to firmware-wide.
SerialTee SerialTeeInstance;

// ---------------------------------------------------------------------------
// Ring buffer. `s_seq` is a monotonic count of all bytes ever written; the
// byte at logical position p lives at s_buf[p % LOG_CAP] and is valid for
// p in [s_seq - LOG_CAP, s_seq). A portMUX spinlock guards both the write
// path (called from the main loop, the animation task and the AsyncTCP task)
// and the fetch path (AsyncTCP task) — the held sections only copy a few
// hundred bytes, so interrupts are off for tens of microseconds at most.
// ---------------------------------------------------------------------------
static const size_t LOG_CAP = 8192;
static char s_buf[LOG_CAP];
static volatile uint32_t s_seq = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void logPush(const uint8_t* data, size_t n) {
  portENTER_CRITICAL(&s_mux);
  for (size_t i = 0; i < n; i++) {
    s_buf[s_seq % LOG_CAP] = (char)data[i];
    s_seq++;
  }
  portEXIT_CRITICAL(&s_mux);
}

size_t SerialTee::write(uint8_t c) {
  size_t r = Serial0.write(c);
  logPush(&c, 1);
  return r;
}

size_t SerialTee::write(const uint8_t* buf, size_t size) {
  size_t r = Serial0.write(buf, size);
  logPush(buf, size);
  return r;
}

uint32_t serialLogSeq() {
  portENTER_CRITICAL(&s_mux);
  uint32_t h = s_seq;
  portEXIT_CRITICAL(&s_mux);
  return h;
}

uint32_t serialLogFetch(uint32_t afterSeq, char* out, size_t maxLen, size_t* outLen) {
  if (!out || maxLen == 0) { if (outLen) *outLen = 0; return afterSeq; }
  portENTER_CRITICAL(&s_mux);
  uint32_t head = s_seq;
  uint32_t oldest = (head > LOG_CAP) ? head - (uint32_t)LOG_CAP : 0;
  uint32_t start = afterSeq;
  if (start < oldest) start = oldest;   // client fell behind → jump to oldest buffered
  if (start > head) start = head;       // client ahead (e.g. after reboot) → nothing new
  size_t n = (size_t)(head - start);
  if (n > maxLen - 1) {                 // cap to output size, keep the newest bytes
    start = head - (uint32_t)(maxLen - 1);
    n = maxLen - 1;
  }
  for (size_t i = 0; i < n; i++)
    out[i] = s_buf[(start + i) % LOG_CAP];
  portEXIT_CRITICAL(&s_mux);
  out[n] = '\0';
  if (outLen) *outLen = n;
  return start + (uint32_t)n;
}

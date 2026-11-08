/*
 * N64 In-Game Reset (IGR) - passive controller sniffer
 * Target: ATmega328P @ 16 MHz (Arduino Nano V3 / ELEGOO Nano clone), 5 V.
 *
 * The sketch never drives the controller data line. It only listens to
 * controller port 1, decodes the console's 0x01 "read buttons" poll and the
 * controller's 32-bit reply, and pulses the console reset line when the button
 * combo is held.
 *
 * Wiring (see README.md):
 *   D2  <- 1 kOhm <- controller port 1 DATA   (input only, NO pull-up)
 *   D3  -> 220 Ohm -> reset-switch signal (open-drain: LOW or high-Z, never HIGH)
 *   5V  <- N64 5 V      GND <- N64 GND
 *   D13 = on-board LED: blinks ~1 Hz while polls are being decoded
 *
 * N64 bit encoding (open drain, idle high at 3.3 V, ~4 us per bit):
 *   '0' = ~3 us low, ~1 us high      '1' = ~1 us low, ~3 us high
 * Sampling ~2 us after each falling edge gives the bit value.
 *
 * Timing is cycle-counted, so interrupts are off for the life of the sketch.
 * millis()/delay()/Serial do not work; Timer1 runs free as a timebase.
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

// ---------------------------------------------------------------- config ----

// Reset-line driver:
//   1 = NPN transistor on D3 (recommended): D3 HIGH turns it on, pulls reset low.
//   0 = D3 wired straight to the reset line through ~220 Ohm, used open-drain
//       (driven LOW to assert, released to high-Z; never driven HIGH).
#define RESET_VIA_NPN 0

// Button combo that triggers a reset. Combine any of the BTN_* masks below.
// Default: L + R + C-Down.
// Avoid L + R + Start: the controller itself treats that as "recenter stick"
// and reports Start as released while it is held.
#define RESET_COMBO (BTN_L | BTN_R | BTN_CDOWN)

// How long the combo must be held before resetting.
#define HOLD_MS 500

// How long the reset line is held low. The console starts its reset sequence
// on press; a short pulse is enough.
#define RESET_PULSE_MS 100

// ------------------------------------------------------------ button bits ---
// 16-bit button word = first two reply bytes, MSB first.
#define BTN_A      0x8000
#define BTN_B      0x4000
#define BTN_Z      0x2000
#define BTN_START  0x1000
#define BTN_DUP    0x0800
#define BTN_DDOWN  0x0400
#define BTN_DLEFT  0x0200
#define BTN_DRIGHT 0x0100
#define BTN_RST    0x0080  // set by the controller while L+R+Start is held
#define BTN_RSVD   0x0040  // always 0 in a valid reply
#define BTN_L      0x0020
#define BTN_R      0x0010
#define BTN_CUP    0x0008
#define BTN_CDOWN  0x0004
#define BTN_CLEFT  0x0002
#define BTN_CRIGHT 0x0001

// ------------------------------------------------------------------ pins ----
#define DATA_PIN_REG PIND
#define DATA_MASK    _BV(PD2)   // D2
#define RESET_DDR    DDRD
#define RESET_PORT   PORTD
#define RESET_MASK   _BV(PD3)   // D3
#define LED_DDR      DDRB
#define LED_PORT     PORTB
#define LED_MASK     _BV(PB5)   // D13

// ---------------------------------------------------------------- timing ----
// Console command (8 bits + stop bit) + controller reply (32 bits).
#define CMD_BITS   9
#define REPLY_BITS 32
#define TOTAL_BITS (CMD_BITS + REPLY_BITS)

// Delay between seeing a falling edge and sampling. The edge-wait loop adds
// roughly 2-6 cycles of latency, so 26 puts the sample near 2 us (32 cycles).
#define SAMPLE_DELAY_CYCLES 26

// The line must stay high this many loop passes (~8 cycles each, ~100 us)
// before we treat the next falling edge as the start of a new transaction.
// Bits inside a transaction are never more than a few us apart; polls are
// milliseconds apart.
#define IDLE_LOOPS 200

// Timer1 at clk/1024 = 15625 ticks per second.
#define MS_TO_TICKS(ms) ((uint16_t)((ms) * 15625UL / 1000UL))
#define HOLD_TICKS MS_TO_TICKS(HOLD_MS)

// Valid polls in a row the combo must be seen in, on top of HOLD_MS, so a
// single misread can never cause a reset.
#define MIN_MATCHING_POLLS 5

// ----------------------------------------------------------------------------

static uint8_t samples[TOTAL_BITS];

static inline void resetAssert() {
#if RESET_VIA_NPN
  RESET_PORT |= RESET_MASK;
#else
  RESET_DDR |= RESET_MASK;   // PORT bit is 0, so this drives LOW
#endif
}

static inline void resetRelease() {
#if RESET_VIA_NPN
  RESET_PORT &= ~RESET_MASK;
#else
  RESET_DDR &= ~RESET_MASK;  // high-Z, no pull-up
#endif
}

// Waits for an idle gap, then samples one full transaction into samples[].
// Returns false if the transaction ended early (wrong command, no reply, noise).
static bool capture() {
  // 1. Wait until the line has been continuously high for ~100 us.
  uint16_t n = IDLE_LOOPS;
  while (n) {
    if (DATA_PIN_REG & DATA_MASK) n--;
    else n = IDLE_LOOPS;
  }

  // 2. Wait as long as needed for the console to start a transaction.
  while (DATA_PIN_REG & DATA_MASK) {}

  // 3. Sample each bit ~2 us after its falling edge.
  uint8_t *p = samples;
  uint8_t i = TOTAL_BITS;
  for (;;) {
    __builtin_avr_delay_cycles(SAMPLE_DELAY_CYCLES);
    *p++ = DATA_PIN_REG;

    uint8_t t = 0;  // 256 passes ~ 90 us
    while (!(DATA_PIN_REG & DATA_MASK)) {
      if (--t == 0) return false;  // stuck low
    }
    if (--i == 0) return true;

    t = 0;
    while (DATA_PIN_REG & DATA_MASK) {
      if (--t == 0) return false;  // transaction ended before TOTAL_BITS
    }
  }
}

static uint8_t bitsToByte(const uint8_t *s) {
  uint8_t b = 0;
  for (uint8_t k = 0; k < 8; k++) {
    b = (b << 1) | ((s[k] & DATA_MASK) ? 1 : 0);
  }
  return b;
}

// Decodes samples[] into the 16-bit button word. Returns false unless it is a
// well-formed reply to a 0x01 poll.
static bool decode(uint16_t *buttons) {
  if (bitsToByte(&samples[0]) != 0x01) return false;
  uint16_t b = ((uint16_t)bitsToByte(&samples[CMD_BITS]) << 8) |
               bitsToByte(&samples[CMD_BITS + 8]);
  if (b & BTN_RSVD) return false;
  *buttons = b;
  return true;
}

void setup() {
  cli();

  // D2: plain input. No pull-up: that would put 5 V on the 3.3 V data line.
  DDRD &= ~DATA_MASK;
  PORTD &= ~DATA_MASK;

  // D3: reset driver, released.
  PORTD &= ~RESET_MASK;
#if RESET_VIA_NPN
  DDRD |= RESET_MASK;   // output LOW = transistor off
#else
  DDRD &= ~RESET_MASK;  // high-Z
#endif

  LED_DDR |= LED_MASK;

  // Timer1 free-running at clk/1024 as a timebase. No interrupts.
  TCCR1A = 0;
  TCCR1B = _BV(CS12) | _BV(CS10);
  TCNT1 = 0;
}

void loop() {
  bool armed = true;          // false after a reset until the combo is released
  uint8_t matching = 0;
  uint16_t holdStart = 0;
  uint8_t ledCount = 0;

  for (;;) {
    uint16_t buttons;
    if (!capture() || !decode(&buttons)) continue;

    // Heartbeat: toggle the LED every 30 valid polls (~1 Hz at 60 polls/s).
    if (++ledCount >= 30) {
      ledCount = 0;
      LED_PORT ^= LED_MASK;
    }

    if ((buttons & RESET_COMBO) != RESET_COMBO) {
      matching = 0;
      armed = true;
      continue;
    }
    if (!armed) continue;

    if (matching == 0) holdStart = TCNT1;
    if (matching < 255) matching++;

    if (matching >= MIN_MATCHING_POLLS &&
        (uint16_t)(TCNT1 - holdStart) >= HOLD_TICKS) {
      LED_PORT |= LED_MASK;
      resetAssert();
      _delay_ms(RESET_PULSE_MS);
      resetRelease();
      armed = false;   // re-arm only after the combo is let go
      matching = 0;
    }
  }
}

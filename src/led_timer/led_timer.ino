#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

// Pin definitions
const int kStatusLedPin = PIN_PA6; 
const int kButtonPin = PIN_PA7;
const int kLedDriverPin = PIN_PA3;
const int kVoltageSensePin = PIN_PA5;

// Times of day, in seconds since midnight
const uint32_t kMinute = 60UL;
const uint32_t kHour = 60 * kMinute;
const uint32_t kSecondsInDay = 24 * kHour;

// Power-up and long-press re-sync both set the clock to this time of day
const uint32_t kStartTimeOfDay = 20 * kHour;  // 8:00 PM

// Schedule boundaries (end of each phase, as time of day)
const uint32_t kPhaseEnds[] = {
  6 * kHour,                // Phase 0: 12 AM - 6 AM (OFF)
  8 * kHour + 30 * kMinute, // Phase 1: 6 AM - 8:30 AM (ON)
  16 * kHour,               // Phase 2: 8:30 AM - 4 PM (OFF)
  kSecondsInDay             // Phase 3: 4 PM - 12 AM (ON)
};

// Supply voltage monitoring
const uint16_t kLowVoltageThresholdMv = 3000;  // Cut LED below this
const uint16_t kRecoveryVoltageMv = 3200;      // Resume above this (hysteresis)

// Button and status LED behavior
const uint32_t kLongPressMs = 2000;           // Hold time for a long press (re-sync)
const uint32_t kDoubleClickWindowMs = 400;    // Max gap between clicks to count as a double-click
const uint32_t kVoltageDigitPauseMs = 1000;   // Gap before each blinked-out voltage digit
const uint32_t kVoltageBlinkHalfMs = 200;     // Half-period of the 4 Hz digit blink

enum ButtonEvent : uint8_t {
  BUTTON_NONE,
  BUTTON_SHORT,
  BUTTON_DOUBLE,
  BUTTON_LONG,
};

// Global state
volatile uint32_t secondsCounter = 0;
bool isOverridden = false;
bool isLowVoltage = false;
int lastKnownPhase = -1;
uint32_t lastMillis = 0;
uint16_t lastVoltageMv = 0;

// --- Setup & Loop ---

void setup() {
  // Disable input buffers on ALL pins first
  // This prevents the ~0.6mA floating pin leakage.
  for (uint8_t i = 0; i < 8; i++) {
    (&PORTA.PIN0CTRL)[i] = PORT_ISC_INPUT_DISABLE_gc;
    (&PORTB.PIN0CTRL)[i] = PORT_ISC_INPUT_DISABLE_gc;
  }

  if constexpr (kStatusLedPin > 0) {
    pinMode(kStatusLedPin, OUTPUT);
    digitalWrite(kStatusLedPin, HIGH);
  }

  // Configure LED driver pin
  digitalWrite(kLedDriverPin, LOW);
  pinMode(kLedDriverPin, OUTPUT);

  // Configure button
  pinMode(kButtonPin, INPUT_PULLUP);
  // Attach interrupt to wake CPU on press
  attachInterrupt(digitalPinToInterrupt(kButtonPin), wakeISR, CHANGE);

  // Disable ADC
  ADC0.CTRLA &= ~ADC_ENABLE_bm;

  // Clock the RTC from the external 32.768 kHz crystal and tick the PIT at 1 Hz
  RTC.CLKSEL = RTC_CLKSEL_TOSC32K_gc;  // Select External Crystal
  CCP = CCP_IOREG_gc;
  CLKCTRL.XOSC32KCTRLA |= CLKCTRL_ENABLE_bm;  // Enable Oscillator

  RTC.PITINTCTRL = RTC_PI_bm;                            // Enable PIT Interrupt
  RTC.PITCTRLA = RTC_PERIOD_CYC32768_gc | RTC_PITEN_bm;  // 1Hz Tick

  signalBlink();
}

void loop() {
  // Handle Button (blocking while button is pressed)
  if (digitalRead(kButtonPin) == LOW) {
    ButtonEvent evt = detectButtonEvent();
    switch (evt) {
      case BUTTON_SHORT: performShortPressAction(); break;
      case BUTTON_DOUBLE: performDoublePressAction(); break;
      case BUTTON_LONG:  performLongPressAction();  break;
      default: break;
    }
  }

  uint32_t currentSeconds = readCurrentSeconds();

  // Sample supply voltage every wake (~1 Hz). The ADC only stays enabled
  // for ~0.5 ms per read, so the extra current is negligible next to the
  // LED load and gives us immediate response to a battery drop.
  lastVoltageMv = measureSupplyVoltageMv();
  if (!isLowVoltage && lastVoltageMv < kLowVoltageThresholdMv) {
    isLowVoltage = true;
  } else if (isLowVoltage && lastVoltageMv >= kRecoveryVoltageMv) {
    isLowVoltage = false;
  }

  // In low-voltage mode, force LED off but keep the RTC ticking so we
  // still track time and can recover when the battery rises again.
  bool ledState = !isLowVoltage && getLedState(currentSeconds);

  digitalWrite(kLedDriverPin, ledState ? HIGH : LOW);

  // Go back to sleep (RTC and PIT continue to run)
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();

  // Ensure interrupts are on before sleeping
  sei();
  sleep_cpu();

  // CPU halts here until PIT (1s) or button interrupt

  sleep_disable();
}

ISR(RTC_PIT_vect) {
  RTC.PITINTFLAGS = RTC_PI_bm;
  secondsCounter++;
}

// Empty ISR just to wake the CPU from sleep on button press
void wakeISR() {
  // logic handled in loop()
}

void signalBlink() {
  const int pin = kStatusLedPin > 0 ? kStatusLedPin : kLedDriverPin;
  for (int i = 0; i < 3; i++) {
    digitalWrite(pin, HIGH);
    delay(150);
    digitalWrite(pin, LOW);
    delay(150);
  }
}

// Read current time (updated by PIT ISR)
uint32_t readCurrentSeconds() {
  noInterrupts();
  uint32_t result = secondsCounter;
  interrupts();
  return result;
}

// Button actions

// Blocking button handler: classifies the interaction and returns the event.
// Stays awake only while the user is holding / double-tapping.
ButtonEvent detectButtonEvent() {
  const uint32_t pressStart = millis();
  bool longPress = false;

  // Wait for first release (or hit long-press threshold)
  while (digitalRead(kButtonPin) == LOW) {
    if (!longPress && (millis() - pressStart > kLongPressMs)) {
      longPress = true;
    }
    delay(10);
  }

  if (longPress) return BUTTON_LONG;

  const uint32_t duration = millis() - pressStart;
  if (duration <= 10) return BUTTON_NONE;  // Bounce

  // Watch for a second press within the double-click window
  const uint32_t releaseTime = millis();
  while (millis() - releaseTime < kDoubleClickWindowMs) {
    if (digitalRead(kButtonPin) == LOW) {
      // Wait for the second press to release before returning
      delay(10);  // debounce
      while (digitalRead(kButtonPin) == LOW) delay(10);
      return BUTTON_DOUBLE;
    }
    delay(5);
  }

  return BUTTON_SHORT;
}

void performLongPressAction() {
  noInterrupts();
  secondsCounter = 0;
  interrupts();
  isOverridden = false;  // Reset overrides on time reset
  signalBlink();
}

void performShortPressAction() {
  isOverridden = !isOverridden;  // Toggle the override state

  if constexpr (kStatusLedPin > 0) {
    const uint32_t currentSeconds = readCurrentSeconds();
    const bool ledState = getLedState(currentSeconds);
    for (int i = 0; i < (ledState ? 2 : 1); i++) {
      digitalWrite(kStatusLedPin, HIGH);
      delay(150);
      digitalWrite(kStatusLedPin, LOW);
      delay(150);
    }
  }
}

void performDoublePressAction() {
  // Re-sample to show what the battery is doing right now
  lastVoltageMv = measureSupplyVoltageMv();
  showVoltageBlinks(lastVoltageMv);
}

// --- Voltage sensing ---

// Reads PA5 through the 1M/1M divider (with 10nF filter cap) and returns the
// estimated Vin in millivolts.
//
// The ADC uses VDD (the coin cell) as its reference, and the internal 1.1V
// reference is measured against VDD as well. The ratio of the two readings
// gives Vin independently of the coin cell voltage, and the 1.1V reference
// works down to VDD = 1.8V. (The 2.5V reference would need VDD >= 2.9V.)
// Vin reads up to 2 * VDD, e.g. ~6V with a fresh 3V coin cell.
uint16_t measureSupplyVoltageMv() {
  // Drive the ADC directly. megaTinyCore's analogRead() was returning
  // 2x the expected raw value here (evidently it enables SAMPNUM
  // accumulation internally), so we skip it.
  VREF.CTRLA = (VREF.CTRLA & ~VREF_ADC0REFSEL_gm) | VREF_ADC0REFSEL_1V1_gc;
  // The reference is only switched on when the ADC uses it as its reference,
  // so force it on while we measure it against VDD.
  VREF.CTRLB |= VREF_ADC0REFEN_bm;
  ADC0.CTRLB = 0;  // Single sample, no accumulation
  // Keep CLK_ADC within the datasheet's 200-1500 kHz (250 kHz at 1 MHz)
#if F_CPU <= 6000000UL
  ADC0.CTRLC = ADC_REFSEL_VDDREF_gc | ADC_PRESC_DIV4_gc | ADC_SAMPCAP_bm;
#else
  ADC0.CTRLC = ADC_REFSEL_VDDREF_gc | ADC_PRESC_DIV16_gc | ADC_SAMPCAP_bm;
#endif
  ADC0.CTRLA = ADC_ENABLE_bm | ADC_RESSEL_10BIT_gc;
  delayMicroseconds(50);  // Reference settling

  const uint32_t refSum = sumAdcSamples(ADC_MUXPOS_INTREF_gc);  // 1.1V * 1023 / VDD each
  const uint32_t senseSum = sumAdcSamples(ADC_MUXPOS_AIN5_gc);  // V_pa5 * 1023 / VDD each
  VREF.CTRLB &= ~VREF_ADC0REFEN_bm;  // Release the reference again
  if (refSum == 0) return 0;

  // V_pa5 = 1100 mV * senseSum / refSum; Vin = V_pa5 * 2
  return (uint16_t)((2200UL * senseSum + refSum / 2) / refSum);
}

// Selects an ADC input, discards the first conversion and returns the sum of
// kAdcSamples conversions.
const uint8_t kAdcSamples = 3;
uint32_t sumAdcSamples(uint8_t muxpos) {
  ADC0.MUXPOS = muxpos;
  uint32_t sum = 0;
  for (uint8_t i = 0; i <= kAdcSamples; i++) {
    ADC0.COMMAND = ADC_STCONV_bm;
    while (ADC0.COMMAND & ADC_STCONV_bm);
    const uint16_t result = ADC0.RES;
    if (i > 0) sum += result;
  }
  return sum;
}

// Blink one decimal digit at ~4 Hz. A zero digit is shown as 10 quick blinks
// so the user can still count something rather than a silent pause.
void blinkStatusDigit(uint8_t digit) {
  const uint8_t count = (digit == 0) ? 10 : digit;
  const int pin = kStatusLedPin > 0 ? kStatusLedPin : kLedDriverPin;
  for (uint8_t i = 0; i < count; i++) {
    digitalWrite(pin, HIGH);
    delay(kVoltageBlinkHalfMs);
    digitalWrite(pin, LOW);
    delay(kVoltageBlinkHalfMs);
  }
}

// Blinks out the voltage as: 1s pause, integer part, 1s pause, first
// decimal, 1s pause, second decimal.
void showVoltageBlinks(uint16_t mv) {
  const uint16_t cv = (mv + 5) / 10;  // Round to centivolts (e.g. 3754 -> 375)
  const uint8_t digits[3] = {
    (uint8_t)((cv / 100) % 10),
    (uint8_t)((cv / 10) % 10),
    (uint8_t)(cv % 10),
  };
  for (uint8_t i = 0; i < 3; i++) {
    delay(kVoltageDigitPauseMs);
    blinkStatusDigit(digits[i]);
  }
}

// --- Logic Helpers ---

int calculateCurrentPhase(uint32_t timeInDay) {
  for (int i = 0; i < 4; i++) {
    if (timeInDay < kPhaseEnds[i]) return i;
  }
  return 0;
}

bool getNaturalState(int phase) {
  // Phase 1 and 3 are ON, 0 and 2 are OFF
  return (phase == 1 || phase == 3);
}

bool getLedState(uint32_t seconds) {
  uint32_t timeInDay = (kStartTimeOfDay + seconds) % kSecondsInDay;
  int currentPhase = calculateCurrentPhase(timeInDay);

  // Clear override if we transition to a new phase
  if (currentPhase != lastKnownPhase) {
    isOverridden = false;
    lastKnownPhase = currentPhase;
  }

  // Determine LED state (invert natural state if overridden)
  bool ledState = getNaturalState(currentPhase);
  if (isOverridden) ledState = !ledState;
  return ledState;
}

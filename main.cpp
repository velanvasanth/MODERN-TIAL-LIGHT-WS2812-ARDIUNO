#include <Arduino.h>
#include <FastLED.h>

/*
  Audi-style LED tail light for ESP32-S3 + WS2812B  (like the HAZI TECH video)
  ----------------------------------------------------------------------------
  Four 12 V inputs (through voltage dividers!):  BRAKE, LEFT, RIGHT, REVERSE

    nothing active : dim red running light, brighter in the middle (DRL / tail)
    BRAKE          : whole strip bright red
    REVERSE        : whole strip white (with brake: white middle, red sides)
    LEFT / RIGHT   : amber sweeps from the centre out to that side, repeats;
                     the other half keeps the red base light
    LEFT + RIGHT   : hazard - both halves sweep outward together

  Power-up: red grows from the centre outwards, then settles to the running light.

  WIRING
    Strip DIN  -> GPIO5 through 330 ohm.   Strip 5V and GND from the 12V->5V buck
    converter (same 5V feeds the ESP32 5V/VIN pin).  All GND joined, incl. car GND.
    Each 12V input:  car wire --47k--+--> GPIO      (BRAKE=1, LEFT=2, RIGHT=3, REVERSE=4)
                                     +--10k--> GND
                                     +--100nF--> GND
    That gives 12 V -> 2.1 V, 14.8 V -> 2.6 V, 0 V -> 0 V (safe for 3.3 V pins).
    Do NOT use the 22k/10k divider from the video: it gives 3.75 V at 12 V and
    4.6 V at 14.8 V, which is too high for the ESP32-S3.
    Recommended: 3.3 V zener or TVS diode from each GPIO to GND, and a fuse on the
    12 V feed. Car wiring has voltage spikes.

  BENCH TEST without a car: open the Serial Monitor (115200) and type
    b = brake   l = left   r = right   v = reverse   h = hazard   (each toggles)
*/

// ---------------- Settings ----------------
#define LED_PIN          5
#define NUM_LEDS         32
#define MAX_MILLIAMPS    2000      // FastLED dims automatically to stay under this
#define LEFT_IS_PIXEL_0  1         // 1: pixel 0 is the LEFT end (seen from behind the car). 0: flip.

#define PIN_BRAKE        1
#define PIN_LEFT         2
#define PIN_RIGHT        3
#define PIN_REVERSE      4

#define ON_MV            1300      // input counts as ON above this (mV at the GPIO)
#define OFF_MV           800       // ... and OFF below this
#define DEBOUNCE_MS      30
#define IND_HOLD_MS      600       // keep animating while the car's flasher is in its off phase

#define DRL_EDGE         30        // running light brightness at the sides (0-255)
#define DRL_CENTER       100       // running light brightness in the middle third
#define BRAKE_LEVEL      255

#define IND_PERIOD_MS    800       // one indicator flash cycle
#define IND_SWEEP_MS     280       // time for the amber to sweep centre -> edge
#define IND_ON_MS        440       // amber stays on until this point, then off until the period ends

#define BLEND_AMOUNT     70        // 0-255: higher = snappier, lower = softer glow
#define FRAME_MS         8

static const CRGB AMBER(255, 130, 0);

// ---------------- Inputs ----------------
struct Input {
  uint8_t  pin;
  bool     state;        // debounced ON/OFF
  bool     pending;
  uint32_t pendingSince;
  uint32_t lastOn;       // last time the input was ON
  bool     virt;         // set from the Serial test keys
};

Input inBrake   = { PIN_BRAKE,   false, false, 0, 0, false };
Input inLeft    = { PIN_LEFT,    false, false, 0, 0, false };
Input inRight   = { PIN_RIGHT,   false, false, 0, 0, false };
Input inReverse = { PIN_REVERSE, false, false, 0, 0, false };

int readMv(uint8_t pin) {
  uint32_t s = 0;
  for (int k = 0; k < 4; k++) s += analogReadMilliVolts(pin);
  return s / 4;
}

void updateInput(Input& in, uint32_t now) {
  bool cand;
  if (in.virt) cand = true;
  else {
    int mv = readMv(in.pin);
    cand = in.state ? (mv > OFF_MV) : (mv > ON_MV);   // hysteresis
  }
  if (cand == in.state) in.pending = false;
  else if (!in.pending) { in.pending = true; in.pendingSince = now; }
  else if (now - in.pendingSince >= DEBOUNCE_MS) { in.state = cand; in.pending = false; }
  if (in.state) in.lastOn = now;
}

// An indicator wire is pulsed by the car's flasher, so stay "active" through the off phase.
bool indicatorActive(const Input& in, uint32_t now) {
  return in.state || (in.lastOn != 0 && (now - in.lastOn) < IND_HOLD_MS);
}

// ---------------- Rendering ----------------
CRGB leds[NUM_LEDS];
CRGB target[NUM_LEDS];
uint32_t indStart = 0;
bool indRunning = false;
uint32_t lastFrame = 0;

void buildTarget(uint32_t now, bool brake, bool rev, bool left, bool right) {
  const int third = NUM_LEDS / 3;

  // base layer: running light / brake / reverse
  for (int i = 0; i < NUM_LEDS; i++) {
    bool center = (i >= third && i < NUM_LEDS - third);
    CRGB c;
    if (brake) c = CRGB(BRAKE_LEVEL, 0, 0);
    else       c = CRGB(center ? DRL_CENTER : DRL_EDGE, 0, 0);
    if (rev && (!brake || center)) c = CRGB(255, 255, 255);
    target[i] = c;
  }

  // indicator layer: amber sweeping from the centre outwards
  if (left || right) {
    uint32_t ph = (now - indStart) % IND_PERIOD_MS;
    if (ph < IND_ON_MS) {
      const int mid = NUM_LEDS / 2;
      const int sizeLeft  = LEFT_IS_PIXEL_0 ? mid : (NUM_LEDS - mid);
      const int sizeRight = NUM_LEDS - sizeLeft;

      auto litCount = [&](int size) -> int {
        if (ph >= IND_SWEEP_MS) return size;
        return (int)(((uint32_t)ph * size) / IND_SWEEP_MS) + 1;
      };

      if (left) {
        int n = litCount(sizeLeft);
        for (int k = 0; k < n; k++) {
          int idx = LEFT_IS_PIXEL_0 ? (mid - 1 - k) : (mid + k);
          target[idx] = AMBER;
        }
      }
      if (right) {
        int n = litCount(sizeRight);
        for (int k = 0; k < n; k++) {
          int idx = LEFT_IS_PIXEL_0 ? (mid + k) : (mid - 1 - k);
          target[idx] = AMBER;
        }
      }
    }
  }
}

// ---------------- Bench test via Serial ----------------
void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'b') inBrake.virt = !inBrake.virt;
    else if (c == 'l') inLeft.virt = !inLeft.virt;
    else if (c == 'r') inRight.virt = !inRight.virt;
    else if (c == 'v') inReverse.virt = !inReverse.virt;
    else if (c == 'h') { bool on = !(inLeft.virt && inRight.virt); inLeft.virt = on; inRight.virt = on; }
    else continue;
    Serial.printf("test inputs: brake=%d left=%d right=%d reverse=%d\n",
                  inBrake.virt, inLeft.virt, inRight.virt, inReverse.virt);
  }
}

// ---------------- Arduino ----------------
void startupAnimation() {
  const int mid = NUM_LEDS / 2;
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
  for (int k = 0; k < mid + 1; k++) {
    if (mid - 1 - k >= 0)      leds[mid - 1 - k] = CRGB(255, 0, 0);
    if (mid + k < NUM_LEDS)    leds[mid + k]     = CRGB(255, 0, 0);
    FastLED.show();
    delay(35);
  }
}

void setup() {
  Serial.begin(115200);
  analogSetAttenuation(ADC_11db);                 // read up to ~3.1 V
  pinMode(PIN_BRAKE, INPUT);
  pinMode(PIN_LEFT, INPUT);
  pinMode(PIN_RIGHT, INPUT);
  pinMode(PIN_REVERSE, INPUT);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  FastLED.setBrightness(255);
  startupAnimation();
  Serial.println("Tail light ready. Test keys: b l r v h");
}

void loop() {
  uint32_t now = millis();
  handleSerial();

  updateInput(inBrake, now);
  updateInput(inLeft, now);
  updateInput(inRight, now);
  updateInput(inReverse, now);

  bool brake = inBrake.state;
  bool rev   = inReverse.state;
  bool left  = indicatorActive(inLeft, now);
  bool right = indicatorActive(inRight, now);

  bool anyInd = left || right;
  if (anyInd && !indRunning) { indRunning = true; indStart = now; }   // start a new flash cycle
  if (!anyInd) indRunning = false;

  if (now - lastFrame >= FRAME_MS) {
    lastFrame = now;
    buildTarget(now, brake, rev, left, right);
    for (int i = 0; i < NUM_LEDS; i++) nblend(leds[i], target[i], BLEND_AMOUNT);   // soft glow
    FastLED.show();
  }
}

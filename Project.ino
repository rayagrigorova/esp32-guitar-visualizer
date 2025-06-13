// Guitar Color-Music – external button on GPIO-14
// ------------------------------------------------------
// This program runs on an ESP32 and visualizes guitar sounds
// by lighting up a ring of NeoPixel (WS2812) LEDs in various colors.
// It receives audio frequency data from a computer
// over Serial, and changes its display mode when a button is pressed.

#include <FastLED.h>

// ------------------------
// User-adjustable settings
// ------------------------
#define LED_PIN        13      // data line to NeoPixel ring
#define NUM_LEDS       16      // Number of leds on the ring
#define BUTTON_PIN     14      // The GPIO pin connected to the external button
#define MAX_BRIGHT    180      // Maximum LED brightness (0–255). Set lower to avoid blinding output or reduce power draw
#define NOISE_GATE      8      // total level below this -> LEDs off
#define SMOOTH         40      // Transition speed between colors: 0 = instant change, 255 = ultra-slow fade

// ------------------------
// Color & Frequency Tables
// ------------------------
// Each entry in this array defines a hue (color) for a specific audio band (0–7)
// The full spectrum is: red -> orange -> yellow -> green -> blue -> violet -> pink
const uint8_t HUE_TABLE[8] = {
  /*0*/   0,   // red
  /*1*/  16,   // orange
  /*2*/  32,   // yellow
  /*3*/  80,   // green
  /*4*/ 140,   // blue-cyan
  /*5*/ 190,   // blue-violet
  /*6*/ 224,   // violet
  /*7*/ 250    // pink
};

// These are "gain factors" to boost higher frequencies, which are quieter
// This helps the guitar's high notes become more visually prominent
const uint8_t GAIN_TABLE[8] = {1,1,1,2,3,4,6,8};

// ------------------------
// Display Modes
// ------------------------
// SPECTRUM: LEDs all show one color based on sound frequency
// GRADIENT: Each LED shows a different band level as a color
// PURPLE: Non-reactive pulsing purple
// AMBIENT: Non-reactive slow rainbow
enum Mode : uint8_t { SPECTRUM = 0, GRADIENT, PURPLE, AMBIENT };

// Flag set by the button interrupt. Used in the main loop to trigger a mode change
volatile bool buttonIRQ = false;

// Current display mode (starts in SPECTRUM mode)
Mode mode = SPECTRUM;

// Array holding the state of each LED (color)
CRGB    leds[NUM_LEDS];

// Array to store the sound energy levels of 8 frequency bands (from Serial)
uint8_t bands[8] = {0};

// Interrupt function that runs when the button is pressed
// It sets a flag so we can change the mode in the main loop
void IRAM_ATTR handleButtonISR() { buttonIRQ = true; }

void setup()
{
  Serial.begin(500000);

  pinMode(BUTTON_PIN, INPUT_PULLUP); // Set button pin as input with internal pull-up resistor
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN),
                  handleButtonISR, FALLING); // Trigger interrupt on button press (falling edge)

  // Set up the LED strip (using WS2812B LEDs, GRB color order)
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS); 
  FastLED.setBrightness(MAX_BRIGHT); // Set the max brightness of all LEDs
}

// ------------------------
// Read incoming Serial data
// ------------------------
// Waits for a full frame of 8 bytes (after a 0xFF start byte)
// Returns true when a complete frame is received
// A frame is in the format [0xFF][band0][band1][band2][band3][band4][band5][band6][band7]
// Example: [0xFF, 21, 89, 182, 137, 20, 5, 0, 0]
bool readFrame()
{
  static bool frame = false;  // Whether we are currently inside a frame
  static uint8_t idx = 0;  // Position in the 8-byte frame

  while (Serial.available()) // While there is data to read
  {
    const uint8_t b = Serial.read(); // Read one byte

    if (!frame)
    {
      if (b == 0xFF) { frame = true; idx = 0; } // Start of a new frame
      else if (b == 'm' || b == 'M') buttonIRQ = true; // If 'm' is received, switch mode
    }
    else
    {
      bands[idx++] = b; // Store the byte into the bands array
      if (idx == 8) { frame = false; return true; } // If we have 8 bytes, frame is complete
    }
  }
  return false; // Frame not yet complete
}

void loop()
{
  if (!readFrame()) return; // Skip loop until we have audio data

  // Check for button press
  static uint32_t lastPress = 0; // Time of the last valid button press
  if (buttonIRQ)
  {
    buttonIRQ = false; // Clear the flag
    uint32_t now = millis(); // Current time in milliseconds

    // Only count the button press if at least 150 ms have passed since the last one.
    if (now - lastPress > 150) // Debounce: ignore presses within 150ms
    {
      mode = static_cast<Mode>((mode + 1) % 4); // Move to next mode
      lastPress = now; // Update last press time
    }
  }

  // ------------------------
  // Calculate total energy
  // ------------------------
  const uint16_t low   = bands[0] + bands[1] + bands[2];
  const uint16_t mid   = bands[3] + bands[4];
  const uint16_t high  = bands[5] + bands[6] + bands[7];
  const uint16_t total = low + mid + high;

  // ------------------------
  // Set LED brightness based on total volume
  // ------------------------
  // Calculate brightness based on total sound energy:
  // - If total is below NOISE_GATE, set brightness to 0 (silent -> LEDs off)
  // - Otherwise, divide total by 4 to get a base brightness level
  // - constrain(...) keeps the brightness between a minimum (10) and maximum (MAX_BRIGHT)
  //   - Minimum of 10 ensures dim sounds are still visible
  //   - Maximum (180) prevents blinding brightness or power overdraw
  const uint8_t brightness =
      total < NOISE_GATE ? 0 : constrain(total / 4, 10, MAX_BRIGHT);
  FastLED.setBrightness(brightness);

  // Used for smooth fading between colors
  static CRGB current = CRGB::Black, target = CRGB::Black;

  switch (mode)
  {
    // ========================================
    // MODE 0: SPECTRUM – all LEDs show one color
    // ========================================
    case SPECTRUM:
    {
      if (brightness == 0) {
        target = CRGB::Black; // If no sound, turn off LEDs
      } else {
        // Calculate the average band index, weighted by energy and gain
        // This will help us determine what color to show, based on the dominant pitch.
        uint32_t idxNum = 0; // This will hold the weighted sum of band indexes (numerator)
        uint32_t idxDen = 0; // This will hold the total weight (denominator)

        for (uint8_t i = 0; i < 8; ++i) {
          // Apply psychoacoustic gain to each band.
          // This gives more importance to higher-frequency bands (which are usually quieter)
          uint16_t w = uint16_t(bands[i]) * GAIN_TABLE[i];

          // Add this band's contribution to the weighted index sum
          // We multiply the band index (0–7) by 100 to keep decimal precision for later.
          idxNum += uint32_t(i * 100) * w; 

          // Accumulate the total weight (used as divisor later)
          idxDen += w;
        }

        // Calculate the average band index (like a "center of mass")
        // If idxDen is zero (to avoid division by zero), fallback to 0.0
        float idxF = idxDen ? float(idxNum) / idxDen / 100.0f : 0.0f;

        uint8_t idxLo = floor(idxF); // Lower band index (e.g., 2)
        uint8_t idxHi = min<uint8_t>(idxLo + 1, 7); // Upper band index (e.g., 3), clamped to max 7
        float    frac = idxF - idxLo; // How far we are between idxLo and idxHi (e.g., 0.7)

        // Interpolate between the two hues using linear interpolation
        // If idxF = 2.7, and hues are [Yellow, Green], the result will be a yellow-green blend
        uint8_t  hue  = HUE_TABLE[idxLo] +
                        uint8_t(frac * (int16_t(HUE_TABLE[idxHi]) - HUE_TABLE[idxLo]));

        // Create the target color from the resulting hue (full saturation and brightness)
        target = CHSV(hue, 255, 255);
      }

      // Smoothly blend the current color toward the new target color
      // 'SMOOTH' defines the speed of this transition
      nblend(current, target, SMOOTH);
      // Set all LEDs in the ring to the same blended color
      fill_solid(leds, NUM_LEDS, current);
      break;
    }

    
    // ========================================
    // MODE 1: GRADIENT – show levels for each band
    // ========================================
    case GRADIENT:
    {
      for (uint8_t i = 0; i < NUM_LEDS; ++i)
      {
        uint8_t band  = i & 0x07;  // Choose band 0–7 in repeating pattern
        uint16_t vRaw = bands[band]; // Get raw band level

        // Apply gain to boost high-frequency visibility
        uint16_t vLev = vRaw * GAIN_TABLE[band]; // This is a brightness value for a specific LED, based on how strong that frequency band is
        vLev = qadd8(min<uint16_t>(vLev, 255), 5); // Add a small floor so dim bands are still visible

        leds[i] = CHSV(HUE_TABLE[band], 255, vLev); // Assign color and brightness to LED
      }
      break;
    }

    // ========================================
    // MODE 2: PURPLE – pulsing, non-reactive
    // ========================================
    case PURPLE:
    {
      // This variable keeps track of the animation phase (how far along the pulse is)
      // 'static' ensures the value is remembered between function calls
      static uint8_t phase = 0; 

      // Increment the phase on every loop to animate the pulsing effect
      phase++; 

      current = CHSV(200, 180, 255); // Base purple color

      // Use a sine wave to modulate the brightness over time
      // sin8(phase) returns a value that smoothly goes from 0 to 255 and back, like a wave
      // Adding 120 ensures the brightness never gets too dim (keeps the light softly visible)
      current.nscale8_video(sin8(phase) + 120);

      fill_solid(leds, NUM_LEDS, current); // Fill all LEDs with purple
      break;
    }

    // ========================================
    // MODE 3: AMBIENT – slow rainbow, non-reactive
    // ========================================
    case AMBIENT:
    {
      // This variable holds the current hue (color position on the color wheel)
      // 'static' means it remembers its value between loop runs
      static uint8_t hue = 0;

      // Slowly increment the hue to cycle through colors over time (0–255 -> wraps around to 0)
      hue++;

      // Set all LEDs to the same color using HSV:
      // - hue: changes gradually to animate a rainbow effect
      // - saturation 200: gives rich colors without being overly neon
      // - brightness 120: keeps the light soft, not blinding
      fill_solid(leds, NUM_LEDS, CHSV(hue, 200, 120));

      break;
    }
  }

  FastLED.show(); // Send LED data to the strip
}

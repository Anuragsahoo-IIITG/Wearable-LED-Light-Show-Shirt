// ----------------------------------------------------------------------------
// Gemini_client_websockets.ino
//
// ESP32 “client” for receiving LED‐effect messages from main.py (FastAPI / WebSocket).
// Supports multiple simultaneous effects (FFT, STATIC, BLINK, RAINBOW, CHASE),
// each targeting disjoint or overlapping LED ranges. Keeps everything in sync
// with the server via a local “chunk counter” (one chunk ≈ 1024 samples @ 44.1 kHz ≈ 23 ms).
//
// Libraries needed (install via Library Manager if missing):
//   • ArduinoJson 6.x
//   • FastLED
//   • WebSocketsClient (https://github.com/Links2004/arduinoWebSockets)
// ----------------------------------------------------------------------------

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <FastLED.h>

// ——————————————————————————————————————————————————————————
// === USER CONFIGURATION ===

// Your Wi-Fi credentials:
static const char*  WIFI_SSID      = "Your_Wifi_Name";
static const char*  WIFI_PASSWORD  = "Your_Password";

// The WebSocket URL of your FastAPI server:
//    e.g. "ws://192.168.1.100:8000/ws/esp32"
static const char*  WS_SERVER_ADDR = "x.x.x.x";  
static const uint16_t WS_SERVER_PORT = 8000;
static const char*   WS_PATH       = "/ws/esp32";

// A unique ID for this ESP32. Must match what the server expects. 
// (In main.py, the ESP does `device_id = await websocket.receive_text()` right after connect.)
static const char*  DEVICE_ID      = "ESP32_1";
static unsigned long lastHeartbeat = 0;

unsigned long lastFftTimestamp = 0;    // millis() at which the last FFT packet arrived
const unsigned long FFT_TIMEOUT_MS = 50; // how long (ms) to wait before considering FFT “stopped”

// WS2812B / FastLED settings:
#define LED_PIN      5
#define NUM_LEDS     300      // Maximum number of LEDs on this strip (adjust as needed)
#define COLOR_ORDER  GRB
#define CHIPSET      WS2812B
#define BRIGHTNESS   255

// Audio chunk parameters (must match what main.py uses):
static const uint32_t SAMPLE_RATE   = 44100;
static const uint32_t CHUNK_SIZE    = 1024;
// One “chunk” duration in milliseconds ≈ 1000 * CHUNK_SIZE / SAMPLE_RATE
static const uint32_t CHUNK_MS      = (CHUNK_SIZE * 1000ul) / SAMPLE_RATE;

// ——————————————————————————————————————————————————————————
// === DATA STRUCTURES FOR EFFECTS ===

CRGB leds[NUM_LEDS];

// Keep a global chunk counter:
static volatile uint32_t current_chunk = 0;
static uint32_t last_chunk_millis = 0;

// Each effect type gets its own small struct and array of “active events.”
// We’ll allow up to, say, 8 concurrent events of each kind. Feel free to enlarge if needed.
#define MAX_EVENTS   8

// 1) STATIC
struct StaticEvent {
  bool   active;
  uint32_t start_chunk;
  uint32_t end_chunk;
  uint16_t start_led, end_led;
  CRGB   color;
};
StaticEvent staticEvents[MAX_EVENTS];

// 2) BLINK
struct BlinkEvent {
  bool   active;
  uint32_t start_chunk;
  uint32_t end_chunk;
  uint16_t start_led, end_led;
  CRGB   on_color;
  CRGB   off_color;
  uint16_t base_rate;    // in chunks (slowest blink when quiet)
  uint16_t min_rate;     // in chunks (fastest blink when loud)
  // For simplicity, we’ll blink at a constant half‐period of base_rate. 
  // (If you want to tie blink speed to local audio RMS, you can replace this.)
};
BlinkEvent blinkEvents[MAX_EVENTS];

// 3) RAINBOW
struct RainbowEvent {
  bool   active;
  uint32_t start_chunk;
  uint32_t end_chunk;
  uint16_t start_led, end_led;
  float  hue_speed;     // how much to shift hue per chunk (0.0..1.0 wraps around)
  float  hue_offset;    // internal running hue offset (0.0..1.0)
};
RainbowEvent rainbowEvents[MAX_EVENTS];

// 4) CHASE
struct ChaseEvent {
  bool   active;
  uint32_t start_chunk;
  uint32_t end_chunk;
  uint16_t start_led, end_led;
  CRGB   chase_color;
  uint16_t tail_length; // how many LEDs “trail” behind the head
  bool   direction;     // true=FORWARD (increasing index), false=BACKWARD
  uint16_t base_speed;  // slowest: chunks per LED step
  uint16_t max_speed;   // fastest: chunks per LED step
  uint16_t head_pos;    // current head position (0..(end_led-start_led))
  uint32_t last_move_chunk; // last chunk at which we advanced head_pos
};
ChaseEvent chaseEvents[MAX_EVENTS];

// 5) FFT (per‐chunk full‐range RGB)
struct FFTEvent {
  bool   active;        // we actually don’t “schedule” FFT the same way; main.py will push an FFT message every chunk
  uint16_t start_led, end_led;
  CRGB   color;         // updated whenever a new FFT JSON arrives
};
FFTEvent fftEvent[MAX_EVENTS];      // Only one FFT range per device is assumed. If you need multiple, turn this into an array.

// 6) SPARKLE
struct SparkleEvent {
  bool active;
  uint32_t start_chunk;
  uint32_t end_chunk;
  uint8_t duration;
  CRGB base_color;
  uint8_t leds[30];
  uint8_t count;
};
SparkleEvent sparkleEvents[MAX_EVENTS];

// 7) STROBE (immediate update like FFT)
struct StrobeEvent {
  bool active;
  uint16_t start_led, end_led;
  CRGB color; // The calculated color (on_color or off_color) sent from Python
  uint32_t last_update_chunk; // To manage deactivation if no new messages
};
StrobeEvent strobeEvents[MAX_EVENTS];

// 8) COMET
// struct CometEvent {
//   bool active;
//   uint32_t start_chunk;
//   uint32_t end_chunk;
//   uint16_t start_led, end_led;
//   CRGB comet_color;
//   uint16_t tail_length;
//   uint16_t current_speed; // chunks per LED step (smaller = faster) - dynamic from Python
//   int16_t head_pos;    // current head position relative to start_led
//   uint32_t last_move_chunk; // last chunk at which head_pos was updated
//   bool direction; // true=FORWARD (increasing index), false=BACKWARD
// };
struct CometEvent {
  bool active;
  uint32_t start_chunk;
  uint32_t end_chunk;
  uint16_t start_led, end_led;
  CRGB comet_color;
  uint8_t comet_count;  // Number of comets
  uint8_t comet_size;   // Size of each comet
};
CometEvent cometEvents[MAX_EVENTS];

// 9) FIRE
struct FireEvent {
  bool active;
  // uint32_t start_chunk;
  // uint32_t end_chunk;
  uint16_t start_led, end_led;
  CRGB base_color;
  float flicker_intensity; // 0.0 to 1.0 - dynamic from Python
  // Additional internal state for fire, e.g., heat array, might be added for more realism.
  // For simplicity, we'll implement a basic flicker based on provided parameters.
};
FireEvent fireEvents[MAX_EVENTS];

// ——————————————————————————————————————————————————————————
// === WEBSOCKET CLIENT ===

WebSocketsClient webSocket;
bool isWSEventAttached = false;
bool isRegistered = false;

void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  // Called whenever a text message arrives (JSON) or connection status changes
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected!");
      isRegistered = false;
      break;

    case WStype_CONNECTED:
      Serial.println("[WS] Connected to “" + String(WS_SERVER_ADDR) + ":" + String(WS_SERVER_PORT) + WS_PATH + "”");
      // Send our DEVICE_ID as soon as we connect
      webSocket.sendTXT(DEVICE_ID);
      isRegistered = true;
      break;

    case WStype_TEXT: {
      Serial.print("[WS] Raw received JSON: "); // <-- ADD THIS LINE
      Serial.println((char*)payload);           // <-- ADD THIS LINE
      // We got a full JSON message in payload (length bytes).
      // Parse it, then dispatch to effect‐handlers
      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, payload, length);
      if (err) {
        Serial.println("[WS] JSON parse error:");
        Serial.println(err.c_str());
        return;
      }

      // Extract common fields:
      uint32_t chunk_idx = doc["chunk_index"] | 0; 
      JsonArray ledsArr = doc["leds"].as<JsonArray>();
      uint16_t led_start = ledsArr[0];
      uint16_t led_end   = ledsArr[1];
      const char* effect = doc["effect_type"] | "UNKNOWN";

      // Determine effective start chunk to account for server/ESP32 desync
      uint32_t effective_start_chunk = current_chunk;
      if(chunk_idx > current_chunk) {
        effective_start_chunk = chunk_idx;
      }

      // 1) FFT: immediate override of [led_start..led_end]
      if (strcmp(effect, "FFT") == 0) {
        JsonArray rgbArr = doc["rgb"].as<JsonArray>();
        uint8_t R = rgbArr[0], G = rgbArr[1], B = rgbArr[2];
        for (int i = 0; i < MAX_EVENTS; i++){
        if(!fftEvent[i].active){
        fftEvent[i].active    = true;
        fftEvent[i].start_led = led_start;
        fftEvent[i].end_led   = led_end;
        fftEvent[i].color     = CRGB(R, G, B);
        // We apply this color in loop() on the next chunk tick.
        lastFftTimestamp = millis();
        }
        }
      }

      // 2) STATIC: one‐time schedule
      if (strcmp(effect, "STATIC") == 0) {
        uint32_t dur = doc["duration_chunks"] | 1;
        JsonArray rgbArr = doc["rgb"].as<JsonArray>();
        CRGB col = CRGB((uint8_t)rgbArr[0], (uint8_t)rgbArr[1], (uint8_t)rgbArr[2]);
        // Find a free slot:
        for (int i = 0; i < MAX_EVENTS; i++) {
          if (!staticEvents[i].active) {
            staticEvents[i].active       = true;
            staticEvents[i].start_chunk  = effective_start_chunk;
            staticEvents[i].end_chunk    = effective_start_chunk + dur - 1;
            // ----------------------------
            staticEvents[i].start_led    = led_start;
            staticEvents[i].end_led      = led_end;
            staticEvents[i].color        = col;

            // --- ADD THESE LINES ---
            Serial.print("[STATIC] Scheduled: Chunk ");
            Serial.print(staticEvents[i].start_chunk);
            Serial.print(" to ");
            Serial.print(staticEvents[i].end_chunk);
            Serial.print(", LEDs ");
            Serial.print(staticEvents[i].start_led);
            Serial.print(" to ");
            Serial.print(staticEvents[i].end_led);
            Serial.print(", Color: ");
            Serial.print(staticEvents[i].color.red);
            Serial.print(",");
            Serial.print(staticEvents[i].color.green);
            Serial.print(",");
            Serial.println(staticEvents[i].color.blue);
            // -----------------------
            break;
          }
        }
      }

      // 3) BLINK: schedule ON/OFF toggling
      else if (strcmp(effect, "BLINK") == 0) {
        uint32_t dur = doc["duration_chunks"] | 1;
        JsonObject p = doc["params"];
        CRGB onC  = CRGB((uint8_t)p["on_color"][0],  (uint8_t)p["on_color"][1],  (uint8_t)p["on_color"][2]);
        CRGB offC = CRGB((uint8_t)p["off_color"][0], (uint8_t)p["off_color"][1], (uint8_t)p["off_color"][2]);
        uint16_t base_r = p["base_rate"] | 8;
        uint16_t min_r  = p["min_rate"]  | 2;
        for (int i = 0; i < MAX_EVENTS; i++) {
          if (!blinkEvents[i].active) {
            blinkEvents[i].active       = true;
            blinkEvents[i].start_chunk  = effective_start_chunk;
            blinkEvents[i].end_chunk    = effective_start_chunk + dur - 1;
            blinkEvents[i].start_led    = led_start;
            blinkEvents[i].end_led      = led_end;
            blinkEvents[i].on_color     = onC;
            blinkEvents[i].off_color    = offC;
            blinkEvents[i].base_rate    = base_r;
            blinkEvents[i].min_rate     = min_r;
            break;
          }
        }
      }

      // 4) RAINBOW: schedule a gradient
      else if (strcmp(effect, "RAINBOW") == 0) {
        uint32_t dur = doc["duration_chunks"] | 1;
        float hs = doc["params"]["hue_speed"] | 0.05f;
        for (int i = 0; i < MAX_EVENTS; i++) {
          if (!rainbowEvents[i].active) {
            rainbowEvents[i].active       = true;
            rainbowEvents[i].start_chunk  = effective_start_chunk;
            rainbowEvents[i].end_chunk    = effective_start_chunk + dur - 1;
            rainbowEvents[i].start_led    = led_start;
            rainbowEvents[i].end_led      = led_end;
            rainbowEvents[i].hue_speed    = hs;
            rainbowEvents[i].hue_offset   = 0.0f;
            break;
          }
        }
      }

      // 5) CHASE: schedule a moving‐tail
      else if (strcmp(effect, "CHASE") == 0) {
        uint32_t dur = doc["duration_chunks"] | 1;
        JsonObject p = doc["params"];
        CRGB chaseC = CRGB((uint8_t)p["chase_color"][0], (uint8_t)p["chase_color"][1], (uint8_t)p["chase_color"][2]);
        uint16_t tailL = p["tail_length"]  | 5;
        bool dir = (String((const char*)p["direction"].as<const char*>()) == "FORWARD");
        uint16_t bs  = p["base_speed"] | 10;
        uint16_t ms  = p["max_speed"]  | 3;
        for (int i = 0; i < MAX_EVENTS; i++) {
          if (!chaseEvents[i].active) {
            chaseEvents[i].active         = true;
            chaseEvents[i].start_chunk    = effective_start_chunk;
            chaseEvents[i].end_chunk      = effective_start_chunk + dur - 1;
            chaseEvents[i].start_led      = led_start;
            chaseEvents[i].end_led        = led_end;
            chaseEvents[i].chase_color    = chaseC;
            chaseEvents[i].tail_length    = tailL;
            chaseEvents[i].direction      = dir;
            chaseEvents[i].base_speed     = bs;
            chaseEvents[i].max_speed      = ms;
            chaseEvents[i].head_pos       = 0;
            chaseEvents[i].last_move_chunk = chunk_idx;
            break;
          }
        }
      }

      // ... (existing handlers for FFT, STATIC, BLINK, RAINBOW, CHASE) ...

      // SPARKLE: per-chunk update or schedule
      else if (strcmp(effect,"SPARKLE") == 0) {
  for (int i = 0; i < MAX_EVENTS; i++) {
    if (!sparkleEvents[i].active) {
      sparkleEvents[i].active = true;
      sparkleEvents[i].start_chunk = current_chunk;
      sparkleEvents[i].duration = doc["duration"] | 2;

      sparkleEvents[i].base_color = CRGB(
        doc["rgb"][0] | 0,
        doc["rgb"][1] | 0,
        doc["rgb"][2] | 0
      );

      // JsonArray ledArray = doc["leds"];
      // int count = ledArray.size();
      // if (count > 30) count = 30;
      // sparkleEvents[i].count = count;

      // for (int j = 0; j < count; j++) {
      //   sparkleEvents[i].leds[j] = ledArray[j] | 0;
      // }

       // Read or generate count
      int count = doc["count"] | random(1, led_end - led_start + 1);
      sparkleEvents[i].count = count;

      // Generate random LEDs within the range
      for (int j = 0; j < count && j < 30; j++) {
        uint16_t randomLed = random(led_start, led_end + 1);
        sparkleEvents[i].leds[j] = randomLed;
      }

      break;
    }
  }
}


      // STROBE: immediate update (like FFT)
      else if (strcmp(effect, "STROBE") == 0) {
        JsonArray rgbArr = doc["rgb"].as<JsonArray>();
        CRGB strobe_color = CRGB((uint8_t)rgbArr[0], (uint8_t)rgbArr[1], (uint8_t)rgbArr[2]);

        bool found_active_event = false;
        for (int i = 0; i < MAX_EVENTS; i++) {
            if (strobeEvents[i].active && strobeEvents[i].start_led == led_start && strobeEvents[i].end_led == led_end) {
                strobeEvents[i].color = strobe_color;
                strobeEvents[i].last_update_chunk = effective_start_chunk;
                found_active_event = true;
                Serial.print("[STROBE] Updated: Chunk "); Serial.print(effective_start_chunk);
                Serial.print(", LEDs "); Serial.print(led_start); Serial.print(" to "); Serial.print(led_end);
                //Serial.print(", Color: "); Serial.print(sparkleC.red); Serial.print(","); Serial.print(sparkleC.green); Serial.print(","); Serial.print(sparkleC.blue);
                Serial.println();
                break;
            }
        }
        if (!found_active_event) {
            for (int i = 0; i < MAX_EVENTS; i++) {
                if (!strobeEvents[i].active) {
                    strobeEvents[i].active = true;
                    strobeEvents[i].start_led = led_start;
                    strobeEvents[i].end_led = led_end;
                    strobeEvents[i].color = strobe_color;
                    strobeEvents[i].last_update_chunk = effective_start_chunk;
                    Serial.print("[STROBE] Activated: Chunk "); Serial.print(effective_start_chunk);
                    Serial.print(", LEDs "); Serial.print(led_start); Serial.print(" to "); Serial.print(led_end);
                   // Serial.print(", Color: "); strobe_color.printTo(Serial); Serial.println();
                    break;
                }
            }
        }
      }

      // COMET: per-chunk update or schedule
      else if (strcmp(effect, "COMET") == 0) {
  JsonArray rgbArr = doc["rgb"].as<JsonArray>();
  CRGB cometC = CRGB((uint8_t)rgbArr[0], (uint8_t)rgbArr[1], (uint8_t)rgbArr[2]);
  uint8_t count = doc["comet_count"] | 1;
  uint8_t size = doc["comet_size"] | 1;
  uint32_t effective_start_chunk = current_chunk;
  
  bool found_active_event = false;
  for (int i = 0; i < MAX_EVENTS; i++) {
    if (cometEvents[i].active && cometEvents[i].start_led == led_start && cometEvents[i].end_led == led_end) {
      // Update existing event for this chunk
      cometEvents[i].comet_color = cometC;
      cometEvents[i].comet_count = count;
      cometEvents[i].comet_size = size;
      cometEvents[i].start_chunk = effective_start_chunk;
      cometEvents[i].end_chunk = effective_start_chunk; // Active only for this chunk
      found_active_event = true;
      break;
    }
  }
  if (!found_active_event) {
    // Find a free slot and initialize
    for (int i = 0; i < MAX_EVENTS; i++) {
      if (!cometEvents[i].active) {
        cometEvents[i].active = true;
        cometEvents[i].start_chunk = effective_start_chunk;
        cometEvents[i].end_chunk = effective_start_chunk;
        cometEvents[i].start_led = led_start;
        cometEvents[i].end_led = led_end;
        cometEvents[i].comet_color = cometC;
        cometEvents[i].comet_count = count;
        cometEvents[i].comet_size = size;
        break;
      }
    }
  }
}

      // FIRE: per-chunk update or schedule
     // FIRE: per-chunk update or schedule
      else if (strcmp(effect, "FIRE") == 0) {
        JsonArray rgbArr = doc["rgb"].as<JsonArray>();
        CRGB baseC = CRGB((uint8_t)rgbArr[0], (uint8_t)rgbArr[1], (uint8_t)rgbArr[2]);
        // float flickerIntensity = p["flicker_intensity"] | 0.5f; // Default flicker intensity

        bool found_active_event = false;
        for (int i = 0; i < MAX_EVENTS; i++) {
          if (fireEvents[i].active && fireEvents[i].start_led == led_start && fireEvents[i].end_led == led_end) {
            fireEvents[i].base_color = baseC;
            // fireEvents[i].flicker_intensity = flickerIntensity;
            found_active_event = true;
            Serial.print("[FIRE] Updated: Chunk "); Serial.print(effective_start_chunk);
            Serial.print(", LEDs "); Serial.print(led_start); Serial.print(" to "); Serial.print(led_end);
            Serial.print(", Base Color: "); Serial.print(baseC.red); Serial.print(","); Serial.print(baseC.green); Serial.print(","); Serial.print(baseC.blue);
            // Serial.print(", Flicker Intensity: "); Serial.println(flickerIntensity);
            break;
          }
        }
        if (!found_active_event) {
          uint32_t dur = doc["duration_chunks"] | 1; // Get duration only when scheduling new
          for (int i = 0; i < MAX_EVENTS; i++) {
            if (!fireEvents[i].active) {
              fireEvents[i].active = true;
              // fireEvents[i].start_chunk = effective_start_chunk;
              // fireEvents[i].end_chunk = effective_start_chunk + dur - 1;
              fireEvents[i].start_led = led_start;
              fireEvents[i].end_led = led_end;
              fireEvents[i].base_color = baseC;
              // fireEvents[i].flicker_intensity = flickerIntensity;
              // Serial.print("[FIRE] Scheduled: Chunk "); Serial.print(fireEvents[i].start_chunk);
              // Serial.print(" to "); Serial.print(fireEvents[i].end_chunk);
              Serial.print(", LEDs "); Serial.print(fireEvents[i].start_led); Serial.print(" to "); Serial.print(fireEvents[i].end_led);
              Serial.print(", Base Color: "); Serial.print(baseC.red); Serial.print(","); Serial.print(baseC.green); Serial.print(","); Serial.print(baseC.blue);
              Serial.print(", Flicker Intensity: "); Serial.println(fireEvents[i].flicker_intensity);
              break;
            }
          }
        }
      }

      // else: unknown effect ‐ ignore
      break;
    }

    default:
      break;
  }
}


// Helper for random color generation (e.g., for sparkle, subtle variations)
CRGB getRandomColor(CRGB baseColor) {
  return CRGB(baseColor.r * (0.8 + random(0, 40) / 200.0),
              baseColor.g * (0.8 + random(0, 40) / 200.0),
              baseColor.b * (0.8 + random(0, 40) / 200.0));
}

// Function to generate a random float between 0.0 and 1.0 (for flicker/density)
float randomFloat() {
  return static_cast<float>(random(0, 1000)) / 1000.0f;
}

// Helper for Fire effect (simplified, applying flicker to a base color)
// A more advanced fire effect would typically use a heat array and FastLED's built-in functions.
CRGB getFlickerColor(CRGB baseColor, float intensity) {
  float flickerFactor = 1.0 - (intensity * randomFloat()); // Invert intensity: higher intensity means more dimming/flicker
  return CRGB(baseColor.r * flickerFactor, baseColor.g * flickerFactor, baseColor.b * flickerFactor);
}
// Initialize WebSocket (attach callback, connect, etc.)
void initWebSocket() {
  webSocket.begin(WS_SERVER_ADDR, WS_SERVER_PORT, WS_PATH);
  webSocket.onEvent(onWebSocketEvent);
  webSocket.setReconnectInterval(4000);  // try reconnect every 5 s
  isWSEventAttached = true;
}

// ——————————————————————————————————————————————————————————
// === SETUP & LOOP ===

void setup() {
  Serial.begin(115200);
  FastLED.addLeds<CHIPSET, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);

  randomSeed(analogRead(0));  // Seed the random number generator

   // --- ADD THIS TEST PATTERN ---
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Red; // Set all LEDs to red
  }
  FastLED.show();
  Serial.println("Test pattern: All LEDs should be RED.");
  delay(5000); // Keep them on for 5 seconds

   WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi‐Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi‐Fi connected, IP = " + WiFi.localIP().toString());
  initWebSocket();

  // Clear all events
  for (int i = 0; i < MAX_EVENTS; i++) {
    staticEvents[i].active   = false;
    blinkEvents[i].active    = false;
    rainbowEvents[i].active  = false;
    chaseEvents[i].active    = false;
    fftEvent[i].active = false;
    sparkleEvents[i].active = false;
    strobeEvents[i].active = false;
    cometEvents[i].active = false;
    fireEvents[i].active = false;
  }

  // Initialize chunk timer
  last_chunk_millis = millis();
  current_chunk = 0;
}

void loop() {

  Serial.println("Looping..."); // <-- ADD THIS
  // 1) Maintain WebSocket
  if (isWSEventAttached) {
    webSocket.loop();
  }
  Serial.println("Looping after websocket.loop()"); // <-- ADD THIS
if (isRegistered && millis() - lastHeartbeat > 2000) {
  webSocket.sendTXT("HEARTBEAT");
  lastHeartbeat = millis();
}

  // 2) Step the “global chunk counter” every CHUNK_MS milliseconds
  uint32_t now = millis();
  if (now - last_chunk_millis >= CHUNK_MS) {
    last_chunk_millis += CHUNK_MS;
    current_chunk++;
  }

  // 3) Zero out the LED buffer (we’ll re‐paint 
  //    all active effects each frame in ascending order)
        for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Black;
  }

// 4) Apply FFTEvent (if active) ‒ this overrides whatever was there:
for (int i = 0; i < MAX_EVENTS; i++) {
  if (!fftEvent[i].active) continue;

  // Deactivate FFT display if we haven't received a new update in time
  if (millis() - lastFftTimestamp > FFT_TIMEOUT_MS) {
    fftEvent[i].active = false;
    continue;
  }

  uint16_t start = fftEvent[i].start_led;
  uint16_t end = fftEvent[i].end_led;

  // Apply the latest FFT color to the assigned LED range
  for (uint16_t ledIdx = start; ledIdx <= end && ledIdx < NUM_LEDS; ledIdx++) {
    leds[ledIdx] = fftEvent[i].color;
  }
}


  // 5) Apply STATIC events
  for (int i = 0; i < MAX_EVENTS; i++) {
    if (!staticEvents[i].active) continue;

    // --- ADD THESE LINES TO CONFIRM ACTIVATION STATE AND CHUNK CHECK ---
    Serial.print("[STATIC DEBUG] Event "); Serial.print(i); Serial.print(" active. current_chunk="); Serial.print(current_chunk);
    Serial.print(", start_chunk="); Serial.print(staticEvents[i].start_chunk);
    Serial.print(", end_chunk="); Serial.println(staticEvents[i].end_chunk);

    if (current_chunk < staticEvents[i].start_chunk) continue;
    if (current_chunk > staticEvents[i].end_chunk) {
      Serial.println("[STATIC] Deactivated event.");
      staticEvents[i].active = false;
      continue;
    }

    // --- ADD THESE LINES ---
    Serial.print("[STATIC] Applying to LEDs ");
    Serial.print(staticEvents[i].start_led);
    Serial.print(" to ");
    Serial.print(staticEvents[i].end_led);
    Serial.print(" at chunk ");
    Serial.println(current_chunk);
    // -----------------------

    // Paint the entire range with this color
    for (int j = staticEvents[i].start_led; j <= staticEvents[i].end_led; j++) {
      if (j < NUM_LEDS) leds[j] = staticEvents[i].color;
    }
  }

  // 6) Apply BLINK events
  for (int i = 0; i < MAX_EVENTS; i++) {
    if (!blinkEvents[i].active) continue;
    if (current_chunk < blinkEvents[i].start_chunk) continue;
    if (current_chunk > blinkEvents[i].end_chunk) {
      blinkEvents[i].active = false;
      continue;
    }
    // Simple fixed‐rate blink: toggle every base_rate chunks
    uint32_t localChunk = current_chunk - blinkEvents[i].start_chunk;
    uint16_t period = blinkEvents[i].base_rate;  
    bool isOn = ((localChunk / period) % 2) == 0;
    CRGB col = isOn ? blinkEvents[i].on_color : blinkEvents[i].off_color;
    for (int j = blinkEvents[i].start_led; j <= blinkEvents[i].end_led; j++) {
      if (j < NUM_LEDS) leds[j] = col;
    }
  }

  // 7) Apply RAINBOW events
  for (int i = 0; i < MAX_EVENTS; i++) {
    if (!rainbowEvents[i].active) continue;
    if (current_chunk < rainbowEvents[i].start_chunk) continue;
    if (current_chunk > rainbowEvents[i].end_chunk) {
      rainbowEvents[i].active = false;
      continue;
    }
    // Update hue_offset:
    rainbowEvents[i].hue_offset += rainbowEvents[i].hue_speed;
    if (rainbowEvents[i].hue_offset >= 1.0f) 
      rainbowEvents[i].hue_offset -= 1.0f;

    uint16_t sLED = rainbowEvents[i].start_led;
    uint16_t eLED = rainbowEvents[i].end_led;
    uint16_t nLEDs = eLED - sLED + 1;
    for (uint16_t idx = 0; idx < nLEDs; idx++) {
      float hue = rainbowEvents[i].hue_offset + (float)idx / (float)nLEDs;
      if (hue >= 1.0f) hue -= 1.0f;
      CRGB c = CHSV((uint8_t)(hue * 255), 255, 255);
      if ((sLED + idx) < NUM_LEDS) leds[sLED + idx] = c;
    }
  }

  // 8) Apply CHASE events
  for (int i = 0; i < MAX_EVENTS; i++) {
    if (!chaseEvents[i].active) continue;
    if (current_chunk < chaseEvents[i].start_chunk) continue;
    if (current_chunk > chaseEvents[i].end_chunk) {
      chaseEvents[i].active = false;
      continue;
    }
    // Compute speed: for simplicity, we linearly interpolate speed between base_speed and max_speed
    // based on how far we are into the event’s duration. (You can substitute a “local RMS” if you like.)
    uint32_t elapsed   = current_chunk - chaseEvents[i].start_chunk;
    uint32_t totalDur  = chaseEvents[i].end_chunk - chaseEvents[i].start_chunk + 1;
    float    frac      = (float)elapsed / (float)totalDur;  // 0.0..1.0
    uint16_t spdChunks = chaseEvents[i].base_speed - (uint16_t)((chaseEvents[i].base_speed - chaseEvents[i].max_speed) * frac);
    if (spdChunks < 1) spdChunks = 1;

    // Advance head if enough chunks have passed:
    if ((current_chunk - chaseEvents[i].last_move_chunk) >= spdChunks) {
      chaseEvents[i].last_move_chunk = current_chunk;
      if (chaseEvents[i].direction) {
        chaseEvents[i].head_pos++;
        uint16_t width = (chaseEvents[i].end_led - chaseEvents[i].start_led + 1);
        if (chaseEvents[i].head_pos >= width) chaseEvents[i].head_pos = 0;
      } else {
        uint16_t width = (chaseEvents[i].end_led - chaseEvents[i].start_led + 1);
        if (chaseEvents[i].head_pos == 0) chaseEvents[i].head_pos = width - 1;
        else chaseEvents[i].head_pos--;
      }
    }

    // Now paint “tail_length” behind head_pos
    uint16_t width = (chaseEvents[i].end_led - chaseEvents[i].start_led + 1);
    for (uint16_t t = 0; t < chaseEvents[i].tail_length; t++) {
      // Compute which LED is part of the tail
      int16_t pos;
      if (chaseEvents[i].direction) {
        // FORWARD: tail goes “backwards” in index
        int16_t raw = (int16_t)chaseEvents[i].head_pos - t;
        if (raw < 0) raw += width;
        pos = raw;
      } else {
        // BACKWARD: tail goes “forwards” in index
        int16_t raw = (int16_t)chaseEvents[i].head_pos + t;
        if (raw >= width) raw -= width;
        pos = raw;
      }
      uint16_t ledIdx = chaseEvents[i].start_led + pos;
      if (ledIdx < NUM_LEDS) {
        // Fade tail from full color → off
        float fadeFactor = ( (float)(chaseEvents[i].tail_length - t) / (float)chaseEvents[i].tail_length );
        CRGB base = chaseEvents[i].chase_color;
        CRGB c( (uint8_t)(base.r * fadeFactor),
                (uint8_t)(base.g * fadeFactor),
                (uint8_t)(base.b * fadeFactor) );
        leds[ledIdx] = c;
      }
    }
  }

   // Sparkle
   for (int i = 0; i < MAX_EVENTS; i++) {
  if (!sparkleEvents[i].active) continue;

  int32_t elapsed = current_chunk - sparkleEvents[i].start_chunk;
  if (elapsed < 0) continue;

  if (elapsed >= sparkleEvents[i].duration) {
    sparkleEvents[i].active = false;
    continue;
  }

  float fade = 1.0 - (float(elapsed) / sparkleEvents[i].duration);
  fade = constrain(fade, 0.1, 1.0);  // prevent full dimming

  CRGB faded = sparkleEvents[i].base_color;
  faded.nscale8(fade * 255);

  for (int j = 0; j < sparkleEvents[i].count; j++) {
    int ledIndex = sparkleEvents[i].leds[j];
    if (ledIndex >= 0 && ledIndex < NUM_LEDS) {
      leds[ledIndex] += faded;
    }
  }
}


    // STROBE rendering
    for (int i = 0; i < MAX_EVENTS; i++) {
      if (strobeEvents[i].active) {
        // Check for timeout if no new messages are received
        if (current_chunk - strobeEvents[i].last_update_chunk > 2) { // Deactivate if no update for 2 chunks
            strobeEvents[i].active = false;
            continue;
        }
        for (uint16_t led = strobeEvents[i].start_led; led <= strobeEvents[i].end_led; led++) {
          leds[led] = strobeEvents[i].color; // Apply the color sent from Python
        }
      }
    }

    // COMET rendering
for (int i = 0; i < MAX_EVENTS; i++) {
  if (cometEvents[i].active && current_chunk >= cometEvents[i].start_chunk && current_chunk <= cometEvents[i].end_chunk) {
    uint16_t start = cometEvents[i].start_led;
    uint16_t end = cometEvents[i].end_led;
    uint8_t count = cometEvents[i].comet_count;
    uint8_t size = cometEvents[i].comet_size;
    CRGB color = cometEvents[i].comet_color;
    
    // Generate comets
    for (uint8_t c = 0; c < count; c++) {
      // Random start position ensuring the comet fits
      int16_t pos = random(start, end - size + 1);
      for (int s = 0; s < size; s++) {
        int ledIdx = pos + s;
        if (ledIdx >= start && ledIdx <= end && ledIdx < NUM_LEDS) {
          leds[ledIdx] = color;
        }
      }
    }
  } else if (cometEvents[i].active && current_chunk > cometEvents[i].end_chunk) {
    cometEvents[i].active = false; // Deactivate after chunk
  }
}

    // FIRE rendering
    for (int i = 0; i < MAX_EVENTS; i++) {
      if (fireEvents[i].active) {
        for (uint16_t led = fireEvents[i].start_led; led <= fireEvents[i].end_led; led++) {
          leds[led] = getFlickerColor(fireEvents[i].base_color, fireEvents[i].flicker_intensity);
        }
        fireEvents[i].active = false;
      }
    } // end if (current_chunk != last_chunk_millis / CHUNK_MS)


  // 9) Finally, push the LED buffer to the strip
  Serial.print("Chunk: "); // <-- ADD THIS
  Serial.print(current_chunk); // <-- ADD THIS
  Serial.print(", Millis: "); // <-- ADD THIS
  Serial.println(millis()); // <-- ADD THIS
  FastLED.show();
}

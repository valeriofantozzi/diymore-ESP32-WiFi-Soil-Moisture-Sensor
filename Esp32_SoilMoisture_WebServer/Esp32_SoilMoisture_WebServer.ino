//ESP32 things
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h> // Required for Home Assistant API communication
#include "esp_sleep.h" // Deep sleep functionality for power management
#include "esp_system.h" // Reset reason detection for manual reset OTA trigger

// Sampling configuration constants
const unsigned long SAMPLING_INTERVAL_MS = 60 * 1000; // 60 seconds between sample collections
const int SAMPLES_COUNT = 10; // Number of samples to collect for each measurement
const unsigned long SAMPLE_DELAY_MS = 100; // Delay between individual samples (1 second total sampling time)

// Deep sleep configuration constants - power management
const uint64_t DEEP_SLEEP_DURATION_US = 1 * 60 * 1000000ULL; // 60 seconds sleep duration in microseconds
const uint32_t OTA_WINDOW_DURATION_MS = 5 * 60 * 1000; // 5 minutes (300 seconds) to wait for OTA after manual reset

// OTA mode state tracking - persists across function calls
bool otaMode = false;
unsigned long otaStartTime = 0;

// // Soil moisture sensor calibration constants - SRP principle
// const int SOIL_SENSOR_WET_VALUE = 1700;   // ADC reading in water (100% moisture)
// const int SOIL_SENSOR_DRY_VALUE = 3400;   // ADC reading in dry air (0% moisture)

// Network configuration
const char* ssid = "Vodafone-C02290188";
const char* password = "AG6sT3CybtssgE6M";

// Static IP configuration - adjust these values for your network
IPAddress local_IP(192, 168, 1, 170);    // Desired static IP address
IPAddress gateway(192, 168, 1, 1);       // Router's IP address
IPAddress subnet(255, 255, 255, 0);      // Subnet mask
IPAddress primaryDNS(8, 8, 8, 8);        // Primary DNS server (optional)
IPAddress secondaryDNS(8, 8, 4, 4);      // Secondary DNS server (optional)

// Home Assistant configuration - Infrastructure layer
const char* HA_URL = "http://192.168.1.155:8123/api/states/";
const char* HA_TOKEN = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiI4YzhjMTUwMjUxN2U0MjE3YWViYTQ0OGUwODg4N2ZhZCIsImlhdCI6MTc0NTc1OTIxNywiZXhwIjoyMDYxMTE5MjE3fQ.IP2RKF5Wptl8Aqxub7p4htpQav-XtOIWzjN_zOmkzpk";

// Home Assistant entity IDs - Domain entities
const char* ENTITY_SOIL = "sensor.esp32_soil_moisture";
const char* ENTITY_TEMP = "sensor.esp32_temperature";
const char* ENTITY_HUM = "sensor.esp32_humidity";

const int led = LED_BUILTIN;

//DHT11 things
#include "DHT.h"
#define DHTPIN 22
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// Sensor data storage for median calculation
float soilMoistSamples[SAMPLES_COUNT];
float tempSamples[SAMPLES_COUNT];
float humSamples[SAMPLES_COUNT];

// Current median values
float medianSoilMoist = 0;
float medianTemp = 0;
float medianHum = 0;

// Timing control
unsigned long lastSamplingTime = 0;

// Median calculation function
float calculateMedian(float samples[], int count) {
  // Create a copy for sorting to avoid modifying original array
  float sortedSamples[SAMPLES_COUNT];
  for (int i = 0; i < count; i++) {
    sortedSamples[i] = samples[i];
  }
  
  // Simple bubble sort for small arrays
  for (int i = 0; i < count - 1; i++) {
    for (int j = 0; j < count - i - 1; j++) {
      if (sortedSamples[j] > sortedSamples[j + 1]) {
        float temp = sortedSamples[j];
        sortedSamples[j] = sortedSamples[j + 1];
        sortedSamples[j + 1] = temp;
      }
    }
  }
  
  // Return median value
  if (count % 2 == 0) {
    return (sortedSamples[count/2 - 1] + sortedSamples[count/2]) / 2.0;
  } else {
    return sortedSamples[count/2];
  }
}

// // Convert raw ADC to moisture percentage - single responsibility
// int convertSoilMoistureToPercent(float rawValue) {
//   // Invert scale because higher ADC values indicate drier soil
//   return map(constrain(rawValue, SOIL_SENSOR_WET_VALUE, SOIL_SENSOR_DRY_VALUE), 
//              SOIL_SENSOR_WET_VALUE, SOIL_SENSOR_DRY_VALUE, 100, 0);
// }

// Print wake-up reason for debugging - single responsibility for diagnostics
void printWakeupReason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  Serial.print("Wake-up reason: ");
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("External signal using RTC_IO");
      break;
    case ESP_SLEEP_WAKEUP_EXT1:
      Serial.println("External signal using RTC controller");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Timer");
      break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      Serial.println("Touchpad");
      break;
    case ESP_SLEEP_WAKEUP_ULP:
      Serial.println("ULP program");
      break;
    default:
      Serial.printf("Other/First boot: %d\n", wakeup_reason);
      break;
  }
}

// Check if manual reset triggered OTA mode - handles EN pin resets vs deep sleep wake-ups
bool isManualResetOtaMode() {
  esp_reset_reason_t reset_reason = esp_reset_reason();
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  Serial.print("Reset reason: ");
  switch(reset_reason) {
    case ESP_RST_POWERON:
      Serial.println("Power-on reset (EN pin or first boot)");
      // Distinguish between manual reset via EN pin vs deep sleep wake-up
      if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
        // First boot or manual reset via EN pin - trigger OTA mode
        Serial.println("Manual reset detected via EN pin - entering OTA mode");
        return true;
      } else {
        // Wake-up from deep sleep - normal operation
        Serial.println("Wake-up from deep sleep - normal operation");
        return false;
      }
    case ESP_RST_EXT:
      Serial.println("External reset (RST pin)");
      return true; // Manual reset via RST pin also triggers OTA mode
    case ESP_RST_SW:
      Serial.println("Software reset");
      return false;
    case ESP_RST_PANIC:
      Serial.println("Software reset due to exception/panic");
      return false;
    case ESP_RST_INT_WDT:
      Serial.println("Reset due to interrupt watchdog");
      return false;
    case ESP_RST_TASK_WDT:
      Serial.println("Reset due to task watchdog");
      return false;
    case ESP_RST_WDT:
      Serial.println("Reset due to other watchdogs");
      return false;
    case ESP_RST_DEEPSLEEP:
      Serial.println("Reset after exiting deep sleep mode");
      return false; // Deep sleep wake-up - normal operation
    case ESP_RST_BROWNOUT:
      Serial.println("Brownout reset");
      return false;
    case ESP_RST_SDIO:
      Serial.println("Reset over SDIO");
      return false;
    default:
      Serial.printf("Unknown reset reason: %d\n", reset_reason);
      return false;
  }
}

// Prepare system for deep sleep - single responsibility for power management
void prepareDeepSleep() {
  Serial.println("Preparing for deep sleep...");
  
  // Disconnect WiFi to save power during sleep
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  
  // Configure timer to wake up after specified duration
  esp_sleep_enable_timer_wakeup(DEEP_SLEEP_DURATION_US);
  
  Serial.printf("Going to sleep for %llu seconds...\n", DEEP_SLEEP_DURATION_US / 1000000ULL);
  
  // Visual indicator before entering sleep
  pulseLED(1, 300);
  
  // Ensure LED is off before sleep to save power
  digitalWrite(led, HIGH); // HIGH = OFF for inverted logic
  
  // Enter deep sleep - execution stops here until wake-up
  esp_deep_sleep_start();
}

// Connect to WiFi with timeout - single responsibility for network connection
bool connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi with static IP...");

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && 
         millis() - startAttemptTime < 20000) {
    digitalWrite(led, !digitalRead(led)); // Toggle LED to show activity
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(ssid);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    pulseLED(1, 1000); // Visual confirmation of connection
    return true;
  } else {
    Serial.println("\nFailed to connect to WiFi");
    pulseLED(5, 200); // Visual indication of failure
    return false;
  }
}

// Helper function to get device class for Home Assistant entities - SRP principle
String getDeviceClass(const String& unit) {
  // Device classes help Home Assistant display sensors properly
  if (unit == "°C") return "temperature";
  if (unit == "%") return "humidity"; // Used for humidity percentage
  if (unit == "ADC") return "moisture"; // Raw ADC values for soil moisture sensors
  return ""; // Default - no device class
}

// Function to send sensor value to Home Assistant - follows SRP principle
bool sendToHomeAssistant(const char* entityId, float value, const char* unitOfMeasurement, const char* friendlyName) {
  // Create HTTP client for API communication
  HTTPClient http;
  
  // Build the full URL for the entity
  String url = String(HA_URL) + entityId;
  
  // Begin HTTP connection
  http.begin(url);
  
  // Set authorization header and content type
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(HA_TOKEN));
  
  // Create JSON payload according to Home Assistant state format
  String payload = "{";
  payload += "\"state\":\"" + String(value) + "\",";
  payload += "\"attributes\":{";
  payload += "\"unit_of_measurement\":\"" + String(unitOfMeasurement) + "\",";
  payload += "\"friendly_name\":\"" + String(friendlyName) + "\",";
  payload += "\"device_class\":\"" + getDeviceClass(unitOfMeasurement) + "\",";
  payload += "\"state_class\":\"measurement\"";
  payload += "}";
  payload += "}";
  
  // Send POST request to update entity state
  int httpCode = http.POST(payload);
  
  // Check for successful response
  bool success = (httpCode > 0 && (httpCode == 200 || httpCode == 201));
  
  if (success) {
    Serial.printf("Updated Home Assistant entity %s: %.1f %s (HTTP %d)\n", 
                 entityId, value, unitOfMeasurement, httpCode);
  } else {
    Serial.printf("Failed to update Home Assistant entity %s (HTTP %d): %s\n", 
                 entityId, httpCode, http.errorToString(httpCode).c_str());
  }
  
  // Close connection
  http.end();
  
  return success;
}

// Function to update all sensor values in Home Assistant - follows SRP principle
void updateHomeAssistant() {
  Serial.println("Sending sensor data to Home Assistant...");
  
  // Send raw ADC value for soil moisture (not percentage)
  // This allows Home Assistant to handle raw sensor data and apply its own calibration
  
  // Update all three sensors
  bool soilSuccess = sendToHomeAssistant(ENTITY_SOIL, medianSoilMoist, "ADC", "ESP32 Soil Moisture");
  bool tempSuccess = sendToHomeAssistant(ENTITY_TEMP, medianTemp, "°C", "ESP32 Temperature");
  bool humSuccess = sendToHomeAssistant(ENTITY_HUM, medianHum, "%", "ESP32 Humidity");
  
  // Log overall status
  if (soilSuccess && tempSuccess && humSuccess) {
    Serial.println("All sensor data successfully sent to Home Assistant");
      pulseLED(1, 500); // Blink once to indicate data sent

  } else {
    Serial.println("Some sensors failed to update in Home Assistant");
      pulseLED(5, 250); // Blink five times to indicate failure

  }

}

// Check WiFi connection and attempt to reconnect if necessary - follows SRP principle
bool ensureWiFiConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    return true; // Already connected
  }
  
  Serial.println("WiFi connection lost, attempting to reconnect...");
  
  // Try to reconnect
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  // Wait for connection with timeout
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nReconnected to WiFi");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\nFailed to reconnect to WiFi");
    return false;
  }
}

// Collect samples and calculate medians
void collectSensorSamples() {
  Serial.println("Collecting sensor samples...");
  
  // Collect samples with small delays between them
  for (int i = 0; i < SAMPLES_COUNT; i++) {
    soilMoistSamples[i] = analogRead(32);
    tempSamples[i] = dht.readTemperature();
    humSamples[i] = dht.readHumidity();
    
    // Skip invalid DHT readings but keep soil moisture
    if (isnan(tempSamples[i])) tempSamples[i] = medianTemp; // Use last valid value
    if (isnan(humSamples[i])) humSamples[i] = medianHum;   // Use last valid value
    
    delay(SAMPLE_DELAY_MS);
  }
  
  // Calculate medians
  medianSoilMoist = calculateMedian(soilMoistSamples, SAMPLES_COUNT);
  medianTemp = calculateMedian(tempSamples, SAMPLES_COUNT);
  medianHum = calculateMedian(humSamples, SAMPLES_COUNT);
  
  Serial.printf("Median values - Soil: %.1f, Temp: %.1f°C, Hum: %.1f%%\n", 
                medianSoilMoist, medianTemp, medianHum);
  
  // Send the new median values to Home Assistant (handles its own LED indication)
  if (ensureWiFiConnection()) {
    updateHomeAssistant();
  } else {
    Serial.println("WiFi not connected - cannot update Home Assistant");
  }
}

void setup(void) {
  pinMode(led, OUTPUT);
  digitalWrite(led, 0);
  Serial.begin(115200);
  
  // Short delay to allow stabilization when powering up
  delay(1000);
    // Print wake-up reason for debugging power management
  printWakeupReason();
    // Check if manual reset triggered OTA mode
  otaMode = isManualResetOtaMode();
  
  // If OTA mode is triggered, record start time for timeout
  if (otaMode) {
    otaStartTime = millis();
  }
  
  // Visual indicator that initialization has started
  pulseLED(2, 200); // 2 quick blinks
  
  // Configure static IP address before connecting to WiFi
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("Failed to configure static IP");
    // Visual indicator for IP config failure - 3 short blinks
    pulseLED(3, 200);
  }
  
  // Connect to WiFi using the dedicated function
  bool wifiConnected = connectToWiFi();
  
  // Initialize DHT sensor
  dht.begin();
  delay(1000); // Allow sensor to stabilize
    if (otaMode) {
    Serial.println("OTA mode active - staying awake for firmware updates");
    Serial.println("Sensor data collection is disabled during OTA window to avoid interference");
    pulseLED(3, 500); // 3 longer pulses to indicate OTA mode
    
    // Initialize OTA if WiFi is connected
    if (wifiConnected) {
      initializeOTA();
    }
  } else {
    Serial.println("Normal operation mode - will enter deep sleep after data collection");
    
    // Collect sensor data and send to Home Assistant
    collectSensorSamples();
    
    // Enter deep sleep to save power
    prepareDeepSleep();
    
    // Code after this point will not execute until next wake-up
  }
}

// Helper function for diagnostic LED pulsing with fade effect - SRP principle
void pulseLED(int times, int delayMs) {
  const int FADE_STEPS = 50; // Number of steps for smooth fade transition
  const int MAX_BRIGHTNESS = 255; // Maximum PWM value for LED brightness
  
  for (int i = 0; i < times; i++) {
    // Fade in: gradually increase brightness (decrease PWM value for inverted LED)
    for (int brightness = 0; brightness <= MAX_BRIGHTNESS; brightness += (MAX_BRIGHTNESS / FADE_STEPS)) {
      analogWrite(led, MAX_BRIGHTNESS - brightness); // Invert for LOW=ON logic
      delay(delayMs / (2 * FADE_STEPS)); // Split delayMs between fade in and fade out
    }
    
    // Fade out: gradually decrease brightness (increase PWM value for inverted LED)
    for (int brightness = MAX_BRIGHTNESS; brightness >= 0; brightness -= (MAX_BRIGHTNESS / FADE_STEPS)) {
      analogWrite(led, MAX_BRIGHTNESS - brightness); // Invert for LOW=ON logic
      delay(delayMs / (2 * FADE_STEPS)); // Split delayMs between fade in and fade out
    }
    
    // Brief pause between pulses (except for the last one)
    if (i < times - 1) {
      delay(delayMs / 4); // Quarter of the original delay as pause between pulses
    }
  }
  
  // Ensure LED is off after pulsing sequence (HIGH = OFF for inverted logic)
  analogWrite(led, 255);
}

// OTA configuration constants - DRY principle
const char* OTA_PASSWORD = "fanculo";  // Change this to your secure password

// OTA update initialization - single responsibility for firmware updates
void initializeOTA() {
  // Configure OTA hostname for network identification
  ArduinoOTA.setHostname("ESP32-SoilSensor");
  
  // Set OTA password for security - prevents unauthorized updates
  ArduinoOTA.setPassword(OTA_PASSWORD);
  
  // OTA callbacks for status feedback and LED indication
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("Start updating " + type);
    
    // LED on during OTA to indicate update in progress
    digitalWrite(led, HIGH);
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA Update completed successfully");
    
    // Blink LED rapidly to indicate successful completion
    for (int i = 0; i < 10; i++) {
      digitalWrite(led, !digitalRead(led));
      delay(100);
    }
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    // Toggle LED during progress to show activity
    static unsigned long lastToggle = 0;
    if (millis() - lastToggle > 500) {
      digitalWrite(led, !digitalRead(led));
      lastToggle = millis();
    }
    
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    
    // Different blink patterns for different errors
    digitalWrite(led, LOW);
    int blinkPattern = 3; // Default 3 blinks for unknown error
    
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
      blinkPattern = 1;
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
      blinkPattern = 2;
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
      blinkPattern = 3;
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
      blinkPattern = 4;
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
      blinkPattern = 5;
    }
    
    // Visual error indication
    pulseLED(blinkPattern, 300);
  });
  
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void loop(void) {
  if (otaMode) {
    // Check if OTA window has expired (1 minute timeout)
    if (millis() - otaStartTime > OTA_WINDOW_DURATION_MS) {
      Serial.println("OTA window expired - switching to normal operation");
      otaMode = false; // Disable OTA mode
      
      // Collect sensor data and enter deep sleep (avoid data collection during OTA window)
      collectSensorSamples();
      prepareDeepSleep();
    }
    
    // In OTA mode, handle OTA updates
    ArduinoOTA.handle();
    
    // Continuous non-blocking LED pulsing during OTA window for visual feedback
    static unsigned long lastPulseUpdate = 0;
    const int PULSE_PERIOD_MS = 2000; // 2 second complete pulse cycle (1s fade in + 1s fade out)
    const int PULSE_UPDATE_INTERVAL_MS = 20; // Update LED every 20ms for smooth animation
    
    unsigned long currentTime = millis();
    
    // Update LED brightness every 20ms for smooth animation
    if (currentTime - lastPulseUpdate >= PULSE_UPDATE_INTERVAL_MS) {
      lastPulseUpdate = currentTime;
      
      // Calculate position in pulse cycle (0-PULSE_PERIOD_MS)
      int cyclePosition = (currentTime - otaStartTime) % PULSE_PERIOD_MS;
      int brightness;
      
      if (cyclePosition < PULSE_PERIOD_MS / 2) {
        // First half: fade in (0 to 255)
        brightness = map(cyclePosition, 0, PULSE_PERIOD_MS / 2 - 1, 0, 255);
      } else {
        // Second half: fade out (255 to 0)
        brightness = map(cyclePosition - PULSE_PERIOD_MS / 2, 0, PULSE_PERIOD_MS / 2 - 1, 255, 0);
      }
      
      // Apply brightness with inverted logic (LOW=ON for built-in LED)
      analogWrite(led, 255 - brightness);
    }
    
    // Print status every 10 seconds for user feedback
    static unsigned long lastStatusPrint = 0;
    if (currentTime - lastStatusPrint > 10000) {
      uint32_t remainingMs = OTA_WINDOW_DURATION_MS - (currentTime - otaStartTime);
      Serial.printf("OTA mode active - %lu seconds remaining\n", remainingMs / 1000);
      lastStatusPrint = currentTime;
    }
  } else {
    // We should not reach here in normal operation due to deep sleep
    // If we do, go to sleep as a fallback safety measure
    Serial.println("Unexpected wake state - going back to sleep");
    prepareDeepSleep();
  }
}

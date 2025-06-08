//ESP32 things
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h> // Required for Home Assistant API communication

// Sampling configuration constants
const unsigned long SAMPLING_INTERVAL_MS = 60000; // 1 minute between sample collections
const int SAMPLES_COUNT = 10; // Number of samples to collect for each measurement
const unsigned long SAMPLE_DELAY_MS = 100; // Delay between individual samples (1 second total sampling time)

// Soil moisture sensor calibration constants - SRP principle
const int SOIL_SENSOR_WET_VALUE = 1700;   // ADC reading in water (100% moisture)
const int SOIL_SENSOR_DRY_VALUE = 3400;   // ADC reading in dry air (0% moisture)

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

// Convert raw ADC to moisture percentage - single responsibility
int convertSoilMoistureToPercent(float rawValue) {
  // Invert scale because higher ADC values indicate drier soil
  return map(constrain(rawValue, SOIL_SENSOR_WET_VALUE, SOIL_SENSOR_DRY_VALUE), 
             SOIL_SENSOR_WET_VALUE, SOIL_SENSOR_DRY_VALUE, 100, 0);
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
      blinkLED(1, 500); // Blink once to indicate data sent

  } else {
    Serial.println("Some sensors failed to update in Home Assistant");
      blinkLED(5, 250); // Blink five times to indicate failure

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
  
  // Visual indicator that initialization has started
  blinkLED(2, 200); // 2 quick blinks
  
  // Configure static IP address before connecting to WiFi
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("Failed to configure static IP");
    // Visual indicator for IP config failure - 3 short blinks
    blinkLED(3, 200);
  }
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("");
  Serial.println("Connecting to WiFi with static IP...");

  // Wait for connection with timeout
  unsigned long startAttemptTime = millis();
  // Try for 20 seconds to connect to WiFi
  while (WiFi.status() != WL_CONNECTED && 
         millis() - startAttemptTime < 20000) {
    digitalWrite(led, !digitalRead(led)); // Toggle LED to show activity
    delay(500);
    Serial.print(".");
  }
  
  // Check if connected after timeout
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Failed to connect to WiFi");
    // Visual indicator for WiFi failure - 5 quick blinks
    blinkLED(5, 200);
    // Continue anyway - device will work with limited functionality
  } else {
    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(ssid);
    Serial.print("Static IP address: ");
    Serial.println(WiFi.localIP());
    
    // Visual indicator for successful connection - 1 long blink
    blinkLED(1, 1000);
    
    // Initialize OTA after successful WiFi connection
    initializeOTA();
  }
  // Continue with OTA initialization if WiFi is connected
  if (WiFi.status() == WL_CONNECTED) {
    // Initialize OTA after successful WiFi connection
    initializeOTA();
  }

  dht.begin();
  
  delay(2000);
}

// Helper function for diagnostic LED blinking
void blinkLED(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(led, LOW);
    delay(delayMs);
    digitalWrite(led, HIGH);
    if (i < times - 1) {
      delay(delayMs); // Add delay between blinks
    }
  }
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
    blinkLED(blinkPattern, 300);
  });
  
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void loop(void) {
  // Check if it's time to collect new samples
  unsigned long currentTime = millis();
  if (currentTime - lastSamplingTime >= SAMPLING_INTERVAL_MS) {
    collectSensorSamples();
    lastSamplingTime = currentTime;
  }
  
  ArduinoOTA.handle();
}

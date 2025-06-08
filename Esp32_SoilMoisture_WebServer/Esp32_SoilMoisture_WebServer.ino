//ESP32 things
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>

// Sampling configuration constants
const unsigned long SAMPLING_INTERVAL_MS = 60000; // 1 minute between sample collections for 24-hour history
const int SAMPLES_COUNT = 10; // Number of samples to collect for each measurement
const unsigned long SAMPLE_DELAY_MS = 100; // Delay between individual samples (1 second total sampling time)

const int HISTORY_SIZE = 24 * 60; // 24-hour history with 1 sample per minute = 1440 samples total

// Soil moisture sensor calibration constants - SRP principle
const int SOIL_SENSOR_WET_VALUE = 1700;   // ADC reading in water (100% moisture)
const int SOIL_SENSOR_DRY_VALUE = 3400;   // ADC reading in dry air (0% moisture)

// Network configuration
const char* ssid = "Vodafone-C02290188";
const char* password = "AG6sT3CybtssgE6M";
int port=80;

// Static IP configuration - adjust these values for your network
IPAddress local_IP(192, 168, 1, 170);    // Desired static IP address
IPAddress gateway(192, 168, 1, 1);       // Router's IP address
IPAddress subnet(255, 255, 255, 0);      // Subnet mask
IPAddress primaryDNS(8, 8, 8, 8);        // Primary DNS server (optional)
IPAddress secondaryDNS(8, 8, 4, 4);      // Secondary DNS server (optional)

WebServer server(port);

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

// Historical data storage (FIFO buffers)
float soilMoistHistory[HISTORY_SIZE];
float tempHistory[HISTORY_SIZE];
float humHistory[HISTORY_SIZE];
unsigned long timestampHistory[HISTORY_SIZE]; // Store timestamp for each measurement

// FIFO control variables
int historyIndex = 0; // Current position in circular buffer
int historyCount = 0; // Number of valid entries (max HISTORY_SIZE)

// Current median values (exposed to web interface)
float medianSoilMoist = 0;
float medianTemp = 0;
float medianHum = 0;

// Timing control
unsigned long lastSamplingTime = 0;
unsigned long lastRefreshTime = 0; // Track when data was last refreshed

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

// Collect samples and calculate medians
void collectSensorSamples() {
  // Turn on LED to indicate data collection in progress
  digitalWrite(led, HIGH);
  
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
  
  // Add calculated medians to historical FIFO buffers
  addToHistory(medianSoilMoist, medianTemp, medianHum);
  
  // Update refresh timestamp
  lastRefreshTime = millis();
  
  // Turn off LED when data collection is complete
  digitalWrite(led, LOW);
  
  Serial.printf("Median values - Soil: %.1f, Temp: %.1f°C, Hum: %.1f%%\n", 
                medianSoilMoist, medianTemp, medianHum);
  Serial.printf("Historical data count: %d/%d\n", historyCount, HISTORY_SIZE);
}

// Helper function to format uptime into readable string
String formatUptime(unsigned long uptimeMs) {
  unsigned long seconds = uptimeMs / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  unsigned long days = hours / 24;
  
  seconds %= 60;
  minutes %= 60;
  hours %= 24;
  
  String result = "";
  if (days > 0) result += String(days) + "d ";
  if (hours > 0) result += String(hours) + "h ";
  if (minutes > 0) result += String(minutes) + "m ";
  result += String(seconds) + "s";
  
  return result;
}

// Add new median values to FIFO history buffers
void addToHistory(float soilMoist, float temp, float hum) {
  // Store values in circular buffer at current index
  soilMoistHistory[historyIndex] = soilMoist;
  tempHistory[historyIndex] = temp;
  humHistory[historyIndex] = hum;
  timestampHistory[historyIndex] = millis();
  
  // Move to next position in circular buffer
  historyIndex = (historyIndex + 1) % HISTORY_SIZE;
  
  // Track number of valid entries (up to HISTORY_SIZE)
  if (historyCount < HISTORY_SIZE) {
    historyCount++;
  }
  
  Serial.printf("Added to history (index %d, count %d): Soil %.1f, Temp %.1f°C, Hum %.1f%%\n", 
                historyIndex, historyCount, soilMoist, temp, hum);
}

// Get historical value by index (0 = most recent, 1 = previous, etc.)
float getHistoricalSoilMoisture(int backIndex) {
  if (backIndex >= historyCount) return -1; // Invalid index
  int actualIndex = (historyIndex - 1 - backIndex + HISTORY_SIZE) % HISTORY_SIZE;
  return soilMoistHistory[actualIndex];
}

float getHistoricalTemperature(int backIndex) {
  if (backIndex >= historyCount) return -1; // Invalid index
  int actualIndex = (historyIndex - 1 - backIndex + HISTORY_SIZE) % HISTORY_SIZE;
  return tempHistory[actualIndex];
}

float getHistoricalHumidity(int backIndex) {
  if (backIndex >= historyCount) return -1; // Invalid index
  int actualIndex = (historyIndex - 1 - backIndex + HISTORY_SIZE) % HISTORY_SIZE;
  return humHistory[actualIndex];
}

// Get total number of historical entries available
int getHistoryCount() {
  return historyCount;
}

// Get timestamp of historical entry by index
unsigned long getHistoricalTimestamp(int backIndex) {
  if (backIndex >= historyCount) return 0; // Invalid index
  int actualIndex = (historyIndex - 1 - backIndex + HISTORY_SIZE) % HISTORY_SIZE;
  return timestampHistory[actualIndex];
}

// Generate SVG line chart for soil moisture history - SRP principle applied
String generateSoilMoistureChart() {
  const int CHART_POINTS = historyCount; // Show all available points
  const int CHART_WIDTH = 300;
  const int CHART_HEIGHT = 80;
  const int MARGIN = 10;
  
  if (CHART_POINTS < 2) {
    return "<div style='text-align: center; padding: 10px; color: #999;'>Not enough data for chart</div>";
  }
  
  String svg = "<svg width='" + String(CHART_WIDTH) + "' height='" + String(CHART_HEIGHT) + "' style='margin: 10px auto; display: block;'>";
  
  // Calculate min/max for scaling - use soil moisture percentage range
  float minVal = 100, maxVal = 0;
  for (int i = 0; i < CHART_POINTS; i++) {
    float rawSoil = getHistoricalSoilMoisture(CHART_POINTS - 1 - i);
    if (rawSoil > 0) {
      float soilPercent = convertSoilMoistureToPercent(rawSoil);
      minVal = min(minVal, soilPercent);
      maxVal = max(maxVal, soilPercent);
    }
  }
  
  // Ensure some range exists
  if (maxVal - minVal < 5) {
    float center = (maxVal + minVal) / 2;
    minVal = center - 2.5;
    maxVal = center + 2.5;
  }
  
  // Generate line path - connects data points
  String pathData = "M ";
  for (int i = 0; i < CHART_POINTS; i++) {
    float rawSoil = getHistoricalSoilMoisture(CHART_POINTS - 1 - i);
    if (rawSoil > 0) {
      float soilPercent = convertSoilMoistureToPercent(rawSoil);
      
      int x = MARGIN + (i * (CHART_WIDTH - 2 * MARGIN)) / (CHART_POINTS - 1);
      int y = CHART_HEIGHT - MARGIN - ((soilPercent - minVal) * (CHART_HEIGHT - 2 * MARGIN)) / (maxVal - minVal);
      
      pathData += String(x) + "," + String(y);
      if (i < CHART_POINTS - 1) pathData += " L ";
    }
  }
  
  // Add grid lines and chart elements
  svg += "<defs><linearGradient id='soilGrad' x1='0%' y1='0%' x2='0%' y2='100%'>";
  svg += "<stop offset='0%' style='stop-color:#2ecc71;stop-opacity:0.3'/>";
  svg += "<stop offset='100%' style='stop-color:#2ecc71;stop-opacity:0.1'/></linearGradient></defs>";
  
  // Background area under line
  svg += "<path d='" + pathData + " L " + String(CHART_WIDTH - MARGIN) + "," + String(CHART_HEIGHT - MARGIN);
  svg += " L " + String(MARGIN) + "," + String(CHART_HEIGHT - MARGIN) + " Z' fill='url(#soilGrad)'/>";
  
  // Main line
  svg += "<path d='" + pathData + "' stroke='#27ae60' stroke-width='2' fill='none'/>";
  
  // Data points as circles
  for (int i = 0; i < CHART_POINTS; i++) {
    float rawSoil = getHistoricalSoilMoisture(CHART_POINTS - 1 - i);
    if (rawSoil > 0) {
      float soilPercent = convertSoilMoistureToPercent(rawSoil);
      
      int x = MARGIN + (i * (CHART_WIDTH - 2 * MARGIN)) / (CHART_POINTS - 1);
      int y = CHART_HEIGHT - MARGIN - ((soilPercent - minVal) * (CHART_HEIGHT - 2 * MARGIN)) / (maxVal - minVal);
      
      svg += "<circle cx='" + String(x) + "' cy='" + String(y) + "' r='3' fill='#27ae60' stroke='white' stroke-width='1'/>";
    }
  }
  
  svg += "</svg>";
  return svg;
}

// Generate SVG line chart for temperature history - SRP principle applied
String generateTemperatureChart() {
  const int CHART_POINTS = historyCount; // Show all available points
  const int CHART_WIDTH = 300;
  const int CHART_HEIGHT = 80;
  const int MARGIN = 10;
  
  if (CHART_POINTS < 2) {
    return "<div style='text-align: center; padding: 10px; color: #999;'>Not enough data for chart</div>";
  }
  
  String svg = "<svg width='" + String(CHART_WIDTH) + "' height='" + String(CHART_HEIGHT) + "' style='margin: 10px auto; display: block;'>";
  
  // Calculate temperature range for scaling
  float minVal = 999, maxVal = -999;
  for (int i = 0; i < CHART_POINTS; i++) {
    float temp = getHistoricalTemperature(CHART_POINTS - 1 - i);
    if (temp > 0) {
      minVal = min(minVal, temp);
      maxVal = max(maxVal, temp);
    }
  }
  
  // Ensure reasonable range
  if (maxVal - minVal < 2) {
    float center = (maxVal + minVal) / 2;
    minVal = center - 1;
    maxVal = center + 1;
  }
  
  String pathData = "M ";
  for (int i = 0; i < CHART_POINTS; i++) {
    float temp = getHistoricalTemperature(CHART_POINTS - 1 - i);
    if (temp > 0) {
      int x = MARGIN + (i * (CHART_WIDTH - 2 * MARGIN)) / (CHART_POINTS - 1);
      int y = CHART_HEIGHT - MARGIN - ((temp - minVal) * (CHART_HEIGHT - 2 * MARGIN)) / (maxVal - minVal);
      
      pathData += String(x) + "," + String(y);
      if (i < CHART_POINTS - 1) pathData += " L ";
    }
  }
  
  svg += "<defs><linearGradient id='tempGrad' x1='0%' y1='0%' x2='0%' y2='100%'>";
  svg += "<stop offset='0%' style='stop-color:#e74c3c;stop-opacity:0.3'/>";
  svg += "<stop offset='100%' style='stop-color:#e74c3c;stop-opacity:0.1'/></linearGradient></defs>";
  
  svg += "<path d='" + pathData + " L " + String(CHART_WIDTH - MARGIN) + "," + String(CHART_HEIGHT - MARGIN);
  svg += " L " + String(MARGIN) + "," + String(CHART_HEIGHT - MARGIN) + " Z' fill='url(#tempGrad)'/>";
  
  svg += "<path d='" + pathData + "' stroke='#c0392b' stroke-width='2' fill='none'/>";
  
  // Data points
  for (int i = 0; i < CHART_POINTS; i++) {
    float temp = getHistoricalTemperature(CHART_POINTS - 1 - i);
    if (temp > 0) {
      int x = MARGIN + (i * (CHART_WIDTH - 2 * MARGIN)) / (CHART_POINTS - 1);
      int y = CHART_HEIGHT - MARGIN - ((temp - minVal) * (CHART_HEIGHT - 2 * MARGIN)) / (maxVal - minVal);
      
      svg += "<circle cx='" + String(x) + "' cy='" + String(y) + "' r='3' fill='#c0392b' stroke='white' stroke-width='1'/>";
    }
  }
  
  svg += "</svg>";
  return svg;
}

// Generate SVG line chart for humidity history - SRP principle applied  
String generateHumidityChart() {
  const int CHART_POINTS = historyCount; // Show all available points
  const int CHART_WIDTH = 300;
  const int CHART_HEIGHT = 80;
  const int MARGIN = 10;
  
  if (CHART_POINTS < 2) {
    return "<div style='text-align: center; padding: 10px; color: #999;'>Not enough data for chart</div>";
  }
  
  String svg = "<svg width='" + String(CHART_WIDTH) + "' height='" + String(CHART_HEIGHT) + "' style='margin: 10px auto; display: block;'>";
  
  // Humidity typically ranges 0-100%, use this for scaling
  float minVal = 999, maxVal = -999;
  for (int i = 0; i < CHART_POINTS; i++) {
    float hum = getHistoricalHumidity(CHART_POINTS - 1 - i);
    if (hum > 0) {
      minVal = min(minVal, hum);
      maxVal = max(maxVal, hum);
    }
  }
  
  if (maxVal - minVal < 5) {
    float center = (maxVal + minVal) / 2;
    minVal = center - 2.5;
    maxVal = center + 2.5;
  }
  
  String pathData = "M ";
  for (int i = 0; i < CHART_POINTS; i++) {
    float hum = getHistoricalHumidity(CHART_POINTS - 1 - i);
    if (hum > 0) {
      int x = MARGIN + (i * (CHART_WIDTH - 2 * MARGIN)) / (CHART_POINTS - 1);
      int y = CHART_HEIGHT - MARGIN - ((hum - minVal) * (CHART_HEIGHT - 2 * MARGIN)) / (maxVal - minVal);
      
      pathData += String(x) + "," + String(y);
      if (i < CHART_POINTS - 1) pathData += " L ";
    }
  }
  
  svg += "<defs><linearGradient id='humGrad' x1='0%' y1='0%' x2='0%' y2='100%'>";
  svg += "<stop offset='0%' style='stop-color:#3498db;stop-opacity:0.3'/>";
  svg += "<stop offset='100%' style='stop-color:#3498db;stop-opacity:0.1'/></linearGradient></defs>";
  
  svg += "<path d='" + pathData + " L " + String(CHART_WIDTH - MARGIN) + "," + String(CHART_HEIGHT - MARGIN);
  svg += " L " + String(MARGIN) + "," + String(CHART_HEIGHT - MARGIN) + " Z' fill='url(#humGrad)'/>";
  
  svg += "<path d='" + pathData + "' stroke='#2980b9' stroke-width='2' fill='none'/>";
  
  // Data points
  for (int i = 0; i < CHART_POINTS; i++) {
    float hum = getHistoricalHumidity(CHART_POINTS - 1 - i);
    if (hum > 0) {
      int x = MARGIN + (i * (CHART_WIDTH - 2 * MARGIN)) / (CHART_POINTS - 1);
      int y = CHART_HEIGHT - MARGIN - ((hum - minVal) * (CHART_HEIGHT - 2 * MARGIN)) / (maxVal - minVal);
      
      svg += "<circle cx='" + String(x) + "' cy='" + String(y) + "' r='3' fill='#2980b9' stroke='white' stroke-width='1'/>";
    }
  }
  
  svg += "</svg>";
  return svg;
}

void handleRoot() {
  String webtext;
  
  // Use median values instead of single readings
  float temp = medianTemp;
  float hum = medianHum;
  float soilMoist = medianSoilMoist;
  
  // Convert soil moisture using dedicated function - SRP principle
  int soilMoistPercent = convertSoilMoistureToPercent(soilMoist);
  
  // Calculate time since last refresh
  unsigned long timeSinceRefresh = millis() - lastRefreshTime;
  String refreshInfo = "Last refresh: " + formatUptime(timeSinceRefresh) + " ago (Device uptime: " + formatUptime(millis()) + ")";
  
  webtext="<html>\
  <head>\
    <meta http-equiv='refresh' content='10'/>\
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>\
    <title>ESP32 Soil Moisture Monitor</title>\
    <style>\
      body { background-color: #f5f5f5; font-family: Arial, sans-serif; margin: 0; padding: 20px; color: #333; }\
      h1 { color: #0066cc; text-align: center; margin-bottom: 30px; }\
      .container { max-width: 800px; margin: 0 auto; }\
      .info-text { font-size: 0.9em; background-color: #e9f7fe; padding: 15px; border-radius: 8px; }\
      .dashboard { display: flex; flex-wrap: wrap; justify-content: space-between; margin-top: 20px; }\
      .widget { background: white; border-radius: 10px; padding: 20px; margin: 10px 0; box-shadow: 0 2px 5px rgba(0,0,0,0.1); flex-basis: 100%; }\
      .widget-title { font-size: 1.2em; margin-bottom: 20px; color: #555; }\
      .bar-container { position: relative; height: 50px; margin: 30px 0; }\
      .bar-background { position: absolute; height: 20px; width: 100%; border-radius: 10px; overflow: hidden; }\
      .bar-segment { position: absolute; height: 20px; }\
      .bar-indicator { position: absolute; width: 12px; height: 12px; background-color: #000; border-radius: 50%; top: 4px; margin-left: -6px; }\
      .bar-value { position: absolute; transform: translateX(-50%); top: -25px; font-weight: bold; }\
      .bar-labels { display: flex; justify-content: space-between; margin-top: 5px; font-size: 0.8em; color: #777; }\
      .timestamp { text-align: center; margin-top: 30px; font-style: italic; color: #888; font-size: 0.85em; }\
    </style>\
  </head>\
  <body>\
    <div class='container'>\
      <h1>ESP32 Soil Monitor Dashboard</h1>\
      <div class='dashboard'>\
        <div class='widget'>\
          <div class='widget-title'>Soil Moisture</div>\
          <div class='bar-container'>\
            <div class='bar-background'>\
              <div class='bar-segment' style='left: 0%; width: 30%; background-color: #e74c3c;'></div>\
              <div class='bar-segment' style='left: 30%; width: 40%; background-color: #2ecc71;'></div>\
              <div class='bar-segment' style='left: 70%; width: 30%; background-color: #3498db;'></div>\
            </div>\
            <div class='bar-indicator' style='left: " + String(soilMoistPercent) + "%;'></div>\
            <div class='bar-value' style='left: " + String(soilMoistPercent) + "%;'>" + String(soilMoistPercent) + "%</div>\
          </div>\
          <div class='bar-labels'>\
            <span>Dry</span>\
            <span>Optimal</span>\
            <span>Wet</span>\
          </div>\
          <div style='text-align: center; font-size: 0.8em; margin-top: 10px;'>Raw: " + String(soilMoist, 1) + "</div>\
          <div style='border-top: 1px solid #eee; margin-top: 15px; padding-top: 15px;'>\
            <div style='font-size: 0.9em; color: #666; margin-bottom: 5px;'>History (All " + String(historyCount) + " readings)</div>\
            " + generateSoilMoistureChart() + "\
          </div>\
        </div>\
        <div class='widget'>\
          <div class='widget-title'>Temperature</div>\
          <div class='bar-container'>\
            <div class='bar-background'>\
              <div class='bar-segment' style='left: 0%; width: 37.5%; background-color: #3498db;'></div>\
              <div class='bar-segment' style='left: 37.5%; width: 37.5%; background-color: #2ecc71;'></div>\
              <div class='bar-segment' style='left: 75%; width: 25%; background-color: #e74c3c;'></div>\
            </div>\
            <div class='bar-indicator' style='left: " + String(map(constrain(temp, 0, 40), 0, 40, 0, 100)) + "%;'></div>\
            <div class='bar-value' style='left: " + String(map(constrain(temp, 0, 40), 0, 40, 0, 100)) + "%;'>" + String(temp, 1) + "&deg;C</div>\
          </div>\
          <div class='bar-labels'>\
            <span>Cold (0&deg;C)</span>\
            <span>Optimal (15&deg;C)</span>\
            <span>Hot (30&deg;C)</span>\
            <span>40&deg;C</span>\
          </div>\
          <div style='border-top: 1px solid #eee; margin-top: 15px; padding-top: 15px;'>\
            <div style='font-size: 0.9em; color: #666; margin-bottom: 5px;'>History (All " + String(historyCount) + " readings)</div>\
            " + generateTemperatureChart() + "\
          </div>\
        </div>\
        <div class='widget'>\
          <div class='widget-title'>Humidity</div>\
          <div class='bar-container'>\
            <div class='bar-background'>\
              <div class='bar-segment' style='left: 0%; width: 30%; background-color: #e74c3c;'></div>\
              <div class='bar-segment' style='left: 30%; width: 40%; background-color: #2ecc71;'></div>\
              <div class='bar-segment' style='left: 70%; width: 30%; background-color: #3498db;'></div>\
            </div>\
            <div class='bar-indicator' style='left: " + String(hum) + "%;'></div>\
            <div class='bar-value' style='left: " + String(hum) + "%;'>" + String(hum, 0) + "%</div>\
          </div>\
          <div class='bar-labels'>\
            <span>Dry (0%)</span>\
            <span>Comfortable (30%)</span>\
            <span>Humid (70%)</span>\
            <span>100%</span>\
          </div>\
          <div style='border-top: 1px solid #eee; margin-top: 15px; padding-top: 15px;'>\
            <div style='font-size: 0.9em; color: #666; margin-bottom: 5px;'>History (All " + String(historyCount) + " readings)</div>\
            " + generateHumidityChart() + "\
          </div>\
        </div>\
      </div>\
      <div class='timestamp'>\
        " + refreshInfo + "\
      </div>\
    </div>\
  </body>\
</html>";
  server.send(200, "text/html", webtext);
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
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

  // Continue with MDNS and server setup regardless of WiFi status
  if (MDNS.begin("esp32")) {
    Serial.println("MDNS responder started");
  }

  server.on("/", handleRoot);

  server.on("/inline", []() {
    server.send(200, "text/plain", "this works as well");
  });

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");
  dht.begin();
  
  // Initialize FIFO history buffers
  Serial.println("Initializing historical data buffers...");
  // No need to zero arrays as global arrays are automatically initialized to 0
  Serial.printf("FIFO buffers ready: %d slots for historical data\n", HISTORY_SIZE);
  
  delay(2000);
}

// Helper function for diagnostic LED blinking
void blinkLED(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(led, HIGH);
    delay(delayMs);
    digitalWrite(led, LOW);
    delay(delayMs);
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
  
  server.handleClient(); 
  ArduinoOTA.handle();
}

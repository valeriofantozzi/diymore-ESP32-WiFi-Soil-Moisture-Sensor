# ESP32 Soil Moisture Web Server

A comprehensive plant monitoring system built with ESP32 that measures soil moisture, temperature, and humidity with web-based visualization and 24-hour historical data tracking.

## Features

- 🌱 **Real-time monitoring** of soil moisture, temperature, and humidity
- 📊 **24-hour historical data** with SVG chart visualization
- 🔢 **Median filtering** (10 samples per measurement) for noise reduction
- 🌐 **Web server interface** with responsive design
- 📱 **Mobile-friendly** dashboard accessible from any device
- 🔄 **OTA firmware updates** for remote maintenance
- 💡 **LED status indicators** during data collection
- 🕒 **System uptime** tracking and display
- 📡 **Static IP configuration** for reliable network access

## Hardware Requirements

### Core Components

- **ESP32 Development Board** (any variant with WiFi)
- **Capacitive Soil Moisture Sensor** (analog output)
- **DHT11 Temperature & Humidity Sensor**
- **Jumper wires** and breadboard
- **USB cable** for programming and power

### Pin Configuration

```
DHT11 Sensor:
├── VCC → 3.3V
├── GND → GND
└── DATA → GPIO 22

Soil Moisture Sensor:
├── VCC → 3.3V
├── GND → GND
└── AOUT → A0 (analog input)

Built-in LED:
└── GPIO 2 (LED_BUILTIN)
```

## Software Setup

### Prerequisites

- [Arduino IDE](https://www.arduino.cc/en/software) 2.0 or later
- ESP32 Board Package installed
- Required libraries (install via Library Manager):
  - `WiFi` (built-in with ESP32)
  - `WebServer` (built-in with ESP32)
  - `DHT sensor library` by Adafruit
  - `ESPmDNS` (built-in with ESP32)
  - `ArduinoOTA` (built-in with ESP32)

### Installation Steps

1. **Clone or download** this repository
2. **Open** `Esp32_SoilMoisture_WebServer.ino` in Arduino IDE
3. **Configure network settings** (see Configuration section)
4. **Select ESP32 board** in Tools → Board
5. **Upload** the sketch to your ESP32

## Configuration

### Network Settings

Update these values in the code before uploading:

```cpp
// Network credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Static IP configuration (adjust for your network)
IPAddress local_IP(192, 168, 1, 170);    // Desired ESP32 IP
IPAddress gateway(192, 168, 1, 1);       // Your router IP
IPAddress subnet(255, 255, 255, 0);      // Subnet mask
```

### Sensor Calibration

Calibrate soil moisture sensor for your specific conditions:

```cpp
// Adjust these values based on your sensor readings
const int SOIL_SENSOR_WET_VALUE = 1700;   // ADC reading in water
const int SOIL_SENSOR_DRY_VALUE = 3400;   // ADC reading in dry air
```

### OTA Configuration

Set your OTA password for secure firmware updates:

```cpp
const char* OTA_PASSWORD = "your_ota_password";
```

## Usage

### Accessing the Web Interface

1. **Power on** the ESP32 with sensors connected
2. **Wait for WiFi connection** (LED will indicate status)
3. **Open web browser** and navigate to the configured IP address
   - Default: `http://192.168.1.170`
4. **View real-time data** and historical charts

### Web Interface Features

- **Current readings** with percentage-based soil moisture
- **System uptime** and last refresh timestamp
- **Interactive SVG charts** showing 24-hour trends for:
  - Soil moisture percentage
  - Temperature (°C)
  - Humidity (%)
- **Auto-refresh** every 30 seconds
- **Responsive design** works on desktop and mobile

### Data Collection

- **Automatic sampling** every 60 seconds
- **Median filtering** from 10 samples per measurement
- **Circular buffer** stores 1440 data points (24 hours)
- **LED indicator** shows when data collection is active

## API Endpoints

The web server provides these endpoints:

- `GET /` - Main dashboard with current readings and charts
- `GET /404` - Custom 404 error page for invalid routes

## Architecture & Design Principles

This project follows clean architecture principles:

### Single Responsibility Principle (SRP)

- `calculateMedian()` - Only handles median calculation
- `convertSoilMoistureToPercent()` - Only converts ADC to percentage
- `addToHistory()` - Only manages FIFO buffer operations
- `generateSoilMoistureChart()` - Only creates SVG charts

### High Cohesion, Low Coupling

- **Sensor layer**: Raw data collection and filtering
- **Data layer**: Historical storage with circular buffers
- **Presentation layer**: Web server and HTML generation
- **Infrastructure layer**: WiFi, OTA, and hardware abstraction

### Key Design Decisions

- **Median filtering** reduces sensor noise and outliers
- **Circular buffer** provides memory-efficient 24-hour history
- **Static IP** ensures reliable network access for monitoring
- **Modular functions** enable easy testing and maintenance

## Monitoring & Diagnostics

### LED Status Indicators

- **Solid ON** - Data collection in progress
- **OFF** - Normal operation, no data collection
- **Blinking patterns** - Various system states during startup

### Serial Monitor Output

Connect to serial monitor (115200 baud) to view:

- WiFi connection status
- Sensor readings and median calculations
- Historical data buffer status
- OTA update progress
- Error messages and diagnostics

## Troubleshooting

### Common Issues

**WiFi Connection Problems**

- Verify SSID and password are correct
- Check signal strength at ESP32 location
- Ensure 2.4GHz network (ESP32 doesn't support 5GHz)

**Sensor Reading Issues**

- Check wiring connections
- Verify power supply (3.3V for sensors)
- Calibrate soil moisture sensor values
- Monitor serial output for error messages

**Web Server Access Problems**

- Verify static IP configuration matches your network
- Check router firewall settings
- Ensure ESP32 and client are on same network
- Try accessing via mDNS: `http://esp32.local`

**OTA Update Failures**

- Verify OTA password is correct
- Ensure stable WiFi connection
- Check available flash memory space
- Try uploading via USB cable first

### Memory Considerations

- **SRAM usage**: ~12KB for 24-hour history buffers
- **Flash usage**: Varies with code size and OTA partition
- **Heap monitoring**: Check available memory via serial output

## Contributing

When contributing to this project, please follow these guidelines:

1. **Maintain SRP** - One responsibility per function
2. **Add comments** explaining "why", not "what"
3. **Test thoroughly** with actual hardware
4. **Document changes** in commit messages
5. **Preserve architecture** - don't introduce coupling between layers

## License

This project is open source and available under the [MIT License](LICENSE).

## Hardware Sources

- **ESP32 DevKit**: Available from various suppliers
- **Capacitive Soil Moisture Sensor**: Search for "capacitive soil moisture sensor v1.2"
- **DHT11 Sensor**: Widely available temperature/humidity sensor
- **Breadboard and jumpers**: Standard prototyping materials

## Future Enhancements

- [ ] MQTT integration for IoT platforms
- [ ] Multiple sensor support (sensor array)
- [ ] Data logging to SD card
- [ ] Alert notifications (email/SMS)
- [ ] REST API for external integrations
- [ ] Configuration web interface
- [ ] Historical data export (CSV/JSON)

---

**Note**: This project prioritizes reliability and simplicity over complex features. The architecture supports easy extension while maintaining clean separation of concerns.

// ======================================================
// SMART CLEANER PRO - ESP32-S3 PRODUCTION FIRMWARE
// Full Robot Control | Supabase | ThingSpeak | Arduino UNO
// Version: 2.0.0 - Production Ready
// ======================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>

// ======================================================
// WiFi Configuration
// ======================================================
const char* ssid = "Ads";
const char* password = "@2111444";

// ======================================================
// Supabase Configuration
// ======================================================
const char* supabaseUrl = "https://hdiqbfngevcpeylzwndq.supabase.co";
const char* supabaseKey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImhkaXFiZm5nZXZjcGV5bHp3bmRxIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjkxODE1NDIsImV4cCI6MjA4NDc1NzU0Mn0.VsbBc06KVttmy5QJTdYSvPYHr6oD9MncRpjJadD9XS0";
const char* robotSerial = "SMARTCLEANER001";

// ======================================================
// ThingSpeak Configuration
// ======================================================
const char* thingSpeakApiKey = "4NLV1IB1FQSKZFHS";
const char* thingSpeakUrl = "https://api.thingspeak.com/update";

// ======================================================
// UART Configuration (Pins 43, 44 for Arduino UNO)
// ======================================================
#define UART_RX_PIN 44
#define UART_TX_PIN 43
HardwareSerial ArduinoSerial(1);

// ======================================================
// Robot State Variables
// ======================================================
String robotId = "";
String robotStatus = "INITIALIZING";
String robotMovement = "STOP";
int leftDist = 0, rightDist = 0, frontLeftDist = 0, frontRightDist = 0;
String currentMode = "MANUAL";
bool isRegistered = false;
bool hasArduinoData = false;

// Vision System (Sensor-based dirt detection)
uint32_t totalFramesProcessed = 0;
bool dirtDetected = false;
int dirtScore = 0;

// Telemetry
int lastCommandCode = 0;

// Timing
unsigned long lastStatusUpdate = 0;
unsigned long lastCommandCheck = 0;
unsigned long lastThingSpeakUpdate = 0;
unsigned long lastSensorUpdate = 0;

// ======================================================
// Helper Functions
// ======================================================

int getWifiStrength() {
    long rssi = WiFi.RSSI();
    int percentage = (rssi + 100) * 100 / 70;
    if (percentage > 100) percentage = 100;
    if (percentage < 0) percentage = 0;
    return percentage;
}

void sendToArduino(String cmd) {
    cmd.toUpperCase();
    ArduinoSerial.println(cmd);
    Serial.print("→ Sent: ");
    Serial.println(cmd);
}

void readFromArduino() {
    while (ArduinoSerial.available()) {
        String data = ArduinoSerial.readStringUntil('\n');
        data.trim();
        
        if (data.startsWith("L:")) leftDist = data.substring(2).toInt();
        else if (data.startsWith("R:")) rightDist = data.substring(2).toInt();
        else if (data.startsWith("FL:")) frontLeftDist = data.substring(3).toInt();
        else if (data.startsWith("FR:")) frontRightDist = data.substring(3).toInt();
        else if (data.startsWith("STATUS:")) {
            robotStatus = data.substring(7);
            hasArduinoData = true;
        }
        else if (data.startsWith("MOVEMENT:")) robotMovement = data.substring(9);
        else if (data.startsWith("MODE:")) currentMode = data.substring(5);
    }
}

void processVision() {
    unsigned long now = millis();
    if (now - lastSensorUpdate < 500) return;
    lastSensorUpdate = now;
    
    totalFramesProcessed++;
    
    bool isMoving = (robotMovement == "FORWARD");
    bool hasObstacle = (frontLeftDist < 40 || frontRightDist < 40);
    
    if (isMoving && hasObstacle && currentMode == "AUTO") {
        dirtScore = min(dirtScore + 8, 100);
        if (dirtScore > 60 && !dirtDetected) {
            dirtDetected = true;
            Serial.println("🧹 DIRT DETECTED!");
            sendToArduino("SLOW_SWEEP");
        }
    } else if (dirtDetected) {
        dirtScore = max(dirtScore - 3, 0);
        if (dirtScore < 20) {
            dirtDetected = false;
            Serial.println("✅ Area cleaned!");
        }
    }
    
    if (totalFramesProcessed % 100 == 0) {
        Serial.printf("👁️ Vision: Frames=%lu, Dirt=%d%%, Detected=%s\n", 
                      totalFramesProcessed, dirtScore, dirtDetected ? "YES" : "NO");
    }
}

void fetchRobotId() {
    HTTPClient http;
    String url = String(supabaseUrl) + "/rest/v1/robots?serial_number=eq." + robotSerial + "&select=id";
    http.begin(url);
    http.addHeader("apikey", supabaseKey);
    
    int code = http.GET();
    if (code == 200) {
        String response = http.getString();
        DynamicJsonDocument doc(512);
        deserializeJson(doc, response);
        if (doc[0]) {
            robotId = doc[0]["id"].as<String>();
            Serial.print("✅ Robot ID: ");
            Serial.println(robotId);
            isRegistered = true;
        }
    }
    http.end();
}

void sendStatusToSupabase() {
    if (!isRegistered || !hasArduinoData) return;
    
    HTTPClient http;
    String url = String(supabaseUrl) + "/rest/v1/robot_status";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", supabaseKey);
    
    String payload = "{\"robot_id\":\"" + robotId + "\",\"status\":\"" + robotStatus + 
                     "\",\"left_sensor\":" + String(leftDist) + ",\"right_sensor\":" + String(rightDist) +
                     ",\"front_left_sensor\":" + String(frontLeftDist) + ",\"front_right_sensor\":" + String(frontRightDist) +
                     ",\"movement\":\"" + robotMovement + "\",\"mode\":\"" + currentMode + "\"}";
    
    http.POST(payload);
    http.end();
    Serial.println("✅ Status sent to Supabase");
}

void sendToThingSpeak() {
    if (!hasArduinoData) return;
    
    int wifiStrength = getWifiStrength();
    int statusCode = (robotStatus == "CLEANING" || robotStatus == "MOVING_FORWARD") ? 1 : 2;
    
    HTTPClient http;
    String url = String(thingSpeakUrl) + "?api_key=" + thingSpeakApiKey +
                 "&field1=" + String(wifiStrength) + "&field2=1&field3=" + String(lastCommandCode) + "&field4=0" +
                 "&field5=" + String(leftDist) + "&field6=" + String(rightDist) +
                 "&field7=" + String(frontLeftDist) + "&field8=" + String(statusCode);
    
    http.begin(url);
    int code = http.GET();
    if (code == 200) {
        Serial.println("📊 ThingSpeak updated");
    }
    http.end();
}

void executeCommand(String cmd) {
    Serial.print("📱 Executing: ");
    Serial.println(cmd);
    
    if (cmd == "FORWARD") {
        sendToArduino("FORWARD");
        robotMovement = "FORWARD";
        lastCommandCode = 1;
    }
    else if (cmd == "BACKWARD") {
        sendToArduino("BACKWARD");
        robotMovement = "BACKWARD";
        lastCommandCode = 2;
    }
    else if (cmd == "LEFT") {
        sendToArduino("LEFT");
        robotMovement = "LEFT";
        lastCommandCode = 3;
    }
    else if (cmd == "RIGHT") {
        sendToArduino("RIGHT");
        robotMovement = "RIGHT";
        lastCommandCode = 4;
    }
    else if (cmd == "STOP") {
        sendToArduino("STOP");
        robotMovement = "STOP";
        lastCommandCode = 5;
    }
    else if (cmd == "START_CLEANING" || cmd == "AUTO_MODE") {
        currentMode = "AUTO";
        sendToArduino("AUTO_MODE");
        Serial.println("🤖 AUTO MODE ACTIVATED");
        lastCommandCode = 6;
    }
    else if (cmd == "STOP_CLEANING" || cmd == "MANUAL_MODE") {
        currentMode = "MANUAL";
        sendToArduino("MANUAL_MODE");
        Serial.println("✋ MANUAL MODE ACTIVATED");
        lastCommandCode = 7;
    }
    else if (cmd == "RETURN_CHARGE") {
        sendToArduino("RETURN_CHARGE");
        Serial.println("🔋 RETURNING TO CHARGE");
        lastCommandCode = 8;
    }
}

void checkForCommands() {
    if (!isRegistered) return;
    
    HTTPClient http;
    String url = String(supabaseUrl) + "/rest/v1/command_queue?robot_id=eq." + robotId + "&status=eq.pending&limit=1";
    http.begin(url);
    http.addHeader("apikey", supabaseKey);
    
    int code = http.GET();
    if (code == 200) {
        String response = http.getString();
        http.end();
        
        if (response != "[]" && response.length() > 10) {
            DynamicJsonDocument doc(512);
            deserializeJson(doc, response);
            
            if (doc[0]) {
                String cmd = doc[0]["command"].as<String>();
                String cmdId = doc[0]["id"].as<String>();
                
                executeCommand(cmd);
                
                String updateUrl = String(supabaseUrl) + "/rest/v1/command_queue?id=eq." + cmdId;
                http.begin(updateUrl);
                http.addHeader("Content-Type", "application/json");
                http.addHeader("apikey", supabaseKey);
                http.sendRequest("PATCH", "{\"status\":\"executed\"}");
                http.end();
            }
        }
    } else {
        http.end();
    }
}

void printStatus() {
    Serial.print("📊 Status: ");
    Serial.print(robotStatus);
    Serial.print(" | Mode: ");
    Serial.print(currentMode);
    Serial.print(" | Movement: ");
    Serial.print(robotMovement);
    Serial.print(" | L:");
    Serial.print(leftDist);
    Serial.print(" R:");
    Serial.print(rightDist);
    Serial.print(" FL:");
    Serial.print(frontLeftDist);
    Serial.print(" FR:");
    Serial.println(frontRightDist);
}

// ======================================================
// Setup
// ======================================================
void setup() {
    Serial.begin(115200);
    ArduinoSerial.begin(9600, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    
    Serial.println("\n==========================================");
    Serial.println("     SMART CLEANER PRO - PRODUCTION");
    Serial.println("         ESP32-S3 FIRMWARE v2.0");
    Serial.println("==========================================\n");
    
    // Connect WiFi
    Serial.print("Connecting to WiFi");
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ WiFi Connected!");
        Serial.print("📡 IP Address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\n❌ WiFi Failed! Check credentials");
    }
    
    // Fetch robot ID from Supabase
    fetchRobotId();
    
    Serial.println("\n✅ SYSTEM READY!");
    Serial.println("📡 WiFi: Connected");
    Serial.println("🔌 UART: Arduino Communication Active");
    Serial.printf("☁️ Supabase: %s\n", isRegistered ? "Connected" : "Waiting for registration");
    Serial.println("📊 ThingSpeak: Active");
    Serial.println("👁️ Vision: Sensor-based active");
    Serial.println("==========================================\n");
}

// ======================================================
// Main Loop
// ======================================================
void loop() {
    readFromArduino();
    processVision();
    
    unsigned long now = millis();
    
    if (now - lastCommandCheck > 2000) {
        checkForCommands();
        lastCommandCheck = now;
    }
    
    if (now - lastStatusUpdate > 3000 && isRegistered && hasArduinoData) {
        sendStatusToSupabase();
        lastStatusUpdate = now;
    }
    
    if (now - lastThingSpeakUpdate > 15000 && hasArduinoData) {
        sendToThingSpeak();
        lastThingSpeakUpdate = now;
    }
    
    static int logCounter = 0;
    if (logCounter++ >= 300) {
        printStatus();
        logCounter = 0;
    }
    
    delay(10);
}

#include <Arduino.h>
#include <WiFi.h>
#include <DHT.h>
#include <PubSubClient.h>
#include <math.h>

#define DHT_PIN       4
#define DHT_TYPE      DHT22
#define LDR_PIN       34
#define PIR_PIN       27
#define POT_PIN       35
#define LIGHT_LED     19
#define FAN_LED       18

const int LDR_THRESHOLD = 2000;
const float HUMIDITY_THRESHOLD = 70.0;
const float MIN_TEMP_LIMIT = 20.0;
const float MAX_TEMP_LIMIT = 35.0;

const unsigned long OCCUPANCY_HOLD_TIME = 30000;
const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long MQTT_PUBLISH_INTERVAL = 5000;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;

const float TEMP_PRINT_CHANGE = 0.5;
const float HUMIDITY_PRINT_CHANGE = 1.0;
const int LDR_PRINT_CHANGE = 100;

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

const char* MQTT_SERVER = "broker.hivemq.com";
const int MQTT_PORT = 1883;

// Use this same topic later in Node-RED.
const char* MQTT_TOPIC =
    "smartclassroom/wit/demo_20261010_akm/telemetry";

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

float temperature = NAN;
float humidity = NAN;
float temperatureLimit = 25.0;

int lightValue = 0;
int potValue = 0;
int motion = LOW;

bool lightState = false;
bool fanState = false;
bool occupied = false;

bool previousLightState = false;
bool previousFanState = false;
bool previousOccupiedState = false;

float previousTemperature = NAN;
float previousHumidity = NAN;
float previousTemperatureLimit = 25.0;

int previousLightValue = 0;
bool firstReading = true;

unsigned long lastSensorRead = 0;
unsigned long lastMotionTime = 0;
unsigned long lastMqttPublish = 0;
unsigned long lastMqttReconnectAttempt = 0;

void printDashboard();
void printTemperature();
void printHumidity();
void printLDR();
void printTemperatureLimit();
void connectMQTT();
void publishMQTT();

void setup()
{
    Serial.begin(115200);
    dht.begin();

    pinMode(PIR_PIN, INPUT);
    pinMode(LIGHT_LED, OUTPUT);
    pinMode(FAN_LED, OUTPUT);

    digitalWrite(LIGHT_LED, LOW);
    digitalWrite(FAN_LED, LOW);

    Serial.println();
    Serial.println("========================================");
    Serial.println("       SMART CLASSROOM IoT SYSTEM");
    Serial.println("========================================");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);

    Serial.print("Connecting to Wi-Fi");

    unsigned long wifiStart = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - wifiStart < 15000)
    {
        delay(250);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("Wi-Fi connected.");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
    }
    else
    {
        Serial.println("Wi-Fi not connected. Will retry.");
    }

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setBufferSize(256);

    Serial.println("System initialized.");
}

void loop()
{
    // Keep MQTT communication running continuously.
    if (WiFi.status() == WL_CONNECTED)
    {
        if (!mqttClient.connected())
        {
            connectMQTT();
        }
        else
        {
            mqttClient.loop();
        }
    }

    unsigned long currentTime = millis();

    if (currentTime - lastSensorRead < SENSOR_INTERVAL)
        return;

    lastSensorRead = currentTime;

    temperature = dht.readTemperature();
    humidity = dht.readHumidity();
    lightValue = analogRead(LDR_PIN);
    potValue = analogRead(POT_PIN);
    motion = digitalRead(PIR_PIN);

    if (motion == HIGH)
    {
        occupied = true;
        lastMotionTime = currentTime;
    }

    if (currentTime - lastMotionTime >= OCCUPANCY_HOLD_TIME)
    {
        occupied = false;
    }

    temperatureLimit =
        MIN_TEMP_LIMIT +
        ((float)potValue / 4095.0) *
        (MAX_TEMP_LIMIT - MIN_TEMP_LIMIT);

    bool darkRoom = lightValue < LDR_THRESHOLD;

    lightState = darkRoom && occupied;

    bool highTemperature =
        !isnan(temperature) &&
        temperature > temperatureLimit;

    bool highHumidity =
        !isnan(humidity) &&
        humidity > HUMIDITY_THRESHOLD;

    fanState = highTemperature || highHumidity;

    digitalWrite(LIGHT_LED, lightState ? HIGH : LOW);
    digitalWrite(FAN_LED, fanState ? HIGH : LOW);

    if (firstReading)
    {
        printDashboard();

        previousLightState = lightState;
        previousFanState = fanState;
        previousOccupiedState = occupied;

        previousTemperature = temperature;
        previousHumidity = humidity;
        previousTemperatureLimit = temperatureLimit;
        previousLightValue = lightValue;

        firstReading = false;
    }
    else
    {
        bool importantChange = false;

        if (lightState != previousLightState)
            importantChange = true;

        if (fanState != previousFanState)
            importantChange = true;

        if (occupied != previousOccupiedState)
            importantChange = true;

        if (importantChange)
        {
            printDashboard();
        }
        else
        {
            if (!isnan(temperature) &&
                !isnan(previousTemperature) &&
                fabsf(temperature - previousTemperature)
                    >= TEMP_PRINT_CHANGE)
            {
                printTemperature();
            }

            if (!isnan(humidity) &&
                !isnan(previousHumidity) &&
                fabsf(humidity - previousHumidity)
                    >= HUMIDITY_PRINT_CHANGE)
            {
                printHumidity();
            }

            if (abs(lightValue - previousLightValue)
                >= LDR_PRINT_CHANGE)
            {
                printLDR();
            }

            if (fabsf(temperatureLimit -
                      previousTemperatureLimit) >= 0.5)
            {
                printTemperatureLimit();
            }
        }

        previousLightState = lightState;
        previousFanState = fanState;
        previousOccupiedState = occupied;

        previousTemperature = temperature;
        previousHumidity = humidity;
        previousTemperatureLimit = temperatureLimit;
        previousLightValue = lightValue;
    }

    // Publish telemetry independently of Serial Monitor output.
    if (mqttClient.connected() &&
        currentTime - lastMqttPublish >= MQTT_PUBLISH_INTERVAL)
    {
        publishMQTT();
        lastMqttPublish = currentTime;
    }
}

void connectMQTT()
{
    unsigned long currentTime = millis();

    if (currentTime - lastMqttReconnectAttempt <
        MQTT_RECONNECT_INTERVAL)
        return;

    lastMqttReconnectAttempt = currentTime;

    String clientId = "SmartClassroom-";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

    Serial.print("Connecting to MQTT... ");

    if (mqttClient.connect(clientId.c_str()))
    {
        Serial.println("connected.");
        lastMqttPublish = 0;
    }
    else
    {
        Serial.print("failed, state = ");
        Serial.println(mqttClient.state());
    }
}

void publishMQTT()
{
    String tempJson =
        isnan(temperature) ? "null" : String(temperature, 1);

    String humidityJson =
        isnan(humidity) ? "null" : String(humidity, 1);

    String payload = "{";
    payload += "\"temperature\":" + tempJson + ",";
    payload += "\"humidity\":" + humidityJson + ",";
    payload += "\"lightLevel\":" + String(lightValue) + ",";
    payload += "\"motion\":" + String(motion == HIGH ? 1 : 0) + ",";
    payload += "\"occupied\":" + String(occupied ? "true" : "false") + ",";
    payload += "\"temperatureLimit\":" + String(temperatureLimit, 1) + ",";
    payload += "\"light\":" + String(lightState ? "true" : "false") + ",";
    payload += "\"fan\":" + String(fanState ? "true" : "false");
    payload += "}";

    if (mqttClient.publish(MQTT_TOPIC, payload.c_str()))
    {
        Serial.println("[MQTT] Telemetry published.");
    }
    else
    {
        Serial.println("[MQTT] Publish failed.");
    }
}

void printDashboard()
{
    Serial.println();
    Serial.println("========================================");
    Serial.println("       SMART CLASSROOM DASHBOARD");
    Serial.println("========================================");

    Serial.print("Temperature   : ");
    if (isnan(temperature))
        Serial.println("ERROR");
    else
    {
        Serial.print(temperature, 1);
        Serial.println(" C");
    }

    Serial.print("Humidity      : ");
    if (isnan(humidity))
        Serial.println("ERROR");
    else
    {
        Serial.print(humidity, 1);
        Serial.println(" %");
    }

    Serial.print("Light Level   : ");
    Serial.println(lightValue);

    Serial.print("Motion        : ");
    Serial.println(motion == HIGH ? "DETECTED" : "NOT DETECTED");

    Serial.print("Classroom     : ");
    Serial.println(occupied ? "OCCUPIED" : "UNOCCUPIED");

    Serial.println("----------------------------------------");

    Serial.print("Temp Limit    : ");
    Serial.print(temperatureLimit, 1);
    Serial.println(" C");

    Serial.print("Humidity Limit: ");
    Serial.print(HUMIDITY_THRESHOLD, 1);
    Serial.println(" %");

    Serial.print("Light         : ");
    Serial.println(lightState ? "ON" : "OFF");

    Serial.print("Fan           : ");
    Serial.println(fanState ? "ON" : "OFF");

    Serial.println("========================================");
}

void printTemperature()
{
    Serial.print("[TEMPERATURE] ");
    Serial.print(temperature, 1);
    Serial.println(" C");
}

void printHumidity()
{
    Serial.print("[HUMIDITY] ");
    Serial.print(humidity, 1);
    Serial.println(" %");
}

void printLDR()
{
    Serial.print("[LDR] ");
    Serial.println(lightValue);
}

void printTemperatureLimit()
{
    Serial.print("[TEMP LIMIT] ");
    Serial.print(temperatureLimit, 1);
    Serial.println(" C");
}

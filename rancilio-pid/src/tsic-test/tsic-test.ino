/********************************************************
 * Version 2.1.0 BLEEDING EDGE MASTER
 *   
 * This enhancement implementation is based on the
 * great work of the rancilio-pid (http://rancilio-pid.de/)
 * team. Hopefully it will be merged upstream soon. In case
 * of questions just contact, Tobias <medlor@web.de>
 * 
 *****************************************************/

#include "icon.h"

//Libraries for OTA
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <EEPROM.h>

#include "rancilio-pid.h"

RemoteDebug Debug;

//Define pins for outputs
#define pinRelayVentil    12
#define pinRelayPumpe     13
#define pinRelayHeater    14
#define pinLed            15

const char* sysVersion PROGMEM  = "Version 2.1.0 beta1";

/********************************************************
  definitions below must be changed in the userConfig.h file
******************************************************/
const int Display = DISPLAY;
const int OnlyPID = ONLYPID;
const int TempSensor = TEMPSENSOR;
const int TempSensorRecovery = TEMPSENSORRECOVERY;
const int brewDetection = BREWDETECTION;
const int triggerType = TRIGGERTYPE;
const boolean ota = OTA;
const int grafana=GRAFANA;

// Wifi
const char* hostname = HOSTNAME;
const char* auth = AUTH;
const char* ssid = D_SSID;
const char* pass = PASS;

unsigned long lastWifiConnectionAttempt = millis();
const unsigned long wifiReconnectInterval = 60000; // try to reconnect every 60 seconds (must be at least 4000)
unsigned long wifiConnectWaitTime = 5000; //ms to wait for the connection to succeed
unsigned int wifiReconnects = 0; //number of reconnects

// OTA
const char* OTAhost = OTAHOST;
const char* OTApass = OTAPASS;

//Blynk
const char* blynkaddress = BLYNKADDRESS;
const int blynkport = BLYNKPORT;
unsigned long blynk_lastReconnectAttemptTime = 0;
unsigned int blynk_reconnectAttempts = 0;
unsigned long blynk_reconnect_incremental_backoff = 180000 ; //Failsafe: add 180sec to reconnect time after each connect-failure.
unsigned int blynk_max_incremental_backoff = 5 ; // At most backoff <mqtt_max_incremenatl_backoff>+1 * (<mqtt_reconnect_incremental_backoff>ms)


// MQTT
//how to set general connect_timeout AND read_timeout (socket timeout)?
#include "src\PubSubClient\PubSubClient.h"
//#include <PubSubClient.h>  // uncomment this line AND delete src/PubSubClient/ folder, if you want to use system lib
const int MQTT_MAX_PUBLISH_SIZE = 120; //see https://github.com/knolleary/pubsubclient/blob/master/src/PubSubClient.cpp
const char* mqtt_server_ip = MQTT_SERVER_IP;
const int mqtt_server_port = MQTT_SERVER_PORT;
const char* mqtt_username = MQTT_USERNAME;
const char* mqtt_password = MQTT_PASSWORD;
const char* mqtt_topic_prefix = MQTT_TOPIC_PREFIX;
char topic_will[256];
char topic_set[256];
unsigned long lastMQTTStatusReportTime = 0;
unsigned long lastMQTTStatusReportInterval = 5000; //mqtt send status-report every 5 second
const boolean mqtt_flag_retained = true;
unsigned long mqtt_dontPublishUntilTime = 0;
unsigned long mqtt_dontPublishBackoffTime = 60000; // Failsafe: dont publish if there are errors for 10 seconds
unsigned long mqtt_lastReconnectAttemptTime = 0;
unsigned int mqtt_reconnectAttempts = 0;
unsigned long mqtt_reconnect_incremental_backoff = 210000 ; //Failsafe: add 210sec to reconnect time after each connect-failure.
unsigned int mqtt_max_incremental_backoff = 5 ; // At most backoff <mqtt_max_incremenatl_backoff>+1 * (<mqtt_reconnect_incremental_backoff>ms)
bool mqtt_disabled_temporary = false;

WiFiClient espClient;
PubSubClient mqtt_client(espClient);
 
/********************************************************
   Vorab-Konfig
******************************************************/
int pidON = 1 ;             // 1 = control loop in closed loop
int relayON, relayOFF;      // used for relay trigger type. Do not change!
int activeState = 3;        // 1:= Coldstart required (maschine is cold) 
                            // 2:= Stabilize temperature after coldstart
                            // 3:= (default) Inner Zone detected (temperature near setPoint)
                            // 4:= Brew detected
                            // 5:= Outer Zone detected (temperature outside of "inner zone")
boolean emergencyStop = false; // Notstop bei zu hoher Temperatur

/********************************************************
   history of temperatures
*****************************************************/
const int numReadings = 75;             // number of values per Array
double readingstemp[numReadings];       // the readings from Temp
float readingstime[numReadings];        // the readings from time
int readIndex = 0;                      // the index of the current reading
int totaltime = 0 ;                     // the running time
unsigned long  timeBrewDetection = 0 ;
int timerBrewDetection = 0 ;
int i = 0;

/********************************************************
   PID Variables
*****************************************************/
const unsigned int windowSizeSeconds = 5;            // How often should PID.compute() run? must be >= 1sec
unsigned int windowSize = windowSizeSeconds * 1000;  // 1000=100% heater power => resolution used in TSR() and PID.compute().
unsigned int isrCounter = 0; // TODO remove windowSize - 500;          // counter for ISR
double Input = 0, Output = 0;
double previousInput = 0;
double previousOutput = 0;

unsigned long previousMillistemp;  // initialisation at the end of init()
unsigned long previousMillistemp2;
const long refreshTempInterval = 1000;
int pidMode = 1;                   //1 = Automatic, 0 = Manual

double setPoint = SETPOINT;
double starttemp = STARTTEMP;      //TODO add auto-tune

// State 1: Coldstart PID values
const int coldStartStep1ActivationOffset = 5;
// ... none ...

// State 2: Coldstart stabilization PID values
// ... none ...

// State 3: Inner Zone PID values
double aggKp = AGGKP;
double aggTn = AGGTN;
double aggTv = AGGTV;
#if (aggTn == 0)
double aggKi = 0;
#else
double aggKi = aggKp / aggTn;
#endif
double aggKd = aggTv * aggKp ;

// State 4: Brew PID values
// ... none ...

// State 5: Outer Zone Pid values
double aggoKp = AGGOKP;
double aggoTn = AGGOTN;
double aggoTv = AGGOTV;
#if (aggoTn == 0)
double aggoKi = 0;
#else
double aggoKi = aggoKp / aggoTn;
#endif
double aggoKd = aggoTv * aggoKp ;
const double outerZoneTemperatureDifference = 1;

/********************************************************
   PID with Bias (steadyPower) Temperature Controller
*****************************************************/
#include "PIDBias.h"
double steadyPower = STEADYPOWER; // in percent . TODO EEPROM every 35 min on change?
double PreviousSteadyPower = 0;
int burstShot      = 0;   // this is 1, when the user wants to immediatly set the heater power to the value specified in burstPower
double burstPower  = 20;  // in percent
int testTrigger    = 0;

// If the espresso hardware itself is cold, we need additional power for steadyPower to hold the water temperature
double steadyPowerOffset   = STEADYPOWER_OFFSET;  // heater power (in percent) which should be added to steadyPower during steadyPowerOffset_Time
int steadyPowerOffset_Time = STEADYPOWER_OFFSET_TIME;  // timeframe (in ms) for which steadyPowerOffset_Activated should be active
unsigned long steadyPowerOffset_Activated = 0;

PIDBias bPID(&Input, &Output, &steadyPower, &setPoint, aggKp, aggKi, aggKd);

/********************************************************
   Analog Schalter Read
******************************************************/
double brewtime          = BREWTIME;
double preinfusion       = PREINFUSION;
double preinfusionpause  = PREINFUSION_PAUSE;
const int analogPin      = 0; // will be use in case of hardware
int brewing              = 0;
int brewswitch           = 0;
bool waitingForBrewSwitchOff = false;
double totalbrewtime     = 0;
unsigned long bezugsZeit = 0;
unsigned long startZeit  = 0;
unsigned long previousBrewCheck = 0;
unsigned long lastBrewMessage   = 0;

/********************************************************
   Sensor check
******************************************************/
boolean sensorError = false;
int error           = 0;
int maxErrorCounter = 10 ;  //define maximum number of consecutive polls (of intervaltempmes* duration) to have errors

/********************************************************
 * Rest
 *****************************************************/
#ifdef EMERGENCY_TEMP
const unsigned int emergency_temperature = EMERGENCY_TEMP;  // temperature at which the emergency shutdown should take place. DONT SET IT ABOVE 120 DEGREE!!
#else
const unsigned int emergency_temperature = 120;             // fallback
#endif
double brewDetectionSensitivity = BREWDETECTION_SENSITIVITY ; // if temperature decreased within the last 6 seconds by this amount, then we detect a brew.
#ifdef BREW_READY_DETECTION
const int brew_ready_led_enabled = BREW_READY_LED;
float marginOfFluctuation = float(BREW_READY_DETECTION);
#else
const int brew_ready_led_enabled = 0;   // 0 = disable functionality
float marginOfFluctuation = 0;          // 0 = disable functionality
#endif
char* blynkReadyLedColor = "#000000";
unsigned long lastCheckBrewReady = 0 ;
bool brewReady = false;
const int expected_eeprom_version = 1;        // EEPROM values are saved according to this versions layout. Increase if a new layout is implemented.
unsigned long eeprom_save_interval = 28*60*1000UL;  //save every 28min
unsigned long last_eeprom_save = 0;
char debugline[100];
unsigned long output_timestamp = 0;
volatile byte bpidComputeHasRun = 0;
unsigned long last_micro = 0;
unsigned long all_services_lastReconnectAttemptTime = 0;
unsigned long all_services_min_reconnect_interval = 160000; // 160sec minimum wait-time between service reconnections
bool force_offline = FORCE_OFFLINE;

/********************************************************
   DISPLAY
******************************************************/
//#include <U8x8lib.h>
//#ifdef U8X8_HAVE_HW_SPI
//#include <SPI.h>
//#endif
//U8X8_SSD1306_128X32_UNIVISION_SW_I2C u8x8(/* clock=*/ 5, /* data=*/ 4, /* reset=*/ 16);   //Display initalisieren  
                        
// Display 128x64
#include <Wire.h>
#include <Adafruit_GFX.h>
//#include <ACROBOTIC_SSD1306.h>
#include <Adafruit_SSD1306.h>
#define OLED_RESET 16
Adafruit_SSD1306 display(OLED_RESET);
#define XPOS 0
#define YPOS 1
#define DELTAY 2
#if (SSD1306_LCDHEIGHT != 64)
#error("Height incorrect, please fix Adafruit_SSD1306.h!");
#endif

/********************************************************
   DALLAS TEMP
******************************************************/
#include <OneWire.h>
#include <DallasTemperature.h>
#define ONE_WIRE_BUS 2         // Data wire is plugged into port 2 on the Arduino
OneWire oneWire(ONE_WIRE_BUS); // Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
DallasTemperature sensors(&oneWire); // Pass our oneWire reference to Dallas Temperature.
DeviceAddress sensorDeviceAddress; // arrays to hold device address

/********************************************************
   B+B Sensors TSIC 306
******************************************************/
#include "TSIC.h"    // include the library
TSIC Sensor1(2);     // only Signalpin, VCCpin unused by default
uint16_t temperature = 0;
float Temperatur_C = 0;
int refreshTempPreviousTimeSpend = 0;

/********************************************************
   BLYNK
******************************************************/
#define BLYNK_PRINT Serial
#include <BlynkSimpleEsp8266.h>
#define BLYNK_GREEN     "#23C48E"
#define BLYNK_YELLOW    "#ED9D00"
#define BLYNK_RED       "#D3435C"
unsigned long previousMillisBlynk = 0;
const long intervalBlynk = 1000;      // Update Intervall zur App
int blynksendcounter = 1;
unsigned long previousMillisDisplay;  // initialisation at the end of init()
const long intervalDisplay = 500;     // Update für Display
bool blynk_sync_run_once = false;
String PreviousError = "";
String PreviousOutputString = "";
String PreviousPastTemperatureChange = "";
String PreviousInputString = "";
bool blynk_disabled_temporary = false;

/******************************************************
 * Receive following BLYNK PIN values from app/server
 ******************************************************/
BLYNK_CONNECTED() {
  if (!blynk_sync_run_once) {
    blynk_sync_run_once = true;
    Blynk.syncAll();  //get all values from server/app when connected
  }
}
// This is called when Smartphone App is opened
BLYNK_APP_CONNECTED() {
  DEBUG_print("App Connected.\n");
}
// This is called when Smartphone App is closed
BLYNK_APP_DISCONNECTED() {
  DEBUG_print("App Disconnected.\n");
}
BLYNK_WRITE(V4) {
  aggKp = param.asDouble();
}
BLYNK_WRITE(V5) {
  aggTn = param.asDouble();
}
BLYNK_WRITE(V6) {
  aggTv = param.asDouble();
}
BLYNK_WRITE(V7) {
  setPoint = param.asDouble();
}
BLYNK_WRITE(V8) {
  brewtime = param.asDouble();
}
BLYNK_WRITE(V9) {
  preinfusion = param.asDouble();
}
BLYNK_WRITE(V10) {
  preinfusionpause = param.asDouble();
}
BLYNK_WRITE(V12) {
  starttemp = param.asDouble();
}
BLYNK_WRITE(V13) {
  pidON = param.asInt();
}
BLYNK_WRITE(V30) {
  aggoKp = param.asDouble();
}
BLYNK_WRITE(V31) {
  aggoTn = param.asDouble();
}
BLYNK_WRITE(V32) {
  aggoTv = param.asDouble();
}
BLYNK_WRITE(V34) {
  brewDetectionSensitivity = param.asDouble();
}
BLYNK_WRITE(V40) {
  burstShot = param.asInt();
}
BLYNK_WRITE(V41) {
  steadyPower = param.asDouble();
  // TODO fix this bPID.SetSteadyPowerDefault(steadyPower); //TOBIAS: working?
}
BLYNK_WRITE(V42) {
  steadyPowerOffset = param.asDouble();
}
BLYNK_WRITE(V43) {
  steadyPowerOffset_Time = param.asInt();
}
BLYNK_WRITE(V44) {
  burstPower = param.asDouble();
}

/******************************************************
 * Type Definition of "sending" BLYNK PIN values from 
 * hardware to app/server (only defined if required)
 ******************************************************/
WidgetLED brewReadyLed(V14);

/******************************************************
 * HELPER
 ******************************************************/
bool wifi_working() {
  return (WiFi.status() == WL_CONNECTED);
}

bool blynk_working() {
  return (WiFi.status() == WL_CONNECTED && Blynk.connected());
}

bool mqtt_working() {
  return (WiFi.status() == WL_CONNECTED && mqtt_client.connected());
}

bool in_sensitive_phase() {
  return (Input >=110 || brewing || activeState==4);
}

/********************************************************
  MQTT
*****************************************************/
#include <math.h>
#include <float.h>
bool almostEqual(float a, float b) {
    return fabs(a - b) <= FLT_EPSILON;
}
char* bool2string(bool in) {
  if (in) {
    return "1";
  } else {
    return "0";
  }
}
char number2string_double[22];
char* number2string(double in) {
  snprintf(number2string_double, sizeof(number2string_double), "%0.2f", in);
  return number2string_double;
}
char number2string_float[22];
char* number2string(float in) {
  snprintf(number2string_float, sizeof(number2string_float), "%0.2f", in);
  return number2string_float;
}
char number2string_int[22];
char* number2string(int in) {
  snprintf(number2string_int, sizeof(number2string_int), "%d", in);
  return number2string_int;
}
char number2string_uint[22];
char* number2string(unsigned int in) {
  snprintf(number2string_uint, sizeof(number2string_uint), "%u", in);
  return number2string_uint;
}

char* mqtt_build_topic(char* reading) {
  char* topic = (char *) malloc(sizeof(char) * 256);
  snprintf(topic, sizeof(topic), "%s%s/%s", mqtt_topic_prefix, hostname, reading);
  return topic;
}

boolean mqtt_publish(char* reading, char* payload) {
  if (!MQTT_ENABLE || force_offline || mqtt_disabled_temporary) return true;
  char topic[MQTT_MAX_PUBLISH_SIZE];
  snprintf(topic, MQTT_MAX_PUBLISH_SIZE, "%s%s/%s", mqtt_topic_prefix, hostname, reading);
  if (!mqtt_working()) {
    DEBUG_print("Not connected to mqtt server. Cannot publish(%s %s)\n", topic, payload);
    return false;
  }
  if (strlen(topic) + strlen(payload) >= MQTT_MAX_PUBLISH_SIZE) {
    ERROR_print("mqtt_publish() wants to send too much data (len=%u)\n", strlen(topic) + strlen(payload));
    return false;
  } else {
    unsigned long currentMillis = millis();
    if (currentMillis > mqtt_dontPublishUntilTime) {
      boolean ret = mqtt_client.publish(topic, payload, mqtt_flag_retained);
      if (ret == false) { //TODO test this code block later (faking an error, eg millis <30000?)
        mqtt_dontPublishUntilTime = millis() + mqtt_dontPublishBackoffTime;
        ERROR_print("Error on publish. Wont publish the next %ul ms\n", mqtt_dontPublishBackoffTime);
        mqtt_client.disconnect();
      }
      return ret;
    } else { //TODO test this code block later (faking an error)
      ERROR_print("Data not published (still for the next %ul ms)\n", mqtt_dontPublishUntilTime - currentMillis);
      return false;
    }
  }
}

boolean mqtt_reconnect(bool force_connect = false) {
  if (!MQTT_ENABLE || force_offline || mqtt_disabled_temporary || mqtt_working() || in_sensitive_phase() ) return true;
  espClient.setTimeout(2000); // set timeout for mqtt connect()/write() to 2 seconds (default 5 seconds).
  unsigned long now = millis();
  if ( force_connect || ((now > mqtt_lastReconnectAttemptTime + (mqtt_reconnect_incremental_backoff * (mqtt_reconnectAttempts))) && now > all_services_lastReconnectAttemptTime + all_services_min_reconnect_interval)) {
    mqtt_lastReconnectAttemptTime = now;
    all_services_lastReconnectAttemptTime = now;
    DEBUG_print("Connecting to mqtt ...\n");
    if (mqtt_client.connect(hostname, mqtt_username, mqtt_password, topic_will, 0, 0, "unexpected exit") == true) {
      DEBUG_print("Connected to mqtt server\n");
      mqtt_publish("events", "Connected to mqtt server");
      mqtt_client.subscribe(topic_set);
      mqtt_lastReconnectAttemptTime = 0;
      mqtt_reconnectAttempts = 0;
    } else {
      DEBUG_print("Cannot connect to mqtt server (consecutive failures=#%u)\n", mqtt_reconnectAttempts);
      if (mqtt_reconnectAttempts < mqtt_max_incremental_backoff) {
        mqtt_reconnectAttempts++;
      }
    }
  }
  return mqtt_client.connected();
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  DEBUG_print("Message arrived [%s]: %s\n", topic, (const char *)payload);
  // OPTIONAL TODO: business logic to activate rancilio functions from external (eg brewing, PidOn, startup, PID parameters,..)
}

/********************************************************
  Emergency Stop when temp too high
*****************************************************/
void testEmergencyStop(){
  if (getCurrentTemperature() >= emergency_temperature){
    if (emergencyStop != true) {
      snprintf(debugline, sizeof(debugline), "EmergencyStop because temperature>%u (temperature=%0.2f)", emergency_temperature, getCurrentTemperature());
      ERROR_println(debugline);
      mqtt_publish("events", debugline);
    }
    emergencyStop = true;
  } else if (getCurrentTemperature() < 100) {
    if (emergencyStop == true) {
      snprintf(debugline, sizeof(debugline), "EmergencyStop ended because temperature<100 (temperature=%0.2f)", getCurrentTemperature());
      ERROR_println(debugline);
      mqtt_publish("events", debugline);
    }
    emergencyStop = false;
  }
}

/********************************************************
  history temperature data
*****************************************************/
void updateTemperatureHistory(double myInput) {
  if (readIndex >= numReadings -1) {
    readIndex = 0;
  } else {
    readIndex++;
  }
  readingstime[readIndex] = millis();
  readingstemp[readIndex] = myInput;
}

//calculate the temperature difference between NOW and a datapoint in history
double pastTemperatureChange(int lookback) {
  double temperatureDiff;
  if (lookback >= numReadings) lookback=numReadings -1;
  int offset = lookback % numReadings;
  int historicIndex = (readIndex - offset);
  if ( historicIndex < 0 ) {
    historicIndex += numReadings;
  }
  //ignore not yet initialized values
  if (readingstime[readIndex] == 0 || readingstime[historicIndex] == 0) return 0;
  if (brewDetectionSensitivity <= 30) {
    temperatureDiff = (readingstemp[readIndex] - readingstemp[historicIndex]);
  } else { // use previous factor on brewDetectionSensitivity threshold (compatibility to old brewDetectionSensitivity values using a factor of 100)
    temperatureDiff = (readingstemp[readIndex] - readingstemp[historicIndex]) * 100;
  }
  return temperatureDiff;
}

//calculate the average temperature over the last (lookback) temperatures samples
double getAverageTemperature(int lookback) {
  double averageInput = 0;
  int count = 0;
  if (lookback >= numReadings) lookback=numReadings -1;
  for (int offset = 0; offset < lookback; offset++) {
    int thisReading = readIndex - offset;
    if (thisReading < 0) thisReading = numReadings + thisReading;
    if (readingstime[thisReading] == 0) break;
    averageInput += readingstemp[thisReading];
    count += 1;
  }
  if (count > 0) {
    return averageInput / count;
  } else {
    DEBUG_print("getAverageTemperature() returned 0");
    return 0;
  }
}

double getCurrentTemperature() {
  return readingstemp[readIndex];
}

//returns heater utilization in percent
double convertOutputToUtilisation(double Output) {
  return (100 * Output) / windowSize;
}

//returns heater utilization in Output
double convertUtilisationToOutput(double utilization) {
  return (utilization / 100 ) * windowSize;
}

bool checkBrewReady(double setPoint, float marginOfFluctuation, int lookback) {
  if (almostEqual(marginOfFluctuation, 0)) return false;
  if (lookback >= numReadings) lookback=numReadings -1;
  for (int offset = 0; offset < lookback; offset++) {
    int thisReading = readIndex - offset;
    if (thisReading < 0) thisReading = numReadings + thisReading;
    if (readingstime[thisReading] == 0) return false;
    if (fabs(setPoint - readingstemp[thisReading]) > (marginOfFluctuation + FLT_EPSILON)) return false;
  }
  return true;
}

void refreshBrewReadyHardwareLed(boolean brewReady) {
  static boolean lastBrewReady = false;
  if (!brew_ready_led_enabled) return;
  if (brewReady != lastBrewReady) {
    digitalWrite(pinLed, brewReady);
    lastBrewReady = brewReady;
  }
}

/********************************************************
  check sensor value. If < 0 or difference between old and new >10, then increase error.
  If error is equal to maxErrorCounter, then set sensorError
*****************************************************/
boolean checkSensor(float tempInput, float temppreviousInput) {
  boolean sensorOK = false;
  /********************************************************
    sensor error
  ******************************************************/
  if ( ( tempInput < 0 || tempInput > 150 || fabs(tempInput - temppreviousInput) > 5) && !sensorError) {
    error++;
    sensorOK = false;
    DEBUG_print("temperature sensor reading: consec_errors=%d, temp_current=%0.2f, temp_prev=%0.2f\n", error, tempInput, temppreviousInput);
  } else {
    error = 0;
    sensorOK = true;
  }

  if (error >= maxErrorCounter && !sensorError) {
    sensorError = true;
    snprintf(debugline, sizeof(debugline), "temperature sensor malfunction: temp_current=%0.2f, temp_prev=%0.2f", tempInput, previousInput);
    ERROR_println(debugline);
    mqtt_publish("events", debugline);
  } else if (error == 0 && TempSensorRecovery == 1) { //Safe-guard: prefer to stop heating forever if sensor is flapping!
    sensorError = false;
  }

  return sensorOK;
}

/********************************************************
  Refresh temperature.
  Each time checkSensor() is called to verify the value.
  If the value is not valid, new data is not stored.
*****************************************************/
void refreshTemp() {
  /********************************************************
    Temp. Request
  ******************************************************/
  unsigned long currentMillistemp = millis();
  previousInput = getCurrentTemperature() ;
  long millis_elapsed = currentMillistemp - previousMillistemp ;
  if ( millis_elapsed <0 ) millis_elapsed = 0;
  if (TempSensor == 1)
  {
    if ( floor(millis_elapsed / refreshTempInterval) >= 2) {
      snprintf(debugline, sizeof(debugline), "Temporary main loop() hang. Number of temp polls missed=%g, millis_elapsed=%lu", floor(millis_elapsed / refreshTempInterval) -1, millis_elapsed);
      ERROR_println(debugline);
      mqtt_publish("events", debugline);
    }
    if (millis_elapsed >= refreshTempInterval)
    {
      sensors.requestTemperatures();
      previousMillistemp = currentMillistemp;
      if (!checkSensor(sensors.getTempCByIndex(0), previousInput)) return;  //if sensor data is not valid, abort function
      updateTemperatureHistory(sensors.getTempCByIndex(0));
      Input = getAverageTemperature(5);
    }
  }
  if (TempSensor == 2)
  {
    if ( floor(millis_elapsed / refreshTempInterval) >= 2) {
      snprintf(debugline, sizeof(debugline), "Temporary main loop() hang. Number of temp polls missed=%g, millis_elapsed=%lu", floor(millis_elapsed / refreshTempInterval) -1, millis_elapsed);
      ERROR_println(debugline);
      mqtt_publish("events", debugline);
    }
    if (millis_elapsed >= refreshTempInterval)
    {
      // variable "temperature" must be set to zero, before reading new data
      // getTemperature only updates if data is valid, otherwise "temperature" will still hold old values
      temperature = 0;
      //unsigned long start = millis();
      Sensor1.getTemperature(&temperature);
      unsigned long stop = millis();
      previousMillistemp = stop;
      
      // temperature must be between 0x000 and 0x7FF(=DEC2047)
      Temperatur_C = Sensor1.calc_Celsius(&temperature); 

      //DEBUG_print("millis=%lu | previousMillistemp=%lu | diff=%lu | Temperatur_C=%0.3f | time_spend=%lu\n", millis(), previousMillistemp, currentMillistemp - previousMillistemp2, Temperatur_C, stop - start);  //TOBIAS
      //previousMillistemp2 = currentMillistemp;
      
      // Temperature_C must be -50C < Temperature_C <= 150C
      if (!checkSensor(Temperatur_C, previousInput)) {
        return;  //if sensor data is not valid, abort function
      }
      updateTemperatureHistory(Temperatur_C);
      Input = getAverageTemperature(5);
    }
  }
}

/********************************************************
    PreInfusion, Brew , if not Only PID
******************************************************/
void brew() {
  if (OnlyPID == 0) {
    unsigned long aktuelleZeit = millis();
    
    if ( aktuelleZeit >= previousBrewCheck + 50 ) {  //50ms
      previousBrewCheck = aktuelleZeit;
      brewswitch = analogRead(analogPin);

      //if (aktuelleZeit >= output_timestamp + 500) {
      //  DEBUG_print("brew(): brewswitch=%u | brewing=%u | waitingForBrewSwitchOff=%u\n", brewswitch, brewing, waitingForBrewSwitchOff);
      //  output_timestamp = aktuelleZeit;
      //}
      if (brewswitch > 700 && not (brewing == 0 && waitingForBrewSwitchOff) ) {
        totalbrewtime = (preinfusion + preinfusionpause + brewtime) * 1000;
        
        if (brewing == 0) {
          brewing = 1;
          startZeit = aktuelleZeit;
          waitingForBrewSwitchOff = true;
          DEBUG_print("brewswitch=on - Starting brew()\n");
        }
        bezugsZeit = aktuelleZeit - startZeit; 
  
        //if (aktuelleZeit >= lastBrewMessage + 500) {
        //  lastBrewMessage = aktuelleZeit;
        //  DEBUG_print("brew(): bezugsZeit=%lu totalbrewtime=%0.1f\n", bezugsZeit/1000, totalbrewtime/1000);
        //}
        if (bezugsZeit <= totalbrewtime) {
          if (bezugsZeit <= preinfusion*1000) {
            //DEBUG_println("preinfusion");
            digitalWrite(pinRelayVentil, relayON);
            digitalWrite(pinRelayPumpe, relayON);
          } else if (bezugsZeit > preinfusion*1000 && bezugsZeit <= (preinfusion + preinfusionpause)*1000) {
            //DEBUG_println("Pause");
            digitalWrite(pinRelayVentil, relayON);
            digitalWrite(pinRelayPumpe, relayOFF);
          } else if (bezugsZeit > (preinfusion + preinfusionpause)*1000) {
            //DEBUG_println("Brew");
            digitalWrite(pinRelayVentil, relayON);
            digitalWrite(pinRelayPumpe, relayON);
          }
        } else {
          DEBUG_print("End brew()\n");
          brewing = 0;
        }
      }
  
      if (brewswitch <= 700) {
        if (waitingForBrewSwitchOff) {
          DEBUG_print("brewswitch=off\n");
        }
        waitingForBrewSwitchOff = false;
        brewing = 0;
        bezugsZeit = 0;
      }
      if (brewing == 0) {
          //DEBUG_println("aus");
          digitalWrite(pinRelayVentil, relayOFF);
          digitalWrite(pinRelayPumpe, relayOFF);
      }
    }
  }
}

 /********************************************************
   Check if Wifi is connected, if not reconnect
 *****************************************************/
 void checkWifi(unsigned long wifiConnectWaitTime_tmp = wifiConnectWaitTime){
  if (force_offline || wifi_working() || in_sensitive_phase()) return;
  if (millis() > lastWifiConnectionAttempt + 5000 + (wifiReconnectInterval * wifiReconnects)) {
    lastWifiConnectionAttempt = millis();
    DEBUG_print("Wifi reconnecting...\n");
    WiFi.persistent(false);  // this is required, else arduino reboots
    WiFi.disconnect(true);
    WiFi.hostname(hostname);
    WiFi.begin(ssid, pass);
    while ((!wifi_working()) && ( millis() < lastWifiConnectionAttempt + wifiConnectWaitTime)) {
        yield(); //Prevent Watchdog trigger
    }
    if (wifi_working()) {
      wifiReconnects = 0;
      DEBUG_print("Wifi reconnection attempt (#%u) successfull in %lu seconds. WiFi.Status=%d\n", wifiReconnects, (millis() - lastWifiConnectionAttempt) /1000, WiFi.status());
      
    } else {
      wifiReconnects++;
      ERROR_print("Wifi reconnection attempt (#%u) not successfull. WiFi.Status=%d\n", wifiReconnects, (millis() - lastWifiConnectionAttempt) /1000, WiFi.status());
    }   
  }
}

/********************************************************
  send data to Blynk server
*****************************************************/
void sendToBlynk() {
  if (force_offline || !blynk_working() || blynk_disabled_temporary) return;
  unsigned long currentMillisBlynk = millis();
  if (currentMillisBlynk >= previousMillisBlynk + intervalBlynk) {
    previousMillisBlynk = currentMillisBlynk;
    if (brewReady) {
      if (blynkReadyLedColor != BLYNK_GREEN) {
        blynkReadyLedColor = BLYNK_GREEN;
        brewReadyLed.setColor(blynkReadyLedColor);
      }
    } else if (marginOfFluctuation != 0 && checkBrewReady(setPoint, marginOfFluctuation * 2, 40)) {
      if (blynkReadyLedColor != BLYNK_YELLOW) {
        blynkReadyLedColor = BLYNK_YELLOW;
        brewReadyLed.setColor(blynkReadyLedColor);
      }
    } else {
      if (blynkReadyLedColor != BLYNK_RED) {
        brewReadyLed.on();
        blynkReadyLedColor = BLYNK_RED;
        brewReadyLed.setColor(blynkReadyLedColor);
      }
    }
    if (grafana == 1 && blynksendcounter == 1) {
      Blynk.virtualWrite(V60, Input, Output, bPID.GetKp(), bPID.GetKi(), bPID.GetKd(), setPoint );
    }
    //performance tests has shown to only send one api-call per sendToBlynk() 
    if (blynksendcounter == 1) {
      if (steadyPower != PreviousSteadyPower) {
        Blynk.virtualWrite(V41, steadyPower);  //auto-tuning params should be saved by Blynk.virtualWrite()
        PreviousSteadyPower = steadyPower;
      } else {
        blynksendcounter++;
      }
    }
    if (blynksendcounter == 2) {
      if (String(pastTemperatureChange(10)/2, 2) != PreviousPastTemperatureChange) {
        Blynk.virtualWrite(V35, String(pastTemperatureChange(10)/2, 2));
        PreviousPastTemperatureChange = String(pastTemperatureChange(10)/2, 2);
      } else {
        blynksendcounter++;
      }
    } 
    if (blynksendcounter == 3) {
      if (String(Input - setPoint, 2) != PreviousError) {
        Blynk.virtualWrite(V11, String(Input - setPoint, 2));
        PreviousError = String(Input - setPoint, 2);
      } else {
        blynksendcounter++;
      }
    }
    if (blynksendcounter == 4) {
      if (String(convertOutputToUtilisation(Output), 2) != PreviousOutputString) {
        Blynk.virtualWrite(V23, String(convertOutputToUtilisation(Output), 2));
        PreviousOutputString = String(convertOutputToUtilisation(Output), 2);
      } else {
        blynksendcounter++;
      }
    }
    if (blynksendcounter >= 5) {
      if (String(Input, 2) != PreviousInputString) {
        Blynk.virtualWrite(V2, String(Input, 2)); //send value to server
        PreviousInputString = String(Input, 2);
      }
      //Blynk.syncVirtual(V2);  //get value from server 
      blynksendcounter = 0;  
    }
    blynksendcounter++;
  }
}

/********************************************************
    state Detection
******************************************************/
void updateState() {
  switch (activeState) {
    case 1: // state 1 running, that means full heater power. Check if target temp is reached
    {
      bPID.SetFilterSumOutputI(100);
      if (Input >= starttemp) {
        snprintf(debugline, sizeof(debugline), "** End of Coldstart. Transition to step 2 (constant steadyPower)");
        DEBUG_println(debugline);
        mqtt_publish("events", debugline);
        bPID.SetSumOutputI(0);
        activeState = 2;
      }
      break;
    }
    case 2: // state 2 running, that means heater is on steadyState and we are waiting to temperature to stabilize
    {
      bPID.SetFilterSumOutputI(30);
      double tempChange = pastTemperatureChange(20);
      if ( (Input - setPoint >= 0) || (Input - setPoint <= -20) || (Input - setPoint <= 0  && tempChange <= 0.3)) {
        snprintf(debugline, sizeof(debugline), "** End of stabilizing. Transition to step 3 (normal mode)");
        DEBUG_println(debugline);
        mqtt_publish("events", debugline);
        bPID.SetSumOutputI(0);
        activeState = 3;
      }
      break;
    }
    case 4: // state 4 running = Brew running
    {
      bPID.SetFilterSumOutputI(100);
      bPID.SetAutoTune(false);
      if (Input > setPoint - outerZoneTemperatureDifference ||
          pastTemperatureChange(10) >= 0.5) {
        snprintf(debugline, sizeof(debugline), "** End of Brew. Transition to step 3 (normal mode)");
        DEBUG_println(debugline);
        mqtt_publish("events", debugline);
        bPID.SetAutoTune(true);
        bPID.SetSumOutputI(0);
        timerBrewDetection = 0 ;
        activeState = 3;
      }
      break;
    }
    case 5: // state 5 in outerZone
    {
      bPID.SetFilterSumOutputI(9);
      if (Input > setPoint - outerZoneTemperatureDifference) {
        snprintf(debugline, sizeof(debugline), "** End of outerZone. Transition to step 3 (normal mode)");
        DEBUG_println(debugline);
        mqtt_publish("events", debugline);
        bPID.SetSumOutputI(0);
        timerBrewDetection = 0 ;
        activeState = 3;
      }
      break;
    }
    case 3: // normal PID mode
    default:
    {
      //set maximum allowed filterSumOutputI based on error/marginOfFluctuation
      if ( Input >= setPoint - marginOfFluctuation) {
        bPID.SetFilterSumOutputI(1.0);
      } else if ( Input >= setPoint - 0.5) {
        bPID.SetFilterSumOutputI(4.5);
      } else {
        bPID.SetFilterSumOutputI(6);
      } 
      
      /* STATE 1 (COLDSTART) DETECTION */
      if ( Input <= starttemp - coldStartStep1ActivationOffset) {
        snprintf(debugline, sizeof(debugline), "** End of normal mode. Transition to step 1 (coldstart)");
        DEBUG_println(debugline);
        mqtt_publish("events", debugline);
        bPID.SetSteadyPowerOffset(steadyPowerOffset);
        steadyPowerOffset_Activated = millis();
        bPID.SetAutoTune(false);  //do not tune during powerOffset
        DEBUG_print("Enable steadyPowerOffset (steadyPower += %0.2f)\n", steadyPowerOffset);
        bPID.SetSumOutputI(0);
        activeState = 1;
        break;
      }

      /* STATE 4 (BREW) DETECTION */
      if (brewDetectionSensitivity != 0 && brewDetection == 1) {
        //enable brew-detection if not already running and diff temp is > brewDetectionSensitivity
        if (pastTemperatureChange(6) <= -brewDetectionSensitivity &&
            Input < setPoint - outerZoneTemperatureDifference) {
          testTrigger = 0;
          if (OnlyPID == 1) {
            bezugsZeit = 0 ;
          }
          timeBrewDetection = millis() ;
          timerBrewDetection = 1 ;
          mqtt_publish("brewDetected", "1");
          snprintf(debugline, sizeof(debugline), "** End of normal mode. Transition to step 4 (brew)");
          DEBUG_println(debugline);
          mqtt_publish("events", debugline);
          bPID.SetSumOutputI(0);
          activeState = 4;
          break;
        }
      }

      /* STATE 5 (OUTER ZONE) DETECTION */
      if ( (Input > starttemp - coldStartStep1ActivationOffset && 
            Input < setPoint - outerZoneTemperatureDifference) || testTrigger  ) {
        snprintf(debugline, sizeof(debugline), "** End of normal mode. Transition to step 5 (outerZone)");
        DEBUG_println(debugline);
        mqtt_publish("events", debugline);
        bPID.SetSumOutputI(0);
        activeState = 5;
        break;
      }

      break;
    }
  }
  
  // steadyPowerOffset_Activated handling
  if ( steadyPowerOffset_Activated >0 ) {
    if (Input - setPoint >= 0.4) {
      bPID.SetSteadyPowerOffset(0);
      steadyPowerOffset_Activated = 0;
      snprintf(debugline, sizeof(debugline), "Disabled steadyPowerOffset because its too large or starttemp too high");
      ERROR_println(debugline);
      mqtt_publish("events", debugline);
      bPID.SetAutoTune(true);
    }
    if (millis() >= steadyPowerOffset_Activated + steadyPowerOffset_Time*1000) {
      //DEBUG_print("millis=%lu | steadyPowerOffset_Activated=%0.2f | steadyPowerOffset_Time=%d\n", millis(), steadyPowerOffset_Activated, steadyPowerOffset_Time*1000);
      bPID.SetSteadyPowerOffset(0);
      steadyPowerOffset_Activated = 0;
      DEBUG_print("Disable steadyPowerOffset (steadyPower -= %0.2f)\n", steadyPowerOffset);
      bPID.SetAutoTune(true);
    }
  }
}

void printPidStatus() {
  if (bpidComputeHasRun > 0) {
    bpidComputeHasRun = 0;
    DEBUG_print("Input=%6.2f | error=%5.2f delta=%5.2f | Output=%6.2f = b:%5.2f + p:%5.2f + i:%5.2f(%5.2f) + d:%5.2f\n", 
      Input,
      (setPoint - Input),
      pastTemperatureChange(10)/2,
      convertOutputToUtilisation(Output),
      steadyPower + ((steadyPowerOffset_Activated) ? steadyPowerOffset: 0),
      convertOutputToUtilisation(bPID.GetOutputP()),
      convertOutputToUtilisation(bPID.GetSumOutputI()),
      convertOutputToUtilisation(bPID.GetOutputI()),
      convertOutputToUtilisation(bPID.GetOutputD())
      );
  }
}

void ICACHE_RAM_ATTR onTimer1ISR() {
  timer1_write(50000); // set interrupt time to 10ms
  if ( bPID.Compute() ) {
    isrCounter = 0;  // Attention: heater might not shutdown if bPid.SetSampleTime(), windowSize, timer1_write() and are not set correctly!
    bpidComputeHasRun = 1;
  }
  if (isrCounter >= Output) {
    digitalWrite(pinRelayHeater, LOW);
  } else {
    digitalWrite(pinRelayHeater, HIGH);
  }
  if (isrCounter <= windowSize) {
    isrCounter += 10; // += 10 because one tick = 10ms
  }
}


/***********************************
 * LOOP()
 ***********************************/
void loop() {
  if (!wifi_working()) {
    checkWifi();
  } else {

    if (!force_offline) {
      ArduinoOTA.handle();
      // Disable interrupt when OTA starts, otherwise it will not work
      ArduinoOTA.onStart([](){
        DEBUG_print("OTA update initiated\n");
        Output = 0;
        timer1_disable();
        digitalWrite(pinRelayHeater, LOW); //Stop heating
      });
      ArduinoOTA.onError([](ota_error_t error) {
        ERROR_print("OTA update error\n");
        timer1_enable(TIM_DIV16, TIM_EDGE, TIM_SINGLE);
      });
      // Enable interrupts if OTA is finished
      ArduinoOTA.onEnd([](){
        timer1_enable(TIM_DIV16, TIM_EDGE, TIM_SINGLE);
      });
    }

    if (BLYNK_ENABLE && !force_offline && !blynk_disabled_temporary) {
      if (blynk_working()) {
        Blynk.run(); //Do Blynk household stuff. (On reconnect after disconnect, timeout seems to be 5 seconds)
      } else {
        unsigned long now = millis();
        if ((now > blynk_lastReconnectAttemptTime + (blynk_reconnect_incremental_backoff * (blynk_reconnectAttempts))) 
             && now > all_services_lastReconnectAttemptTime + all_services_min_reconnect_interval
             && !in_sensitive_phase() ) {
            blynk_lastReconnectAttemptTime = now;
            all_services_lastReconnectAttemptTime = now;
            ERROR_print("Blynk disconnected. Reconnecting...\n");
            if ( Blynk.connect(2000) ) { // Attempt to reconnect
              blynk_lastReconnectAttemptTime = 0;
              blynk_reconnectAttempts = 0;
              DEBUG_print("Blynk reconnected in %lu seconds\n", (millis() - now)/1000);
            } else if (blynk_reconnectAttempts < blynk_max_incremental_backoff) {
              blynk_reconnectAttempts++;
            }
        }
      }
    }
  
    //Check mqtt connection
    if (MQTT_ENABLE && !force_offline && !mqtt_disabled_temporary) {
      unsigned long now = millis();
      mqtt_client.loop(); // mqtt client connected, do mqtt housekeeping
      if (!mqtt_working()) {
        mqtt_reconnect();
      } else {
        if (now >= lastMQTTStatusReportTime + lastMQTTStatusReportInterval) {
          lastMQTTStatusReportTime = now;
          //TODO performance?: use beginPublish() and endPublish() instead
          mqtt_publish("temperature", number2string(Input));  //TODO: check this in mqtt explorer!!
          mqtt_publish("temperatureAboveTarget", number2string((Input - setPoint)));
          mqtt_publish("heaterUtilization", number2string(convertOutputToUtilisation(Output)));
          //mqtt_publish("kp", number2string(bPID.GetKp()));
          //mqtt_publish("ki", number2string(bPID.GetKi()));
          //mqtt_publish("kd", number2string(bPID.GetKd()));
          //mqtt_publish("outputP", number2string(convertOutputToUtilisation(bPID.GetOutputP())));
          //mqtt_publish("outputI", number2string(convertOutputToUtilisation(bPID.GetOutputI())));
          //mqtt_publish("outputD", number2string(convertOutputToUtilisation(bPID.GetOutputD())));
          mqtt_publish("pastTemperatureChange", number2string(pastTemperatureChange(10)));
          mqtt_publish("brewReady", bool2string(brewReady));
         }
      }
    }
  }
  
  unsigned long startT;
  unsigned long stopT;

  refreshTemp();   //read new temperature values
  testEmergencyStop();  // test if Temp is to high
  brew();   //start brewing if button pressed
  if (millis() > lastCheckBrewReady + refreshTempInterval) {
    brewReady = checkBrewReady(setPoint, marginOfFluctuation, 40);
    lastCheckBrewReady = millis();
  }
  refreshBrewReadyHardwareLed(brewReady);

  //check if PID should run or not. If not, set to manuel and force output to zero
  if (pidON == 0 && pidMode == 1) {
    pidMode = 0;
    bPID.SetMode(pidMode);
    Output = 0 ;
    DEBUG_print("Set PID=off\n");
  } else if (pidON == 1 && pidMode == 0) {
    Output = 0; // safety: be 100% sure that PID.compute() starts fresh.
    pidMode = 1;
    bPID.SetMode(pidMode);
    if ( millis() - output_timestamp > 21000) {
      DEBUG_print("Set PID=on\n");
      output_timestamp = millis();
    }
  }
  if (burstShot == 1 && pidMode == 1) {
    burstShot = 0;
    bPID.SetBurst(burstPower);
    snprintf(debugline, sizeof(debugline), "BURST Output=%0.2f", convertOutputToUtilisation(Output));
    DEBUG_println(debugline);
    mqtt_publish("events", debugline);
  }

  //Sicherheitsabfrage
  if (!sensorError && !emergencyStop && Input > 0) {
    printPidStatus();
    updateState();

    /* state 1: Water is very cold, set heater to full power */
    if (activeState == 1) {
      Output = windowSize;  //fix mqtt to show correct values

    /* state 2: ColdstartTemp reached. Now stabilizing temperature after coldstart */
    } else if (activeState == 2) {
      Output = convertUtilisationToOutput(steadyPower);  //fix mqtt to show correct values

    /* state 4: Brew detected. Increase heater power */
    } else if (activeState == 4) {
      Output = windowSize;
      if (OnlyPID == 1 && timerBrewDetection == 1){
        bezugsZeit = millis() - timeBrewDetection;
      }

    /* state 5: Outer Zone reached. More power than in inner zone */
    } else if (activeState == 5) {
      if (aggoTn != 0) {
        aggoKi = aggoKp / aggoTn ;
      } else {
        aggoKi = 0;
      }
      aggoKd = aggoTv * aggoKp ;
      if (pidMode == 1) bPID.SetMode(AUTOMATIC);
      bPID.SetTunings(aggoKp, aggoKi, aggoKd);

    /* state 3: Inner zone reached = "normal" low power mode */
    } else {
      if (pidMode == 1) bPID.SetMode(AUTOMATIC);
      if (aggTn != 0) {
        aggKi = aggKp / aggTn ;
      } else {
        aggKi = 0 ;
      }
      aggKd = aggTv * aggKp ;
      bPID.SetTunings(aggKp, aggKi, aggKd);
    }

    sendToBlynk();

    unsigned long currentMillisDisplay = millis();
    if (currentMillisDisplay - previousMillisDisplay >= intervalDisplay) {
      previousMillisDisplay  = currentMillisDisplay;
      displaymessage("brew", "", "", "");
    }

  } else if (sensorError) {
    //Deactivate PID
    if (pidMode == 1) {
      pidMode = 0;
      bPID.SetMode(pidMode);
      Output = 0 ;
      if ( millis() - output_timestamp > 15000) {
        ERROR_print("sensorError detected. Shutdown PID and heater\n");
        output_timestamp = millis();
      }
    }
    digitalWrite(pinRelayHeater, LOW); //Stop heating
    //DISPLAY AUSGABE
    unsigned long currentMillisDisplay = millis();
    if (currentMillisDisplay - previousMillisDisplay >= intervalDisplay) {
      previousMillisDisplay  = currentMillisDisplay;
      snprintf(debugline, sizeof(debugline), "Temp: %0.2f", getCurrentTemperature());
      displaymessage("rancilio", "Check Temp. Sensor!", debugline, "");
    }
    
  } else if (emergencyStop) {
    //Deactivate PID
    if (pidMode == 1) {
      pidMode = 0;
      bPID.SetMode(pidMode);
      Output = 0 ;
      if ( millis() - output_timestamp > 10000) {
         ERROR_print("emergencyStop detected. Shutdown PID and heater (temp=%0.2f)\n", getCurrentTemperature());
         output_timestamp = millis();
      }
    }
    digitalWrite(pinRelayHeater, LOW); //Stop heating
    //DISPLAY AUSGABE
    unsigned long currentMillisDisplay = millis();
    if (currentMillisDisplay - previousMillisDisplay >= intervalDisplay) {
      previousMillisDisplay  = currentMillisDisplay;
      char line2[17];
      char line3[17];
      snprintf(line2, sizeof(line2), "Temp: %0.2f", getCurrentTemperature());
      snprintf(line3, sizeof(line3), "Temp > %u", emergency_temperature);
      displaymessage(EMERGENCY_ICON, EMERGENCY_TEXT, line2, line3);
    }
    
  } else {
    if ( millis() - output_timestamp > 15000) {
       ERROR_print("unknown error\n");
       output_timestamp = millis();
    }
  }

  if (millis() >= last_eeprom_save + eeprom_save_interval) {
    last_eeprom_save = millis();
    sync_eeprom();
  }
  Debug.handle();
  yield();
}



/***********************************
 * EEPROM
 ***********************************/
void sync_eeprom() { sync_eeprom(false); }
void sync_eeprom(bool force_read) {
  int current_version;
  DEBUG_print("EEPROM: sync_eeprom(%d) called\n", force_read);
  EEPROM.begin(1024);
  EEPROM.get(290, current_version);
  DEBUG_print("EEPROM: Detected Version=%d Expected Version=%d\n", current_version, expected_eeprom_version);
  if (current_version != expected_eeprom_version) {
    ERROR_print("EEPROM: Settings are corrupt or not previously set. Ignoring..\n");
    EEPROM.put(290, expected_eeprom_version);
  }

  //if variables are not read from blynk previously, always get latest values from EEPROM
  if (force_read && current_version == expected_eeprom_version) {
    DEBUG_print("EEPROM: Blynk not active. Reading settings from EEPROM\n");
    EEPROM.get(0, aggKp);
    EEPROM.get(10, aggTn);
    EEPROM.get(20, aggTv);
    EEPROM.get(30, setPoint);
    EEPROM.get(40, brewtime);
    EEPROM.get(50, preinfusion);
    EEPROM.get(60, preinfusionpause);
    EEPROM.get(80, starttemp);
    EEPROM.get(90, aggoKp);
    EEPROM.get(100, aggoTn);
    EEPROM.get(110, aggoTv);
    EEPROM.get(130, brewDetectionSensitivity);
    EEPROM.get(140, steadyPower);
    EEPROM.get(150, steadyPowerOffset);
    EEPROM.get(160, steadyPowerOffset_Time);
    EEPROM.get(170, burstPower);
    //Reminder: 290 is reserved for "version"
  }

  //if blynk vars are not read previously, get latest values from EEPROM
  double aggKp_latest_saved;
  double aggTn_latest_saved;
  double aggTv_latest_saved;
  double aggoKp_latest_saved;
  double aggoTn_latest_saved;
  double aggoTv_latest_saved;
  double setPoint_latest_saved;
  double brewtime_latest_saved;
  double preinfusion_latest_saved;
  double preinfusionpause_latest_saved;
  double starttemp_latest_saved;
  double brewDetectionSensitivity_latest_saved;
  double steadyPower_latest_saved;
  double steadyPowerOffset_latest_saved;
  int steadyPowerOffset_Time_latest_saved;
  double burstPower_latest_saved;
  EEPROM.get(0, aggKp_latest_saved);
  EEPROM.get(10, aggTn_latest_saved);
  EEPROM.get(20, aggTv_latest_saved);
  EEPROM.get(30, setPoint_latest_saved);
  EEPROM.get(40, brewtime_latest_saved);
  EEPROM.get(50, preinfusion_latest_saved);
  EEPROM.get(60, preinfusionpause_latest_saved);
  EEPROM.get(80, starttemp_latest_saved);
  EEPROM.get(90, aggoKp_latest_saved);
  EEPROM.get(100, aggoTn_latest_saved);
  EEPROM.get(110, aggoTv_latest_saved);
  EEPROM.get(130, brewDetectionSensitivity_latest_saved);
  EEPROM.get(140, steadyPower_latest_saved);
  EEPROM.get(150, steadyPowerOffset_latest_saved);
  EEPROM.get(160, steadyPowerOffset_Time_latest_saved);
  EEPROM.get(170, burstPower_latest_saved); 

  //get saved userConfig.h values
  double aggKp_config_saved;
  double aggTn_config_saved;
  double aggTv_config_saved;
  double aggoKp_config_saved;
  double aggoTn_config_saved;
  double aggoTv_config_saved;
  double setPoint_config_saved;
  double brewtime_config_saved;
  double preinfusion_config_saved;
  double preinfusionpause_config_saved;
  double starttemp_config_saved;
  double brewDetectionSensitivity_config_saved;
  double steadyPower_config_saved;
  double steadyPowerOffset_config_saved;
  int steadyPowerOffset_Time_config_saved;
  double burstPower_config_saved;
  EEPROM.get(300, aggKp_config_saved);
  EEPROM.get(310, aggTn_config_saved);
  EEPROM.get(320, aggTv_config_saved);
  EEPROM.get(330, setPoint_config_saved);
  EEPROM.get(340, brewtime_config_saved);
  EEPROM.get(350, preinfusion_config_saved);
  EEPROM.get(360, preinfusionpause_config_saved);
  EEPROM.get(380, starttemp_config_saved);
  EEPROM.get(390, aggoKp_config_saved);
  EEPROM.get(400, aggoTn_config_saved);
  EEPROM.get(410, aggoTv_config_saved);
  EEPROM.get(430, brewDetectionSensitivity_config_saved);
  EEPROM.get(440, steadyPower_config_saved);
  EEPROM.get(450, steadyPowerOffset_config_saved);
  EEPROM.get(460, steadyPowerOffset_Time_config_saved);
  EEPROM.get(470, burstPower_config_saved);

  //use userConfig.h value if if differs from *_config_saved
  if (AGGKP != aggKp_config_saved) { aggKp = AGGKP; EEPROM.put(300, aggKp); }
  if (AGGTN != aggTn_config_saved) { aggTn = AGGTN; EEPROM.put(310, aggTn); }
  if (AGGTV != aggTv_config_saved) { aggTv = AGGTV; EEPROM.put(320, aggTv); }
  if (AGGOKP != aggoKp_config_saved) { aggoKp = AGGOKP; EEPROM.put(390, aggoKp); }
  if (AGGOTN != aggoTn_config_saved) { aggoTn = AGGOTN; EEPROM.put(400, aggoTn); }
  if (AGGOTV != aggoTv_config_saved) { aggoTv = AGGOTV; EEPROM.put(410, aggoTv); }
  if (SETPOINT != setPoint_config_saved) { setPoint = SETPOINT; EEPROM.put(330, setPoint); DEBUG_print("EEPROM: setPoint (%0.2f) is read from userConfig.h\n", setPoint); }
  if (BREWTIME != brewtime_config_saved) { brewtime = BREWTIME; EEPROM.put(340, brewtime); DEBUG_print("EEPROM: brewtime (%0.2f) is read from userConfig.h\n", brewtime); }
  if (PREINFUSION != preinfusion_config_saved) { preinfusion = PREINFUSION; EEPROM.put(350, preinfusion); }
  if (PREINFUSION_PAUSE != preinfusionpause_config_saved) { preinfusionpause = PREINFUSION_PAUSE; EEPROM.put(360, preinfusionpause); }
  if (STARTTEMP != starttemp_config_saved) { starttemp = STARTTEMP; EEPROM.put(380, starttemp); }
  if (BREWDETECTION_SENSITIVITY != brewDetectionSensitivity_config_saved) { brewDetectionSensitivity = BREWDETECTION_SENSITIVITY; EEPROM.put(430, brewDetectionSensitivity); }
  if (STEADYPOWER != steadyPower_config_saved) { steadyPower = STEADYPOWER; EEPROM.put(440, steadyPower); }
  if (STEADYPOWER_OFFSET != steadyPowerOffset_config_saved) { steadyPowerOffset = STEADYPOWER_OFFSET; EEPROM.put(450, steadyPowerOffset); }
  if (STEADYPOWER_OFFSET_TIME != steadyPowerOffset_Time_config_saved) { steadyPowerOffset_Time = STEADYPOWER_OFFSET_TIME; EEPROM.put(460, steadyPowerOffset_Time); }
  //if (BURSTPOWER != burstPower_config_saved) { burstPower = BURSTPOWER; EEPROM.put(470, burstPower); }
  
  //save latest values to eeprom
  //EEPROM.begin(1024);
  if ( aggKp != aggKp_latest_saved) EEPROM.put(0, aggKp);
  if ( aggTn != aggTn_latest_saved) EEPROM.put(10, aggTn);
  if ( aggTv != aggTv_latest_saved) EEPROM.put(20, aggTv);
  if ( setPoint != setPoint_latest_saved) { EEPROM.put(30, setPoint); DEBUG_print("EEPROM: setPoint (%0.2f) is saved\n", setPoint); }
  if ( brewtime != brewtime_latest_saved) { EEPROM.put(40, brewtime); DEBUG_print("EEPROM: brewtime (%0.2f) is saved (previous:%0.2f)\n", brewtime, brewtime_latest_saved); }
  if ( preinfusion != preinfusion_latest_saved) EEPROM.put(50, preinfusion);
  if ( preinfusionpause != preinfusionpause_latest_saved) EEPROM.put(60, preinfusionpause);
  if ( starttemp != starttemp_latest_saved) EEPROM.put(80, starttemp);
  if ( aggoKp != aggoKp_latest_saved) EEPROM.put(90, aggoKp);
  if ( aggoTn != aggoTn_latest_saved) EEPROM.put(100, aggoTn);
  if ( aggoTv != aggoTv_latest_saved) EEPROM.put(110, aggoTv);
  if ( brewDetectionSensitivity != brewDetectionSensitivity_latest_saved) EEPROM.put(130, brewDetectionSensitivity);
  if ( steadyPower != steadyPower_latest_saved) { EEPROM.put(140, steadyPower); DEBUG_print("EEPROM: steadyPower (%0.2f) is saved (previous:%0.2f)\n", steadyPower, steadyPower_latest_saved); }
  if ( steadyPowerOffset != steadyPowerOffset_latest_saved) EEPROM.put(150, steadyPowerOffset);
  if ( steadyPowerOffset_Time != steadyPowerOffset_Time_latest_saved) EEPROM.put(160, steadyPowerOffset_Time);
  if ( burstPower != burstPower_latest_saved) EEPROM.put(170, burstPower);
  
  EEPROM.commit();
  DEBUG_print("EEPROM: sync_eeprom() finished.\n");
}

/***********************************
 * SETUP()
 ***********************************/
void setup() {
  DEBUGSTART(115200);
  Debug.begin(hostname, Debug.DEBUG);
  Debug.setResetCmdEnabled(true); // Enable the reset command
  Debug.showProfiler(true); // Profiler (Good to measure times, to optimize codes)
  Debug.showColors(true); // Colors
  Debug.setSerialEnabled(true); // log to Serial also

  /********************************************************
    Define trigger type
  ******************************************************/
  if (triggerType)
  {
    relayON = HIGH;
    relayOFF = LOW;
  } else {
    relayON = LOW;
    relayOFF = HIGH;
  }

  /********************************************************
    Ini Pins
  ******************************************************/
  pinMode(pinRelayVentil, OUTPUT);
  digitalWrite(pinRelayVentil, relayOFF);
  pinMode(pinRelayPumpe, OUTPUT);
  digitalWrite(pinRelayPumpe, relayOFF);
  pinMode(pinRelayHeater, OUTPUT);
  digitalWrite(pinRelayHeater, LOW);
  #ifdef BREW_READY_LED
  pinMode(pinLed, OUTPUT);
  digitalWrite(pinLed, LOW);
  #endif

  if (Display == 2) {
    /********************************************************
      DISPLAY 128x64
    ******************************************************/
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize with the I2C addr 0x3C (for the 128x64)  for AZ Deliv. Display
    //display.begin(SSD1306_SWITCHCAPVCC, 0x3D);  // initialize with the I2C addr 0x3D (for the 128x64)
    display.clearDisplay();
  }
  DEBUG_print("\nVersion: %s\n", sysVersion);
  displaymessage("rancilio", sysVersion, "", "");
  delay(1000);

  /********************************************************
     BLYNK & Fallback offline
  ******************************************************/
  if (!force_offline) {
    WiFi.hostname(hostname);
    unsigned long started = millis();
    //displaymessage("rancilio", "1: Connect Wifi to:", ssid, "");

    /* Explicitly set the ESP8266 to be a WiFi-client, otherwise, it by default,
      would try to act as both a client and an access-point and could cause
      network-issues with your other WiFi-devices on your WiFi-network. */
    //WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.mode(WIFI_STA);
    #ifdef STATIC_IP
    IPAddress STATIC_IP; //ip(192, 168, 10, 177);
    IPAddress STATIC_GATEWAY; //gateway(192, 168, 10, 1);
    IPAddress STATIC_SUBNET; //subnet(255,255,255,0);
    WiFi.config(ip, gateway, subnet);
    #endif
    WiFi.begin(ssid, pass);
    DEBUG_print("Connecting to WIFI with SID %s ...\n", ssid);
    // wait up to 10 seconds for connection
    while (!wifi_working() && (millis() < started + 20000))
    {
      yield();    //Prevent Watchdog trigger
    }

    if (!wifi_working()) {
      ERROR_print("Cannot connect to WIFI %s. Disabling WIFI\n", ssid);
      if (DISABLE_SERVICES_ON_STARTUP_ERRORS) {
        force_offline = true;
        mqtt_disabled_temporary = true;
        blynk_disabled_temporary = true;
        lastWifiConnectionAttempt = millis();
      }
      displaymessage("rancilio", "Cannot connect to Wifi:", ssid, "");
      delay(1000);
    } else {
      DEBUG_print("IP address: %s\n", WiFi.localIP().toString().c_str());
      //displaymessage("rancilio", "2: Wifi connected, ", "try mqtt/Blynk   ", "");

      // Connect to MQTT-Service
      if (MQTT_ENABLE) {
        snprintf(topic_will, sizeof(topic_will), "%s%s/%s", mqtt_topic_prefix, hostname, "will");
        snprintf(topic_set, sizeof(topic_set), "%s%s/%s", mqtt_topic_prefix, hostname, "set");
        mqtt_client.setServer(mqtt_server_ip, mqtt_server_port);
        mqtt_client.setCallback(mqtt_callback);
        if (!mqtt_reconnect(true)) {
          if (DISABLE_SERVICES_ON_STARTUP_ERRORS) mqtt_disabled_temporary = true;
          ERROR_print("Cannot connect to MQTT. Disabling...\n");
          displaymessage("rancilio", "Cannot connect to MQTT:", "", "");
          delay(1000);
        }
      }

      if (BLYNK_ENABLE) {
        DEBUG_print("Connecting to Blynk ...\n");
        Blynk.config(auth, blynkaddress, blynkport) ;
        if (!Blynk.connect(5000)) {
          if (DISABLE_SERVICES_ON_STARTUP_ERRORS) blynk_disabled_temporary = true;
          ERROR_print("Cannot connect to Blynk. Disabling...\n");
          displaymessage("rancilio", "Cannot connect to Blynk:", "", "");
          delay(1000);
        } else {
          //displaymessage("rancilio", "3: Blynk connected", "sync all variables...", "");
          DEBUG_print("Blynk is online, get latest values\n");
          unsigned long started = millis();
          while (blynk_working() && (millis() < started + 2000))
          {
            Blynk.run();
          }
        }
      }  
    }
  } else {
    DEBUG_print("Staying offline due to force_offline=1\n");
  }

  /********************************************************
   * READ/SAVE EEPROM
   *  get latest values from EEPROM if blynk is not working/enabled. 
   *  Additionally this function honors changed values in userConfig.h (changed values have priority)
  ******************************************************/
  sync_eeprom(!blynk_working());

  DEBUG_print("Active settings:\n");
  DEBUG_print("aggKp: %0.2f | aggTn: %0.2f | aggTv: %0.2f\n", aggKp, aggTn, aggTv);
  DEBUG_print("aggoKp: %0.2f | aggoTn: %0.2f | aggoTv: %0.2f\n", aggoKp, aggoTn, aggoTv);
  DEBUG_print("setPoint: %0.2f | starttemp: %0.2f | brewDetectionSensitivity: %0.2f\n", setPoint, starttemp, brewDetectionSensitivity);
  DEBUG_print("brewtime: %0.2f | preinfusion: %0.2f | preinfusionpause: %0.2f\n", brewtime, preinfusion, preinfusionpause);
  DEBUG_print("steadyPower: %0.2f | steadyPowerOffset: %0.2f | steadyPowerOffset_Time: %d\n", steadyPower, steadyPowerOffset, steadyPowerOffset_Time);
  DEBUG_print("burstPower: %0.2f\n", burstPower);
  /********************************************************
     OTA
  ******************************************************/
  if (ota && !force_offline ) {
    //wifi connection is done during blynk connection
    ArduinoOTA.setHostname(OTAhost);  //  Device name for OTA
    ArduinoOTA.setPassword(OTApass);  //  Password for OTA
    ArduinoOTA.begin();
  }

  /********************************************************
    movingaverage ini array
  ******************************************************/
  for (int thisReading = 0; thisReading < numReadings; thisReading++) {
    readingstemp[thisReading] = 0;
    readingstime[thisReading] = 0;
  }

  /********************************************************
     TEMP SENSOR
  ******************************************************/
  //displaymessage("rancilio", "Init. vars", "", "");
  if (TempSensor == 1) {
    sensors.begin();
    sensors.getAddress(sensorDeviceAddress, 0);
    sensors.setResolution(sensorDeviceAddress, 10) ;
    while (true) {
      sensors.requestTemperatures();
      previousInput = sensors.getTempCByIndex(0);
      delay(400);
      sensors.requestTemperatures();
      Input = sensors.getTempCByIndex(0);
      if (checkSensor(Input, previousInput)) {
        updateTemperatureHistory(Input);
        break;
      }
      displaymessage("rancilio", "Temp. sensor defect", "", "");
      ERROR_print("Temp. sensor defect. Cannot read consistant values. Retrying\n");
      delay(400);
    }
  }

  if (TempSensor == 2) {
    while (true) {
      temperature = 0;
      Sensor1.getTemperature(&temperature);
      previousInput = Sensor1.calc_Celsius(&temperature);
      delay(400);
      temperature = 0;
      Sensor1.getTemperature(&temperature);
      Input = Sensor1.calc_Celsius(&temperature);
      if (checkSensor(Input, previousInput)) {
        updateTemperatureHistory(Input);
        break;
      }
      displaymessage("rancilio", "Temp. sensor defect", "", "");
      ERROR_print("Temp. sensor defect. Cannot read consistant values. Retrying\n");
      delay(400);
    }
  }

  /********************************************************
     Ini PID
  ******************************************************/
  bPID.SetSampleTime(windowSize);
  bPID.SetOutputLimits(0, windowSize);
  bPID.SetMode(AUTOMATIC);

  /********************************************************
     REST INIT()
  ******************************************************/
  //Initialisation MUST be at the very end of the init(), otherwise the time comparison in loop() will have a big offset
  unsigned long currentTime = millis();
  previousMillistemp = currentTime;
  previousMillistemp2 = currentTime;;
  previousMillisDisplay = currentTime + 50;
  previousMillisBlynk = currentTime + 800;
  lastMQTTStatusReportTime = currentTime + 300;

  /********************************************************
    Timer1 ISR - Initialisierung
    TIM_DIV1 = 0,   //80MHz (80 ticks/us - 104857.588 us max)
    TIM_DIV16 = 1,  //5MHz (5 ticks/us - 1677721.4 us max)
    TIM_DIV256 = 3  //312.5Khz (1 tick = 3.2us - 26843542.4 us max)
  ******************************************************/
  //delay(35);
  timer1_isr_init();
  timer1_attachInterrupt(onTimer1ISR);
  timer1_enable(TIM_DIV16, TIM_EDGE, TIM_SINGLE);
  timer1_write(50000); // set interrupt time to 10ms
  DEBUG_print("End of setup()\n");
}

/********************************************************
  Displayausgabe
*****************************************************/
char float2stringChar[8];
char* float2string(float in) {
  snprintf(float2stringChar, sizeof(float2stringChar), "%3d.%1d", (int) in, (int) (in*10) % 10);
  return float2stringChar;
}
void displaymessage(String logo, String displaymessagetext, String displaymessagetext2, String displaymessagetext3) {
  if (Display == 2 && !sensorError) {
    /********************************************************
       DISPLAY AUSGABE
    ******************************************************/
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.drawRoundRect(0, 0, 128, 64, 1, WHITE);
    if (logo == "rancilio") {
      display.drawBitmap(41,2, rancilio_logo_bits,rancilio_logo_width, rancilio_logo_height, WHITE);
      display.setCursor(2, 47);
      display.println(displaymessagetext);
      display.println(displaymessagetext2);
      display.print(displaymessagetext3);
      
    } else if (logo == "steam") {
      display.drawBitmap(83,20, steam_logo_bits, steam_logo_width, steam_logo_height, WHITE);
      
      display.setCursor(8, 8);
      display.setTextSize(2);
      display.setTextColor(WHITE);
      display.print(float2string(Input));   //temperature line
      display.setTextSize(1);
      display.print(" ");
      display.print((char)247);
      display.println("C");
      
      display.setCursor(10, 25); 
      display.println(displaymessagetext);  // description line

    } else if (logo == "brew") {
      display.drawBitmap(83,20, brew_logo_bits, brew_logo_width, brew_logo_height, WHITE);
      
      display.setCursor(8, 8);
      display.setTextSize(2);
      display.setTextColor(WHITE);
      display.print(float2string(Input)); //temperature line
      display.setTextSize(1);
      display.print(" ");
      display.print((char)247);
      display.println("C");

      display.setCursor(25, 25); 
      display.print(setPoint, 1);      //setPoint line
      display.print(" ");
      display.print((char)247);
      display.println("C");

      display.setCursor(10, 55);       // preinfusion line
      display.print(bezugsZeit / 1000);
      if (ONLYPID == 0) {
        display.print("/");
        display.print(totalbrewtime / 1000); 
      }
      display.print(" ");
      display.println("sec.");
    }
    
    display.display();
  }
}

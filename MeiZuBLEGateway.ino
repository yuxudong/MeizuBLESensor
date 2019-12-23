#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <WebServer.h> 
#include <WiFiManager.h>
#include <MQTT.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include "BLEDevice.h"

const int LED_PIN = 2;
const int button = 18;         //gpio
hw_timer_t *timer = NULL;
unsigned long start_time = 0;
bool needconfig = false;
bool firststart = false;

static BLEUUID serviceUUID("000016f0-0000-1000-8000-00805f9b34fb");
static BLEUUID charUUID("000016f2-0000-1000-8000-00805f9b34fb");

const char SETUP_FORM[] PROGMEM  = "<form method='get' action='/reset'><button>Configure System</button></form><br/>";
const char UPDATE_FORM[] PROGMEM  = "<form method='post' action='/update' enctype='multipart/form-data'>Firmware Update:<input type='file' name='update'><input type='submit' name='submit' value='Update'></form>";

static BLEClient* myClient;
//char ssid[50] = "";
//char wpapass[50] = "";
char bleaddrs[512] = "";
char mqtt_server[40];
char mqtt_port[6] = "1883";
char mqtt_user[50] = "pi";
char mqtt_pass[50] = "raspiberry";
double wdtTimeout = 1200000;  //time in ms to trigger the watchdog
double delayTime  =  600000;

WebServer server(80);
WiFiManager manager;
MQTTClient mqttclient;
WiFiClient wificlient;

void IRAM_ATTR resetModule() {
  Serial.println("Watchdog timeup.");
  ets_printf("reboot\n");
  esp_restart();
}

void saveConfig() {
  Serial.println("saving config");
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["mqtt_server"] = mqtt_server;
  json["mqtt_port"] = mqtt_port;
  json["mqtt_user"] = mqtt_user;
  json["mqtt_pass"] = mqtt_pass;
  json["bleaddrs"] = bleaddrs;
  json["wdtTimeout"] = String(wdtTimeout);
  json["delayTime"] = String(delayTime);
  json["needconfig"] = String(needconfig);
  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("failed to open config file for writing");
  }

  json.printTo(Serial);
  json.printTo(configFile);
  configFile.close();
}

void saveConfigCallback () {
  Serial.println("Should save config");
  saveConfig();
}

void print(std::string str) {
  char *p = (char *)str.c_str();
  for (int i = 0; i < strlen(p); i++) {
    Serial.print(" ");
    if (p[i] < 16) Serial.print("0");
    Serial.print(p[i], HEX);
  }
}

void mountFS() {
  Serial.println("mounting FS...");

  if (SPIFFS.begin(true)) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          strcpy(mqtt_server, json["mqtt_server"]);
          strcpy(mqtt_port, json["mqtt_port"]);
          strcpy(mqtt_user, json["mqtt_user"]);
          strcpy(mqtt_pass, json["mqtt_pass"]);
          wdtTimeout =  json.get<int>("wdtTimeout");
          delayTime =  json.get<int>("delayTime");
          strcpy(bleaddrs, json["bleaddrs"]);
          needconfig = bool(json["needconfig"]);
        } else {
          Serial.println("failed to load json config");
        }
        configFile.close();
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
}

void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  //if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

void wifisetup() {
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 6);
  WiFiManagerParameter custom_mqtt_user("user", "mqtt user", mqtt_user, 50);
  WiFiManagerParameter custom_mqtt_pass("pass", "mqtt pass", mqtt_pass, 50);
  char c[100];
  dtostrf(wdtTimeout,20,0,c);
  WiFiManagerParameter custom_wdtTimeout("wdtTimeout", "Watchdog timeout", c, 50);
  dtostrf(delayTime,20,0,c);
  WiFiManagerParameter custom_delayTimeout("delayTime", "Delay Time", c, 50);
  WiFiManagerParameter custom_bleaddrs("bleaddrs", "BLE Addresses", bleaddrs, 512);
  
  manager.setSaveConfigCallback(saveConfigCallback);
  manager.setDebugOutput(false);
  manager.setAPCallback(configModeCallback);
  manager.addParameter(&custom_mqtt_server);
  manager.addParameter(&custom_mqtt_port);
  manager.addParameter(&custom_mqtt_user);
  manager.addParameter(&custom_mqtt_pass);
  manager.addParameter(&custom_wdtTimeout);
  manager.addParameter(&custom_delayTimeout);
  manager.addParameter(&custom_bleaddrs);

  if (needconfig) {
    manager.startConfigPortal();
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());
    strcpy(mqtt_user, custom_mqtt_user.getValue());
    strcpy(mqtt_pass, custom_mqtt_pass.getValue());
    wdtTimeout=String(custom_wdtTimeout.getValue()).toDouble();
    delayTime=String(custom_delayTimeout.getValue()).toDouble();
    strcpy(bleaddrs, custom_bleaddrs.getValue());
    needconfig = false;
    saveConfig();
  }

  if (!manager.autoConnect()) {
    manager.startConfigPortal();
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());
    strcpy(mqtt_user, custom_mqtt_user.getValue());
    strcpy(mqtt_pass, custom_mqtt_pass.getValue());
    wdtTimeout=String(custom_wdtTimeout.getValue()).toDouble();
    delayTime=String(custom_delayTimeout.getValue()).toDouble();
    strcpy(bleaddrs, custom_bleaddrs.getValue());
    needconfig = false;
    saveConfig();
  }

  Serial.print("local ip: ");
  Serial.println(WiFi.localIP());

  if (mqttclient.connected()) mqttclient.disconnect();
  mqttclient.begin(mqtt_server, (int)String(mqtt_port).toInt(), wificlient);
  if (!mqttclient.connect(mqtt_server, mqtt_user, mqtt_pass)) {
    manager.startConfigPortal();
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());
    strcpy(mqtt_user, custom_mqtt_user.getValue());
    strcpy(mqtt_pass, custom_mqtt_pass.getValue());
    wdtTimeout=String(custom_wdtTimeout.getValue()).toDouble();
    delayTime=String(custom_delayTimeout.getValue()).toDouble();
    strcpy(bleaddrs, custom_bleaddrs.getValue());
    needconfig = false;
    saveConfig();
  }
  needconfig = false;
}

std::vector<std::string> split(const char *s, const char *delim)
{
  std::vector<std::string> result;
  if (s && strlen(s))
  {
    int len = strlen(s);
    char *src = new char[len + 1];
    strcpy(src, s);
    src[len] = '\0';
    char *tokenptr = strtok(src, delim);
    while (tokenptr != NULL)
    {
      std::string tk = tokenptr;
      result.emplace_back(tk);
      tokenptr = strtok(NULL, delim);
    }
    delete[] src;
  }
  return result;
}

std::string& StringTrim(std::string &str)
{
  if (str.empty()) {
    return str;
  }
  str.erase(0, str.find_first_not_of(" "));
  str.erase(str.find_last_not_of(" ") + 1);
  return str;
}

void watchdog_setup() {
  //pinMode(button, INPUT_PULLUP);                    //init control pin
  timer = timerBegin(0, 80, true);                  //timer 0, div 80
  timerAttachInterrupt(timer, &resetModule, true);  //attach callback
  timerAlarmWrite(timer, wdtTimeout * 1000, true); //set time in us
  timerAlarmEnable(timer);                          //enable interrupt
}

bool BLEProcess(BLEAddress blea) {
  Serial.print("Connecting to ");
  Serial.print(blea.toString().c_str());
  if (myClient->connect(blea)) {
    Serial.println(" Done.");
    BLERemoteService* pRemoteService = myClient->getService(serviceUUID);
    if (pRemoteService == nullptr) {
      Serial.print("Failed to find our service UUID: ");
      Serial.println(serviceUUID.toString().c_str());
      myClient->disconnect();
      return false;
    }
    Serial.println(" - Found our service");
    BLERemoteCharacteristic* pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
    if (pRemoteCharacteristic == nullptr) {
      Serial.print("Failed to find our characteristic UUID: ");
      Serial.println(charUUID.toString().c_str());
      myClient->disconnect();
      return false;
    }
    Serial.println(" - Found our characteristic");

    if (pRemoteCharacteristic->canRead()) {
      pRemoteCharacteristic->writeValue("\x55\x03\x08\x11", true);
      std::string s = pRemoteCharacteristic->readValue();
      int s_length = s.length();
      Serial.print("    The characteristic value was: ");
      print(s);
      Serial.print(" length ");
      Serial.println(String(s_length));
      if (s_length != 8) {
        Serial.println("Data error.");
        return false;
      }
      float Temperature = float(s[5] << 8 | s[4]) / 100.0;
      float Humidity = float(s[7] << 8 | s[6]) / 100.0;
      pRemoteCharacteristic->writeValue("\x55\x03\x01\x10", true);
      s = pRemoteCharacteristic->readValue();
      s_length = s.length();
      Serial.print("    The characteristic value was: ");
      print(s);
      Serial.print(" length ");
      Serial.println(String(s_length));
      if (s_length != 5) {
        Serial.println("Data error.");
        return false;
      }
      float Battery = float(s[4]) / 10.0;
      Serial.print("Temperature: ");
      Serial.print(Temperature);
      Serial.print(" Humidity: ");
      Serial.print(Humidity);
      Serial.print(" Battery: ");
      Serial.println(Battery);
      if (Temperature > 100 || Humidity > 100 || Battery > 3) {
        Serial.println("Data error.");
        return false;
      }
      String jsonstr = "";
      DynamicJsonBuffer jsonBuffer;
      JsonObject& json = jsonBuffer.createObject();
      json["Temperature"] = Temperature;
      json["Humidity"] = Humidity;
      json["Battery"] = Battery;
      json.printTo(jsonstr);
      //Serial.println(jsonstr);
      String sblea = String(blea.toString().c_str());
      sblea.replace(":", "");
      //std::string topic = std::string("/Meizu/") + std::string(  blea.toString().c_str());
      std::string topic = std::string("/Meizu/") + std::string(sblea.c_str());
      if (mqttclient.connected()) mqttclient.publish(topic.c_str(), jsonstr);
    }
    myClient->disconnect();
    return true;
  } else {
    Serial.println(" Failed.");
  }
  return false;
}

void IRAM_ATTR isr() {
  needconfig = true;
  unsigned long current_time = millis();
  if ( current_time - start_time > 2000 ) {
    Serial.println("");
    Serial.println("Button has been pressed.");
    handle_reset();
    //wifisetup();
    start_time = current_time;
  }
}

void led_flash(int pin) {
  digitalWrite(pin, HIGH);
  delay(500);
  digitalWrite(pin, LOW);
  delay(500);
}

void handle_root() {
  String page = FPSTR(ESP32_HTTP_HEAD);
  page.replace("{v}", "Setup");
  page += FPSTR(ESP32_HTTP_SCRIPT);
  page += FPSTR(ESP32_HTTP_STYLE);
  page += FPSTR(ESP32_HTTP_HEAD_END);
  page += F("<h3>ESP32 Setup</h3>");
  page += FPSTR(SETUP_FORM);
  page += FPSTR(UPDATE_FORM);
  page += FPSTR(ESP32_HTTP_END);

  server.sendHeader("Content-Length", String(page.length()));
  server.send(200, "text/html", page);
}

void handle_reset() {
  String page = FPSTR(ESP32_HTTP_HEAD);
  page.replace("{v}", "Reset");
  page += FPSTR(ESP32_HTTP_SCRIPT);
  page += FPSTR(ESP32_HTTP_STYLE);
  page += FPSTR(ESP32_HTTP_HEAD_END);
  page += F("<h3>The Module is set to Ap, IP: 192.168.4.1</h3>");
  page += FPSTR(ESP32_HTTP_END);

  server.sendHeader("Content-Length", String(page.length()));
  server.send(200, "text/html", page);
  needconfig = true;
  saveConfig();
  server.close();
  ESP.restart();
  //wifisetup();
}

void setup() {
  watchdog_setup();
  Serial.begin(115200);
  pinMode(button, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(button), isr, FALLING);
  Serial.print("Button interrrupt is ");
  Serial.print(String(digitalPinToInterrupt(button)));
  Serial.println("");
  mountFS();
  wifisetup();
  server.on ( "/", handle_root);
  server.on ( "/reset", handle_reset);
  server.on ( "/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.setDebugOutput(true);
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin()) { //start with max available size
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
      Serial.setDebugOutput(false);
    } else {
      Serial.printf("Update Failed Unexpectedly (likely broken connection): status=%d\n", upload.status);
    }
  });
  server.begin();
  ArduinoOTA.begin();
  Serial.println("Starting Arduino BLE Client application...");
  BLEDevice::init("");
  myClient = new BLEClient();
  start_time = millis();
  firststart = true;
}

void loop() {
  timerWrite(timer, 0); //reset timer (feed watchdog)
  unsigned long current_time = millis();
  mqttclient.loop();
  server.handleClient();
  ArduinoOTA.handle();
  if ( current_time - start_time >= delayTime || firststart) {
    std::vector<std::string> vec = split(bleaddrs, ",");
    size_t len = vec.size();
    Serial.println("");
    Serial.println("Total: " + String(len) + " Devices");
    for (size_t i = 0; i < len; i ++) {
      Serial.println(StringTrim(vec[i]).c_str());
    }
    if (!mqttclient.connected()) {
      Serial.print("Check WIFI");
      while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(1000);
      }
      Serial.println(" Done.");
      Serial.print("Connect to MqttServer");
      while (!mqttclient.connect(mqtt_server, mqtt_user, mqtt_pass)) {
        Serial.print(".");
        delay(1000);
      }
      Serial.println(" Done.");
    }
    for (size_t i = 0; i < len; i ++) {
      digitalWrite(LED_PIN, HIGH);
      BLEProcess(BLEAddress( StringTrim(vec[i]).c_str()));
      digitalWrite(LED_PIN, LOW);
      mqttclient.loop();
    }
    //std::string BLEs[] = {"68:3E:34:CC:E5:5A", "68:3E:34:CC:DF:D2"};
    //Serial.println("Total: " + String(sizeof(BLEs) / sizeof(BLEs[0])) + " Devices");
    //for (int i = 0; i < (sizeof(BLEs) / sizeof(BLEs[0])); i++) {
    //  BLEAddress blea = BLEAddress(BLEs[i]);
    //  BLEProcess(blea);
    //}
    start_time = millis();
  }
  firststart = false;
  Serial.print(".");
  led_flash(LED_PIN);
}

#include <Arduino.h>

#include <BasicTimer.h>
#include <Preferences.h>

#include <WiFi.h>
#include <espMqttClient.h>
#include <WiFiManager.h>

#include <Preferences.h>
Preferences preferences;

#include "FS.h"
#include "SPIFFS.h"
#define FORMAT_SPIFFS_IF_FAILED true

bool wm_nonblocking = true; // change to true to use non blocking

WiFiManager wm; // global wm instance
WiFiManagerParameter mqtthost_param; // global param ( for non blocking w params )
WiFiManagerParameter mqttport_param; // global param ( for non blocking w params )
WiFiManagerParameter mqttusr_param; // global param ( for non blocking w params )
WiFiManagerParameter mqttpwd_param; // global param ( for non blocking w params )
WiFiManagerParameter rootca_param; // global param ( for non blocking w params )
WiFiManagerParameter rootca_helper_param;

espMqttClientSecure mqttClient(espMqttClientTypes::UseInternalTask::NO);
static TaskHandle_t taskHandle;
bool reconnectMqtt = false;
uint32_t lastReconnect = 0;
uint32_t lastMillis = 0;
uint32_t millisDisconnect = 0;

BasicBlinker basicBlinker(500);
BasicTimer constructedTimer;
BasicTimer delayTimer;
BasicTimer BellTimer(2000);

int IOLOCK = 1;
int MUTEPIN_JMP = 2;
int IOBELL = 3;
int MUTEPIN = 0;
int IORST = 10;
int IOLED = 8;
int call_url_length = 400;

bool autounlock;

String call_url;
String struid;
String strvolt;

uint32_t chipID;

String APSSID;
const char *APPWD = "password";

const int led = IOLED;

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println();

  if(!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED)){
    Serial.println("SPIFFS Mount Failed");
    return;
  }

  #ifdef ESP8266
  
    chipID = ESP.getChipId();
  
  #endif

  #ifdef ESP32
  
    uint64_t macAddress = ESP.getEfuseMac();
    uint64_t macAddressTrunc = macAddress << 40;
    chipID = macAddressTrunc >> 40;
  
  #endif

  struid = String(chipID, HEX);
  strvolt = "N/A";

  APSSID = "Intercom-" + struid;

  pinMode(IOLOCK, OUTPUT);
  pinMode(IOLED, OUTPUT);
  pinMode(IOBELL, INPUT);
  pinMode(MUTEPIN, OUTPUT);
  pinMode(IORST, INPUT_PULLUP);
  pinMode(MUTEPIN_JMP, INPUT_PULLUP);

  uint8_t MUTEPIN_JMP_STATE = !digitalRead(MUTEPIN_JMP);

  digitalWrite(IOLOCK, LOW);
  digitalWrite(MUTEPIN, MUTEPIN_JMP_STATE);

  updateLED(false);

  basicBlinker.setBlinkTime(200);
  basicBlinker.reset();
  basicBlinker = true;

  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
  WiFi.onEvent(WiFiEvent);

  if(wm_nonblocking) wm.setConfigPortalBlocking(false);

  preferences.begin("mqtt-settings", false);
  char mqtthost[200] = "";
  uint16_t mqttport = preferences.getInt("mqttport");
  char mqttusr[20] = "";
  char mqttpwd[25] = "";
  memcpy(mqtthost, preferences.getString("mqtthost").c_str(), strlen(preferences.getString("mqtthost").c_str())+1);
  memcpy(mqttusr, preferences.getString("mqttusr").c_str(), strlen(preferences.getString("mqttusr").c_str())+1);
  memcpy(mqttpwd, preferences.getString("mqttpwd").c_str(), strlen(preferences.getString("mqttpwd").c_str())+1);
  preferences.end();
  char rootca[5000] = "";
  memcpy(rootca, readFile(SPIFFS, "/cert.pem").c_str(), strlen(readFile(SPIFFS, "/cert.pem").c_str())+1);

  WiFiManagerParameter h2_param("<br/><h2>MQTT Broker Settings</h2>");
  new (&mqtthost_param) WiFiManagerParameter("mqtthost", "Host/IP", mqtthost, 200,"placeholder=\"mqtt.innovutech.com\" ");
  new (&mqttport_param) WiFiManagerParameter("mqttport", "Port", String(mqttport).c_str(), 5,"placeholder=\"8883\" type=\"number\" ") ;
  new (&mqttusr_param) WiFiManagerParameter("mqttusr", "Username", mqttusr, 20,"placeholder=\"username\" ");
  new (&mqttpwd_param) WiFiManagerParameter("mqttpwd", "Password", mqttpwd, 25,"placeholder=\"**********\" type=\"password\" ");
  
  const char *rootca_html = "<br/><hr/><br/><label for='rootca'>Root CA</label><br/><textarea id='rootca' name='rootca' rows='50' cols='50'></textarea>";
  new (&rootca_param) WiFiManagerParameter(rootca_html);
  
  new (&rootca_helper_param) WiFiManagerParameter("rootca_helper", "", rootca, 5000, "type='hidden'");
  
  const char *jscript = "\n" \
  "<script>\n" \
  " document.getElementById('rootca').innerHTML = document.getElementById('rootca_helper').value;\n" \
  "</script>";
  WiFiManagerParameter jscript_param(jscript);

  wm.setDebugOutput(false);
  wm.addParameter(&h2_param);
  wm.addParameter(&mqtthost_param);
  wm.addParameter(&mqttport_param);
  wm.addParameter(&mqttusr_param);
  wm.addParameter(&mqttpwd_param);
  wm.addParameter(&rootca_param);
  wm.addParameter(&rootca_helper_param);
  wm.addParameter(&jscript_param);
  wm.setSaveParamsCallback(saveParamCallback);
  wm.setConfigPortalTimeoutCallback(portalTimeoutCallback);

  std::vector<const char *> menu = {"wifi","info","param","sep","restart","exit"};
  wm.setMenu(menu);
  wm.setClass("invert");
  wm.setConfigPortalTimeout(30);
  wm.setAPClientCheck(true);

  wm.setHostname(APSSID.c_str());

  //mqttClient.setInsecure();
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onSubscribe(onMqttSubscribe);
  mqttClient.onUnsubscribe(onMqttUnsubscribe);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.onPublish(onMqttPublish);
  mqttClient.setCleanSession(true);
  mqttClient.setCACert(rootca);
  mqttClient.setCredentials(mqttusr, mqttpwd);
  mqttClient.setServer(mqtthost, mqttport);

  xTaskCreatePinnedToCore((TaskFunction_t)networkingTask, "mqttclienttask", 5120, nullptr, 1, &taskHandle, 0);

  bool res;
  res = wm.autoConnect(APSSID.c_str(),APPWD);
  while (WiFi.status() != WL_CONNECTED || wm.getConfigPortalActive()) {
    yield();
    wm.process();
    basicBlinker.run();
    updateLED(basicBlinker);
  }

  updateLED(true);
}

String readFile(fs::FS &fs, const char * path) {
    Serial.printf("Reading file: %s\r\n", path);

    File file = fs.open(path);
    if(!file || file.isDirectory()){
        Serial.println("- failed to open file for reading");
        return "";
    }

    //Serial.println("- read from file:");
    String d_buff;
    while(file.available()){
        d_buff += String((char)file.read());
    }
    file.close();
    return d_buff;
}

void writeFile(fs::FS &fs, const char * path, const char * message){
    Serial.printf("Writing file: %s\r\n", path);

    File file = fs.open(path, FILE_WRITE);
    if(!file){
        Serial.println("- failed to open file for writing");
        return;
    }
    if(file.print(message)){
        Serial.println("- file written");
    } else {
        Serial.println("- write failed");
    }
    file.close();
}

void WiFiEvent(WiFiEvent_t event) {
  Serial.printf("[WiFi-event] event: %d\n", event);
  switch(event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.println("WiFi connected");
      Serial.println("IP address: ");
      Serial.println(WiFi.localIP());
      connectToMqtt();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi lost connection");
      break;
    default:
      break;
  }
}

void connectToMqtt() {
  Serial.println("Connecting to MQTT...");
  if (!mqttClient.connect()) {
    reconnectMqtt = true;
    lastReconnect = millis();
    Serial.println("Connecting failed.");
  } else {
    reconnectMqtt = false;
  }
}

void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT.");
  Serial.print("Session present: ");
  Serial.println(sessionPresent);
  
  String unlock_subtopic = "devices/" + APSSID + "/cmd";
  String mute_subtopic = "devices/" + APSSID + "/cmd/mute";
  String autounlock_subtopic = "devices/" + APSSID + "/cmd/autounlock";

  String pubtopic = "devices/announce";

  uint16_t packetIdSub0 = mqttClient.subscribe(unlock_subtopic.c_str(), 0);
  uint16_t packetIdSub1 = mqttClient.subscribe(mute_subtopic.c_str(), 0);
  uint16_t packetIdSub2 = mqttClient.subscribe(autounlock_subtopic.c_str(), 0);
  Serial.print("Subscribing at QoS 0, packetId: ");
  Serial.println(packetIdSub0);
  Serial.println(packetIdSub1);
  Serial.println(packetIdSub2);

  uint16_t packetIdPub0 = mqttClient.publish(pubtopic.c_str(), 0, false, String("{\"name\": \"" + APSSID + "\", \"status\": \"ONLINE\"}").c_str());
  Serial.println("Publishing at QoS 0, packetId: ");
  Serial.println(packetIdPub0);
}

void onMqttDisconnect(espMqttClientTypes::DisconnectReason reason) {
  Serial.printf("Disconnected from MQTT: %u.\n", static_cast<uint8_t>(reason));

  if (WiFi.isConnected()) {
    reconnectMqtt = true;
    lastReconnect = millis();
  }
}

void onMqttSubscribe(uint16_t packetId, const espMqttClientTypes::SubscribeReturncode* codes, size_t len) {
  Serial.println("Subscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
  for (size_t i = 0; i < len; ++i) {
    Serial.print("  qos: ");
    Serial.println(static_cast<uint8_t>(codes[i]));
  }
}

void onMqttUnsubscribe(uint16_t packetId) {
  Serial.println("Unsubscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

void onMqttMessage(const espMqttClientTypes::MessageProperties& properties, const char* topic, const uint8_t* payload, size_t len, size_t index, size_t total) {
  (void) payload;
  const char* data = (char*)payload;
  const char * cmd_topic = String("devices/" + APSSID + "/cmd").c_str();
  const char * mute_topic = String("devices/" + APSSID + "/cmd/mute").c_str();
  const char * autounlock_topic = String("devices/" + APSSID + "/cmd/autounlock").c_str();

  const char * mute_pubtopic = String("devices/" + APSSID + "/mute").c_str();
  const char * autounlock_pubtopic = String("devices/" + APSSID + "/autounlock").c_str();

  if (topic == cmd_topic) {
    if (data == "unlock"){
      unlockDoor();
    } else if (data == "reset") {
      handleReset();
    }
  } else if (topic == mute_topic) {
    int mapped_setting = (data == "1") ? 1 : ((data == "0") ? 0 : -1);

    switch (mapped_setting) {
      case 0:
      case 1:
        {
          bool mapped_state = !mapped_setting;
          digitalWrite(MUTEPIN, mapped_state);
          mqttClient.publish(mute_pubtopic, 0, true, (char*)mapped_state);
          String state = (mapped_setting == 1) ? "MUTED" : "UNMUTED";
          Serial.println(state);
          break;
        }
      default:
        Serial.print("Unrecognised option -> ");
        Serial.println(data);
    }
  } else if (topic == autounlock_topic) {
    int mapped_setting = (data == "1") ? 1 : ((data == "0") ? 0 : -1);

    switch (mapped_setting) {
      case 0:
      case 1:
        {
          autounlock = mapped_setting;
          String state = (mapped_setting == 1) ? "AUTOUNLOCK ON" : "AUTOUNLOCK OFF";
          Serial.println(state);
          break;
        }
      default:
        Serial.print("Unrecognised option -> ");
        Serial.println(data);
    }
  }

  Serial.println("Publish received.");
  Serial.print("  topic: ");
  Serial.println(topic);
  Serial.print("  qos: ");
  Serial.println(properties.qos);
  Serial.print("  dup: ");
  Serial.println(properties.dup);
  Serial.print("  retain: ");
  Serial.println(properties.retain);
  Serial.print("  len: ");
  Serial.println(len);
  Serial.print("  index: ");
  Serial.println(index);
  Serial.print("  total: ");
  Serial.println(total);
  Serial.println(" payload ");
  Serial.print(data);
}

void onMqttPublish(uint16_t packetId) {
  Serial.println("Publish acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

void networkingTask() {
  for (;;) {
    mqttClient.loop();
  }
}

void handleReset() {
  digitalWrite(led, 0);
  udelay(500);
  digitalWrite(led, 1);

  wm.resetSettings();
  ESP.restart();
}

void unlockDoor() {
  digitalWrite(IOLOCK, HIGH);
  digitalWrite(led, 0);
  udelay(500);
  digitalWrite(IOLOCK, LOW);
  digitalWrite(led, 1);
}

void portalTimeoutCallback() {
  updateLED(true);
  blink();

  ESP.deepSleep(0);
}

String getParam(String name){
  //read parameter from Server, for customhmtl input
  String value;
  if(wm.server->hasArg(name)) {
    value = wm.server->arg(name);
  }
  return value;
}

void saveParamCallback(){
  preferences.begin("mqtt-settings", false);

  String mqtthost = getParam("mqtthost");
  String mqttport = getParam("mqttport");
  String mqttusr = getParam("mqttusr");
  String mqttpwd = getParam("mqttpwd");
  String rootca = getParam("rootca");

  if (mqtthost.length() <= 200) {
    preferences.putString("mqtthost", mqtthost);
    mqtthost_param.setValue(mqtthost.c_str() , 200);
  }
  if (mqttport.length() <= 5) {
    preferences.putInt("mqttport", mqttport.toInt());
    mqttport_param.setValue(mqttport.c_str() , 5);
  }
  if (mqttusr.length() <= 20) {
    preferences.putString("mqttusr", mqttusr);
    mqttusr_param.setValue(mqttusr.c_str() , 20);
  }
  if (mqttpwd.length() <= 25) {
    preferences.putString("mqttpwd", mqttpwd);
    mqttpwd_param.setValue(mqttpwd.c_str() , 25);
  }
  if (rootca.length() <= 5000) {
    writeFile(SPIFFS, "/cert.pem", rootca.c_str());
    rootca_helper_param.setValue(rootca.c_str() , 5000);
  }
  preferences.end();
}

void handleMqttReconnect() {
  uint32_t currentMillis = millis();
  uint32_t elapsed = (currentMillis - lastReconnect);

  if (reconnectMqtt && (elapsed > 5000)) {
    connectToMqtt();
  }

  if (currentMillis - millisDisconnect > 60000) {
    millisDisconnect = currentMillis;
    mqttClient.disconnect();
  }
}

void loop() {
  handleMqttReconnect();
  if (digitalRead(IOBELL) == HIGH && BellTimer.hasExpired()) {
    if (autounlock) {
      unlockDoor();
    }

    // wait for WiFi connection
    if ((WiFi.status() == WL_CONNECTED)) {
      callUrl();
    }

    blink();

    BellTimer.begin();

  } else if (digitalRead(IORST) == LOW) {
    bool reset_triggered = true;
    int elapsed = 0;
    constructedTimer.begin(3000);
    while (!constructedTimer.hasExpired()) {
      udelay(10);
      if (digitalRead(IORST) == HIGH) {
        elapsed = constructedTimer.elapsedTime();
        reset_triggered = false;
        break;
      }
      yield();
      //handleMqttReconnect();
    }

    if (reset_triggered) {
      blink();
      wm.resetSettings();
      ESP.restart();
    } else if (elapsed > 50) {
      flash();
      callUrl();
    }
  }
}

void callUrl() {

}

void udelay(int ms) {
  delayTimer.begin(ms);
  while (!delayTimer.hasExpired()) {
    yield();
    //Server.handleClient();
  }
}

void blink() {
  updateLED(false);
  udelay(200);
  updateLED(true);
  udelay(200);
  updateLED(false);
  udelay(200);
  updateLED(true);
}

void flash() {
  updateLED(false);
  udelay(200);
  updateLED(true);
}

void updateLED(bool onOrOff) {
  if (onOrOff == true) {
    digitalWrite(IOLED, HIGH);
  } else {
    digitalWrite(IOLED, LOW);
  }
}

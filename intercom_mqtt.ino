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
bool autounlock;
uint8_t MUTEPIN_JMP_STATE;

BasicBlinker basicBlinker(500);
BasicTimer constructedTimer;
BasicTimer delayTimer;
BasicTimer BellTimer(2000);

const int IOLOCK = 1;
const int MUTEPIN_JMP = 2;
const int IOBELL = 3;
const int MUTEPIN = 0;
const int IORST = 10;
const int IOLED = 8;
const int led = IOLED;

uint64_t macAddress = ESP.getEfuseMac();
uint64_t macAddressTrunc = macAddress << 40;
uint32_t chipID = macAddressTrunc >> 40;

String uid = String(chipID, HEX);

String friendly_name = "Intercom";
String APSSID = friendly_name + "-" + uid;
String APPWD = "AP_PASSWORD";
String discovery_prefix = "homeassistant";
String topic_prefix = "devices";

String will_pubtopic = topic_prefix + "/" + uid + "/status";
String mute_pubtopic = topic_prefix + "/" + uid + "/mute";
String autounlock_pubtopic = topic_prefix + "/" + uid + "/autounlock";
String ring_pubtopic = topic_prefix + "/" + uid + "/events";

String birth_subtopic = discovery_prefix + "/status";
String cmd_subtopic = topic_prefix + "/" + uid + "/cmd";
String mute_subtopic = topic_prefix + "/" + uid + "/cmd/mute";
String autounlock_subtopic = topic_prefix + "/" + uid + "/cmd/autounlock";

const char * configtopic(String compoment, String identifier) {
  return String(discovery_prefix + "/" + compoment + "/" + identifier + uid + "/config").c_str();
}

String event_payload =
"{"
" \"name\": \"Intercom RING Event\","
" \"state_topic\": \"" + ring_pubtopic + "\","
" \"event_types\": [\"ring\"],"
" \"qos\": 0,"
" \"device_class\": \"doorbell\","
" \"icon\": \"mdi:doorbell\","
" \"unique_id\": \"ringevent_" + uid + "\","
" \"device\": {"
"   \"identifiers\": [\"" + uid + "\"],"
"   \"hw_version\": \"" + String(ESP.getChipModel()) + "\","
"   \"manufacturer\": \"Innovutech\","
"   \"model\": \"intercom_proto\","
"   \"name\": \"Intercom - " + uid + "\","
"   \"serial_number\": \"" + uid + "\","
"   \"sw_version\": \"1.0.0\" "
" }"
"}"
;

String unlockbtn_payload =
"{"
" \"name\": \"Unlock\","
" \"command_topic\": \"" + cmd_subtopic + "\","
" \"payload_press\": \"unlock\","
" \"qos\": 0,"
" \"availability_topic\": \"" + will_pubtopic + "\","
" \"icon\": \"mdi:lock-open\","
" \"unique_id\": \"unlockbtn_" + uid + "\","
" \"device\": {"
"   \"identifiers\": [\"" + uid + "\"]"
" }"
"}"
;

String rbbtn_payload =
"{"
" \"name\": \"Reboot\","
" \"command_topic\": \"" + cmd_subtopic + "\","
" \"payload_press\": \"reboot\","
" \"qos\": 0,"
" \"availability_topic\": \"" + will_pubtopic + "\","
" \"device_class\": \"restart\","
" \"icon\": \"mdi:restart\","
" \"unique_id\": \"rbbtn_" + uid + "\","
" \"device\": {"
"   \"identifiers\": [\"" + uid + "\"]"
" }"
"}"
;

String rstbtn_payload =
"{"
" \"name\": \"Reset Setings\","
" \"command_topic\": \"" + cmd_subtopic + "\","
" \"payload_press\": \"reset\","
" \"qos\": 0,"
" \"availability_topic\": \"" + will_pubtopic + "\","
" \"device_class\": \"restart\","
" \"icon\": \"mdi:cog-refresh\","
" \"unique_id\": \"rstbtn_" + uid + "\","
" \"device\": {"
"   \"identifiers\": [\"" + uid + "\"]"
" }"
"}"
;

String mutesw_payload =
"{"
" \"name\": \"Sound\","
" \"command_topic\": \"" + mute_subtopic + "\","
" \"payload_off\": \"1\","
" \"payload_on\": \"0\","
" \"state_topic\": \"" + mute_pubtopic + "\","
" \"qos\": 0,"
" \"availability_topic\": \"" + will_pubtopic + "\","
" \"icon\": \"mdi:volume-high\","
" \"unique_id\": \"mutesw_" + uid + "\","
" \"device\": {"
"   \"identifiers\": [\"" + uid + "\"]"
" }"
"}"
;

String autounlocksw_payload =
"{"
" \"name\": \"Autounlock\","
" \"command_topic\": \"" + autounlock_subtopic + "\","
" \"payload_off\": \"0\","
" \"payload_on\": \"1\","
" \"state_topic\": \"" + autounlock_pubtopic + "\","
" \"qos\": 0,"
" \"availability_topic\": \"" + will_pubtopic + "\","
" \"icon\": \"mdi:lock-open-check\","
" \"unique_id\": \"autounlocksw_" + uid + "\","
" \"device\": {"
"   \"identifiers\": [\"" + uid + "\"]"
" }"
"}"
;

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println();

  if(!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED)){
    Serial.println("SPIFFS Mount Failed");
    return;
  }

  pinMode(IOLOCK, OUTPUT);
  pinMode(IOLED, OUTPUT);
  pinMode(IOBELL, INPUT_PULLDOWN);
  pinMode(MUTEPIN, OUTPUT);
  pinMode(IORST, INPUT_PULLUP);
  pinMode(MUTEPIN_JMP, INPUT_PULLUP);

  MUTEPIN_JMP_STATE = !digitalRead(MUTEPIN_JMP);

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
  mqttClient.setWill(will_pubtopic.c_str(), 0, true, "offline");
  mqttClient.setCleanSession(true);
  mqttClient.setCACert(rootca);
  mqttClient.setCredentials(mqttusr, mqttpwd);
  mqttClient.setServer(mqtthost, mqttport);

  xTaskCreatePinnedToCore((TaskFunction_t)networkingTask, "mqttclienttask", 5120, nullptr, 1, &taskHandle, 0);

  bool res;
  res = wm.autoConnect(APSSID.c_str(),APPWD.c_str());
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

  Serial.println("Publishing online status at QoS 0, packetId: ");
  uint16_t packetIdPub0 = mqttClient.publish(will_pubtopic.c_str(), 0, true, "online");
  uint16_t packetIdPub1 = mqttClient.publish(autounlock_pubtopic.c_str(), 0, true, String(autounlock).c_str());
  uint16_t packetIdPub2 = mqttClient.publish(mute_pubtopic.c_str(), 0, true, String(!MUTEPIN_JMP_STATE).c_str());
  Serial.println("Publishing at QoS 0, packetId: ");
  Serial.println(packetIdPub0);
  Serial.println(packetIdPub1);
  Serial.println(packetIdPub2);
  publish_discovery();

  uint16_t packetIdSub0 = mqttClient.subscribe(cmd_subtopic.c_str(), 0);
  uint16_t packetIdSub1 = mqttClient.subscribe(mute_subtopic.c_str(), 0);
  uint16_t packetIdSub2 = mqttClient.subscribe(autounlock_subtopic.c_str(), 0);
  uint16_t packetIdSub3 = mqttClient.subscribe(birth_subtopic.c_str(), 0);
  Serial.print("Subscribing at QoS 0, packetId: ");
  Serial.println(packetIdSub0);
  Serial.println(packetIdSub1);
  Serial.println(packetIdSub2);
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
}

void onMqttUnsubscribe(uint16_t packetId) {
  Serial.println("Unsubscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

void onMqttMessage(const espMqttClientTypes::MessageProperties& properties, const char* topic, const uint8_t* payload, size_t len, size_t index, size_t total) {

  char* tmp = new char[len + 1];
  memcpy(tmp, payload, len);
  tmp[len] = '\0';
  String data = String(tmp);
  delete[] tmp;

  if (String(topic) == cmd_subtopic) {
    if (data == "unlock"){
      unlockDoor();
    } else if (data == "reset") {
      handleReset();
    } else if (data == "reboot") {
      handleReboot();
    }
  } else if (String(topic) == mute_subtopic) {
    int mapped_setting = (data == "1") ? 1 : ((data == "0") ? 0 : -1);

    switch (mapped_setting) {
      case 0:
      case 1:
        {
          bool mapped_state = !mapped_setting;
          digitalWrite(MUTEPIN, mapped_state);
          mqttClient.publish(mute_pubtopic.c_str(), 0, true, data.c_str());
          String state = (mapped_setting == 1) ? "MUTED" : "UNMUTED";
          Serial.println(state);
          break;
        }
      default:
        Serial.print("Unrecognised option -> ");
        Serial.println(data);
    }
  } else if (String(topic) == autounlock_subtopic) {
    int mapped_setting = (data == "1") ? 1 : ((data == "0") ? 0 : -1);

    switch (mapped_setting) {
      case 0:
      case 1:
        {
          autounlock = mapped_setting;
          mqttClient.publish(autounlock_pubtopic.c_str(), 0, true, data.c_str());
          String state = (mapped_setting == 1) ? "AUTOUNLOCK ON" : "AUTOUNLOCK OFF";
          Serial.println(state);
          break;
        }
      default:
        Serial.print("Unrecognised option -> ");
        Serial.println(data);
    }
  } else if (String(topic) == birth_subtopic) {
    if (data == "online") {
      udelay(200);
      publish_discovery();
    }
  }

  Serial.println("Publish received.");
  Serial.print("  topic: ");
  Serial.println(topic);
  Serial.print("  retain: ");
  Serial.println(properties.retain);
  Serial.println(" payload ");
  Serial.print(data.c_str());
  Serial.println("");
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

void publish_discovery() {
  //mqtt-discovery for automatic discovery of the device on Home Assistant
  //note for myself: this should be called durring the mqtt connection event or when the device receives a birth status message from homeassistant's birth topic
  Serial.println("Publishing at QoS 0, packetId: ");
  uint16_t packetIdPub0 = mqttClient.publish(configtopic("event", "ring"), 0, false, event_payload.c_str());
  uint16_t packetIdPub1 = mqttClient.publish(configtopic("button", "unlock"), 0, false, unlockbtn_payload.c_str());
  uint16_t packetIdPub2 = mqttClient.publish(configtopic("switch", "mute"), 0, false, mutesw_payload.c_str());
  uint16_t packetIdPub3 = mqttClient.publish(configtopic("switch", "autounlock"), 0, false, autounlocksw_payload.c_str());
  uint16_t packetIdPub4 = mqttClient.publish(configtopic("button", "rb"), 0, false, rbbtn_payload.c_str());
  uint16_t packetIdPub5 = mqttClient.publish(configtopic("button", "rst"), 0, false, rstbtn_payload.c_str());

  Serial.println(packetIdPub0);
  Serial.println(packetIdPub1);
  Serial.println(packetIdPub2);
  Serial.println(packetIdPub3);
  Serial.println(packetIdPub4);
  Serial.println(packetIdPub5);
}

void handleReset() {
  digitalWrite(led, 0);
  udelay(500);
  digitalWrite(led, 1);

  mqttClient.disconnect(true);
  wm.resetSettings();
  ESP.restart();
}

void handleReboot() {
  digitalWrite(led, 0);
  udelay(500);
  digitalWrite(led, 1);

  mqttClient.disconnect(true);
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

  /*if (currentMillis - millisDisconnect > 60000) {
    millisDisconnect = currentMillis;
    mqttClient.disconnect();
  }*/
}

void loop() {
  handleMqttReconnect();
  if (digitalRead(IOBELL) == HIGH && BellTimer.hasExpired()) {
    if (autounlock) {
      unlockDoor();
    }

    // wait for WiFi connection
    if ((WiFi.status() == WL_CONNECTED)) {
      publishRing();
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
      publishRing();
    }
  }
}

void publishRing() {
  Serial.println("Bell ring detected!");
  const char * payload =
  "{"
  "\"event_type\": \"ring\""
  "}"
  ;
  mqttClient.publish(ring_pubtopic.c_str(), 0, false, payload);
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

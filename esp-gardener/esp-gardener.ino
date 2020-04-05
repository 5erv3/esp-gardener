#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"
#include "esp_system.h"

// uncomment logging when not needed
#define LOGGING  1

#define WIFI_MAX_CONNECT_TIME_SEC 60
#define MQTT_RECONNECT_ATTEMPTS   3

#define WATERLEVEL_STOP_HEIGHT_CM           (75)

#define WATERLEVEL_TIME_INTERVAL_US         (60 * 60 * 1000 * 1000UL)
#define MAX_WATERINGTIME_S                  (60 * 45)
#define WATCHDOG_TIMEOUT_US                 (30 * 60 * 1000 * 1000UL)
#define MAX_TIMER_LEN_US                    (70 * 60 * 1000 * 1000UL)

#define WATCHDOG_TIMER_ID   0
#define WATERLEVEL_TIMER_ID 1
#define WATERPUMP_TIMER_ID  2

WiFiClient espClient;
PubSubClient client(espClient);

char buf[150];
int reconnect_counter = 0;

volatile bool waterpump_running = false;
volatile bool waterlevel_timer_expired = false;
volatile bool waterpump_timer_expired = false;

hw_timer_t *wdt_timer = NULL;
hw_timer_t *waterpump_timer = NULL;
hw_timer_t *waterlevel_timer = NULL;

int get_waterlevel_cm(void);

void LOG(const char* logstring, bool newline = true){
  #if LOGGING
  if (newline){
    Serial.println(logstring);
  } else {      
    Serial.print(logstring);
  }
  #endif
}

void initGPIO() {
  pinMode(relaisPin, OUTPUT);
  pinMode(sonicTriggerPin, OUTPUT);
  pinMode(sonicEchoPin, INPUT);
  digitalWrite(relaisPin, 0);
}

void IRAM_ATTR watchdog_timeout() {
  LOG("watchdog reset");
  esp_restart();
}

void start_waterpump_timer(int time_sec){
  uint64_t time_interval_us = 0;
  if (time_sec > MAX_WATERINGTIME_S){
    time_interval_us = ((uint64_t) MAX_WATERINGTIME_S) * 1000 * 1000;
    sprintf(buf, "Lowered waterpump interval to max time of %d us", MAX_WATERINGTIME_S);
    LOG(buf);
  } else {
    LOG("watertimer start");
    time_interval_us = ((uint64_t) time_sec) * 1000 * 1000;    
  }
  timerAlarmWrite(waterpump_timer, time_interval_us, false);
  timerAlarmEnable(waterpump_timer);
}

void stop_pump(){
  if (waterpump_running){
    if (timerStarted(waterpump_timer)){
      timerAlarmDisable(waterpump_timer);
      waterpump_timer_expired = false;
    }    
    waterpump_running = false;  
  }
  digitalWrite(relaisPin, 0);
}

bool check_waterlevel_ok(){
  if (get_waterlevel_cm() >= WATERLEVEL_STOP_HEIGHT_CM){
    return false;
  } else {
    return true;
  }
}

void start_pump(int seconds){
  if (!check_waterlevel_ok()){
    LOG("waterlevel too low, not staring pump");
    return;
  }
  if (waterpump_running){
    LOG("waterpump already running, not starting");
    return;
  }
  start_waterpump_timer(seconds);
  waterpump_running = true;
  digitalWrite(relaisPin, 1);  
}

void IRAM_ATTR waterpump_timeout(){
  waterpump_timer_expired = true;
}

void IRAM_ATTR waterlevel_timeout(){
  waterlevel_timer_expired = true;
}

void callback(char* topic, byte* payload, unsigned int length) {
  bool discard = false;
  int payload_int = 0;
  sprintf(buf, "Message on topic %s, lenght %d", topic, length);
  LOG(buf);
  
  for (int i = 0; i < length; i++) {
    if ( (char)payload[i] < '0' || (char)payload[i] > '9'){
      sprintf(buf, "non-number payload received: %d, discarding", payload[i]);
      LOG(buf);
      discard = true;
    } else {
      payload_int += ((char) payload[i] - '0') * pow(10, (length - (i + 1)));
    }
  }
  sprintf(buf, "payload: %d, discard: %d", payload_int, discard);
  LOG(buf);

  if (!discard){
    publish_waterlevel();
    
    if (payload_int > 0){   
      sprintf(buf, "setting waterpump to %d sec", payload_int);
      LOG(buf); 
      start_pump(payload_int);
    } else{
      stop_pump();
    }
  }
}

int getDistance()
{
  long distance = 0;
  long timex = 0;

  digitalWrite(sonicTriggerPin, LOW);
  delayMicroseconds(3);
  noInterrupts();
  digitalWrite(sonicTriggerPin, HIGH); //Trigger Impuls 10 us
  delayMicroseconds(10);
  digitalWrite(sonicTriggerPin, LOW);
  timex = pulseIn(sonicEchoPin, HIGH, 200000); // Echo-Zeit messen
  interrupts();
  if (timex == 0) {
    return -1;
  }
  timex = (timex / 2); // Zeit halbieren
  distance = timex / 29.1; // Zeit in Zentimeter umrechnen
  return (distance);
}

int getMovingAverage() {
  int alt = 0;
  int mittel;
  int entf;
  int i;

  alt = getDistance();
  if (alt == -1) {
    return -1;
  }
  delay(10);
  for (i = 0; i < 50; i++)
  {
    entf = getDistance();
    if (entf == -1) {
      return -1;
    }
    mittel = (0.8 * alt) + (0.2 * entf);
    alt = mittel;
    delay(5);
  }
  return (mittel);
}

void init_connection(){  
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void init_watchdog(){
  wdt_timer = timerBegin(WATCHDOG_TIMER_ID, 80, true);                  
  timerAttachInterrupt(wdt_timer, &watchdog_timeout, true);
  if (WATCHDOG_TIMEOUT_US > MAX_TIMER_LEN_US){
    sprintf(buf, "Lowered watchdog interval to max time of %d us", MAX_TIMER_LEN_US);
    LOG(buf);
    timerAlarmWrite(wdt_timer, MAX_TIMER_LEN_US, false);
  } else {
    timerAlarmWrite(wdt_timer, WATCHDOG_TIMEOUT_US, false);
  }  
  timerAlarmEnable(wdt_timer);
}

void init_waterlevel_timer(){
  waterlevel_timer = timerBegin(WATERLEVEL_TIMER_ID, 80, true);
  timerAttachInterrupt(waterlevel_timer, &waterlevel_timeout, true);
  if (WATERLEVEL_TIME_INTERVAL_US > MAX_TIMER_LEN_US){
    sprintf(buf, "Lowered waterlevel interval to max time of %d us", MAX_TIMER_LEN_US);
    LOG(buf);
    timerAlarmWrite(waterlevel_timer, MAX_TIMER_LEN_US, true);
  } else {
    timerAlarmWrite(waterlevel_timer, WATERLEVEL_TIME_INTERVAL_US, true);
  }
  timerAlarmEnable(waterlevel_timer);
}

void init_waterpump_timer(){
  waterpump_timer = timerBegin(WATERPUMP_TIMER_ID, 80, true);
  timerAttachInterrupt(waterpump_timer, &waterpump_timeout, true);
}

void setup() {
  initGPIO();
#if LOGGING
  Serial.begin(115200);
#endif
  init_watchdog();
  init_waterlevel_timer();
  init_waterpump_timer();
  init_connection();
}

void setup_wifi() {
  delay(10);

  sprintf(buf, "Connecting to %s...", wifi_ssid);
  LOG(buf);

  WiFi.begin(wifi_ssid, wifi_password);

  reconnect_counter = 0;

  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    reconnect_counter ++;
    if (reconnect_counter >= WIFI_MAX_CONNECT_TIME_SEC){
      LOG("ERROR: WiFi connection counter expired");
      esp_restart();
    }
  }  
  sprintf(buf, "WiFi connected, IP address: ");
  LOG(buf, false);
  Serial.print(WiFi.localIP());
  LOG("");
}

void reconnect() {
  reconnect_counter = 0;
  while (!client.connected()) {
    LOG("Attempting MQTT connection...");
    if (client.connect("ESP8266Client")) {
      LOG("connected", true);
      publish_waterlevel();
      client.subscribe(mqtt_topic_watercontrol);
    } else {
      reconnect_counter++;
      if (reconnect_counter >= MQTT_RECONNECT_ATTEMPTS){
        LOG("ERROR: too many mqtt attempts");
        esp_restart();
      }      
      sprintf(buf, "connection failed, rc %d", client.state());
      LOG(buf);      
      LOG(" try again in 5 seconds", true);
      delay(5000);
    }
  }
}

int get_waterlevel_cm(){
  int distance;
  distance = getMovingAverage();
  sprintf(buf, "Waterlevel = %d cm", distance);
  LOG(buf);
  return distance;
}

int publish_waterlevel(){
  int distance;  
  int return_val;
  distance = get_waterlevel_cm();
  sprintf(buf, "%d", distance); 
  return_val = client.publish(mqtt_topic_waterlevel, buf);
  sprintf(buf, "published %d to topic %s", distance, mqtt_topic_waterlevel);
  LOG(buf);
  return return_val;
}

void loop() {
  int distance;
  static bool stop_reported = true;

  if (WiFi.status() != WL_CONNECTED){
    init_connection();
  }
  
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  if (waterpump_running && !check_waterlevel_ok() || waterpump_timer_expired){
    waterpump_timer_expired = false;
    stop_pump();
    publish_waterlevel();
  }

  if (waterlevel_timer_expired){
    waterlevel_timer_expired = false;
    publish_waterlevel();
  }

  if (client.connected() && WiFi.status() == WL_CONNECTED){
    timerWrite(wdt_timer, 0);
  }
  
  delay(1000);
}

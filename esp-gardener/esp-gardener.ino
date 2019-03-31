#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"

// uncomment logging when not needed
#define LOGGING  1

#define WIFI_MAX_CONNECT_TIME_SEC 30
#define MQTT_RECONNECT_ATTEMPTS   3

#define WATERLEVEL_STOP_HEIGHT_CM           80
#define PAUSE_BETWEEN_WATERLEVEL_MESSAGE_S  (60 * 60)
#define DEFAULT_RESET_TIME                  (60 * 60 * 24)
#define MAX_WATERINGTIME_S                  (60 * 15)

WiFiClient espClient;
PubSubClient client(espClient);

char buf[150];
int reconnect_counter = 0;
uint64_t loopcount = 0;
int wateringcounter = 0;

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

void control_waterpump(int sensorValue, int enable){
  if (!enable){
    if (digitalRead(relaisPin) == 1){
     LOG("Disabling water pump");
     wateringcounter = 0;
    }
    digitalWrite(relaisPin, 0);
  }else if (sensorValue >= WATERLEVEL_STOP_HEIGHT_CM){
    sprintf(buf, "Waterlevel %d >= %d, disabling water pump", sensorValue, WATERLEVEL_STOP_HEIGHT_CM);
    LOG(buf);
    digitalWrite(relaisPin, 0);
    wateringcounter = 0; 
  } else {
    digitalWrite(relaisPin, 1);
  }
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
      payload_int += (char) (((char) payload[i] - '0') * pow(10, (length - (i + 1))));
    }
  }
  sprintf(buf, "payload: %d, discard: %d", payload_int, discard);
  LOG(buf);

  if (!discard){
    
    if (payload_int > MAX_WATERINGTIME_S){
      payload_int = MAX_WATERINGTIME_S;
    }

    if (payload_int > 0){   
      if ( wateringcounter == 0){
        sprintf(buf, "setting wateringcounter to %d sec", payload_int);
        LOG(buf); 
        wateringcounter = payload_int;
      }      
    } else{
      wateringcounter = 0;
      control_waterpump(0,0);
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

void setup() {
  initGPIO();
#if LOGGING
  Serial.begin(115200);
#endif
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
      ESP.restart();
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
      client.subscribe(mqtt_topic_watercontrol);
    } else {
      reconnect_counter++;
      if (reconnect_counter >= MQTT_RECONNECT_ATTEMPTS){
        LOG("ERROR: too many mqtt attempts");
        ESP.restart();
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

void publish_waterlevel(){
  int distance;  
  distance = get_waterlevel_cm();
  sprintf(buf, "%d", distance); 
  client.publish(mqtt_topic_waterlevel, buf);
  sprintf(buf, "published %d to topic %s", distance, mqtt_topic_waterlevel);
  LOG(buf);
}



void loop() {
  int distance;
  
  if (loopcount >= DEFAULT_RESET_TIME){
    // reset esp after a default time, just because
    ESP.restart();
  }

  if (WiFi.status() != WL_CONNECTED){
    init_connection();
  }
  
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  if (loopcount % PAUSE_BETWEEN_WATERLEVEL_MESSAGE_S == 0){
    publish_waterlevel();
  }

  if (wateringcounter > 0){
    wateringcounter --;
    distance = get_waterlevel_cm();
    control_waterpump(distance, 1);
    sprintf(buf, "Watercounter: %d sec", wateringcounter);
    LOG(buf);
  } else {
    control_waterpump(0,0);
  }
  
  delay(1000);
  loopcount++;
}

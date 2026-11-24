
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

//WiFi login.  
const char* WIFI_NAME = "";
const char* WIFI_PASS = "";

//ThingsBoard login.  
const char* MQTT_SERVER = "thingsboard.cloud";
const int   MQTT_PORT   = 1883;
const char* MQTT_TOKEN  = "BOofAdywM7xCiscneb4b";

const char* TOPIC_TELEMETRY = "v1/devices/me/telemetry";
const char* TOPIC_RPC       = "v1/devices/me/rpc/request/+";   
const char* RPC_RESPONSE_PREFIX = "v1/devices/me/rpc/response/"; 

//Hardware pins.
const int SOUND_PIN  = 34;
const int LED_PIN    = 2;
const int BUZZER_PIN = 4;

//Noise measurement settings.
const int  SAMPLE_COUNT       = 1000;  
const int  NOISE_PERIOD_MS    = 200;    
const int  PUBLISH_PERIOD_MS  = 5000;   
const int  ADC_MAX            = 4095;   
const bool PRINT_LIVE_NOISE   = true;   

//Scale of YOUR microphone.
const int  NOISE_RAW_MAX      = 250;
const int  NOISE_ALERT_RAW    = 150;  

//Buzzer settings.
const int  BEEP_DURATION_MS    = 1000;
const bool BUZZER_IS_PASSIVE   = false;
const int  TONE_HALF_PERIOD_US = 250; 

WiFiClient   espClient;
PubSubClient mqtt_client(espClient);

//One noise measurement.
struct NoiseData {
  int  raw;
  int  percent;  
  bool alert;     
};

//Queue with size 1 that always holds the NEWEST measurement.
QueueHandle_t noise_queue = NULL;

//Event group = status flags.
EventGroupHandle_t status_events = NULL;
#define WIFI_CONNECTED_BIT  (1 << 0)
#define MQTT_CONNECTED_BIT  (1 << 1)

//Mutex (lock) for mqtt_client.
SemaphoreHandle_t mqtt_mutex = NULL;

//Binary semaphore = a "doorbell" for the buzzer task.
SemaphoreHandle_t beep_sem = NULL;


//Handle every message coming from ThingsBoard (RPC commands).
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("rx topic: %s\n", topic);

  // Copy the payload into a normal C string.
  char rx_payload[length + 1];
  memcpy(rx_payload, payload, length);
  rx_payload[length] = '\0';
  Serial.printf("rx payload: %s\n", rx_payload);

  // Read the JSON to find the command name ("method").
  StaticJsonDocument<200> doc;
  DeserializationError err = deserializeJson(doc, rx_payload);
  if (err) {
    Serial.printf("json error: %s\n", err.c_str());
    return;
  }
  const char* method = doc["method"] | "";

  if (strcmp(method, "beep") == 0) {
    Serial.println("command received: BEEP");
    xSemaphoreGive(beep_sem);                 

    const char* request_id = strrchr(topic, '/');
    if (request_id != NULL) {
      char response_topic[100];
      snprintf(response_topic, sizeof(response_topic), "%s%s",
               RPC_RESPONSE_PREFIX, request_id + 1);
      mqtt_client.publish(response_topic, "{\"beeped\":true}");
    }
  } else {
    Serial.printf("unknown command: %s\n", method);
  }
}

//Connect to WiFi and keep it connected.
void wifi_task_fn(void *arg) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_NAME, WIFI_PASS);

  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      xEventGroupSetBits(status_events, WIFI_CONNECTED_BIT);
    } else {
      Serial.println("wifi disconnected, retrying...");
      xEventGroupClearBits(status_events, WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT);
      WiFi.reconnect();
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

//Connect to ThingsBoard, keep the MQTT link alive, receive commands.
void mqtt_task_fn(void *arg) {
  while (true) {
    // Sleep (no CPU use) until WiFi is connected.
    xEventGroupWaitBits(status_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    xSemaphoreTake(mqtt_mutex, portMAX_DELAY);

    if (!mqtt_client.connected()) {
      xEventGroupClearBits(status_events, MQTT_CONNECTED_BIT);

      if (mqtt_client.connect("esp_noise_device", MQTT_TOKEN, "")) {
        Serial.println("mqtt connected to server");
        mqtt_client.subscribe(TOPIC_RPC);      
        xEventGroupSetBits(status_events, MQTT_CONNECTED_BIT);
      } else {
        Serial.printf("mqtt connect failed, state=%d\n", mqtt_client.state());
        xSemaphoreGive(mqtt_mutex);
        vTaskDelay(pdMS_TO_TICKS(5000));      
        continue;
      }
    }

    mqtt_client.loop();
    xSemaphoreGive(mqtt_mutex);

    vTaskDelay(pdMS_TO_TICKS(100));          
  }
}

//Measure the noise level, turn on the LED when too loud, and
void noise_task_fn(void *arg) {
  pinMode(LED_PIN, OUTPUT);

  while (true) {
    int signal_max = 0;
    int signal_min = ADC_MAX;

    // Read the microphone SAMPLE_COUNT times, remember the biggest and smallest value.
    for (int i = 0; i < SAMPLE_COUNT; i++) {
      int sample = analogRead(SOUND_PIN);
      if (sample > signal_max) signal_max = sample;
      if (sample < signal_min) signal_min = sample;
    }

    NoiseData data;
    data.raw     = signal_max - signal_min; 
    data.percent = constrain((data.raw * 100) / NOISE_RAW_MAX, 0, 100);   
    data.alert   = (data.raw >= NOISE_ALERT_RAW);                         

    digitalWrite(LED_PIN, data.alert ? HIGH : LOW);  
    xQueueOverwrite(noise_queue, &data);             

    if (PRINT_LIVE_NOISE) {
      char bar[21];                    
      int filled = data.percent / 5;        
      for (int i = 0; i < 20; i++) bar[i] = (i < filled) ? '#' : '-';
      bar[20] = '\0';

      Serial.printf("NOISE [%s] %3d %%  raw=%4d  %s\n",
                    bar, data.percent, data.raw,
                    data.alert ? "<< TOO LOUD" : "");
    }

    vTaskDelay(pdMS_TO_TICKS(NOISE_PERIOD_MS));
  }
}

//Send the latest noise value to ThingsBoard every PUBLISH_PERIOD_MS.
void publish_task_fn(void *arg) {
  while (true) {
    xEventGroupWaitBits(status_events, MQTT_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    NoiseData data;
    if (xQueuePeek(noise_queue, &data, pdMS_TO_TICKS(1000)) == pdTRUE) {

      StaticJsonDocument<200> doc;
      doc["noise_percent"] = data.percent;
      doc["noise_raw"]     = data.raw;
      doc["noise_alert"]   = data.alert;

      char jsonBuffer[200];
      serializeJson(doc, jsonBuffer);

      xSemaphoreTake(mqtt_mutex, portMAX_DELAY);
      bool sent = mqtt_client.publish(TOPIC_TELEMETRY, jsonBuffer);
      xSemaphoreGive(mqtt_mutex);

      Serial.printf("publish %s : %s\n", sent ? "OK" : "FAILED", jsonBuffer);
    }

    vTaskDelay(pdMS_TO_TICKS(PUBLISH_PERIOD_MS));
  }
}

// Sound the buzzer for BEEP_DURATION_MS (1 second) on command.
void buzzer_task_fn(void *arg) {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  while (true) {
    // Sleep until a "beep" command arrives.
    xSemaphoreTake(beep_sem, portMAX_DELAY);

    Serial.println("BUZZER ON");

    if (BUZZER_IS_PASSIVE) {
      // Passive buzzer: make a 2 kHz square wave for 1 second.
      uint32_t start = millis();
      while (millis() - start < BEEP_DURATION_MS) {
        digitalWrite(BUZZER_PIN, HIGH);
        delayMicroseconds(TONE_HALF_PERIOD_US);
        digitalWrite(BUZZER_PIN, LOW);
        delayMicroseconds(TONE_HALF_PERIOD_US);
      }
    } else {
      // Active buzzer: just switch it on, wait, switch it off.
      digitalWrite(BUZZER_PIN, HIGH);
      vTaskDelay(pdMS_TO_TICKS(BEEP_DURATION_MS));
      digitalWrite(BUZZER_PIN, LOW);
    }

    Serial.println("BUZZER OFF");

    // Ignore extra button presses that happened while beeping (one press = one beep).
    xSemaphoreTake(beep_sem, 0);
  }
}

//Create shared objects, then start the tasks.
void setup() {
  Serial.begin(115200);

  noise_queue   = xQueueCreate(1, sizeof(NoiseData));
  status_events = xEventGroupCreate();
  mqtt_mutex    = xSemaphoreCreateMutex();
  beep_sem      = xSemaphoreCreateBinary();
  if (noise_queue == NULL || status_events == NULL || mqtt_mutex == NULL || beep_sem == NULL) {
    Serial.println("failed to create queue / event group / mutex / semaphore");
    while (true) { delay(1000); }
  }

  analogReadResolution(12);   // 12-bit ADC: values 0 - 4095

  mqtt_client.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt_client.setCallback(mqtt_callback);

  xTaskCreate(wifi_task_fn,    "wifi_task",    4 * 1024, NULL, 5, NULL);
  xTaskCreate(mqtt_task_fn,    "mqtt_task",    6 * 1024, NULL, 6, NULL);
  xTaskCreate(noise_task_fn,   "noise_task",   3 * 1024, NULL, 4, NULL);
  xTaskCreate(publish_task_fn, "publish_task", 5 * 1024, NULL, 5, NULL);
  xTaskCreate(buzzer_task_fn,  "buzzer_task",  3 * 1024, NULL, 3, NULL);
}

//Nothing to do.
void loop() {
  vTaskDelete(NULL);
}
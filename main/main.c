// ESP32 + L9110, 1 kierunek, 1 pin (GPIO23), tylko HIGH/LOW
const int LigtPin = 23;


#include <passwords.h>             // Plik z danymi do WiFi (ssid1, password1, ssid2, password2)
// Biblioteki użyte na ESP32
#include <WiFi.h>               // Łączenie z WiFi
#include <PubSubClient.h>       // Wykrzyzstywana do obsługi MQTT

#include "driver/mcpwm.h"       // sprzętowe PWM (MCPWM) do sterowania mikro silnikiem MG90S

// WiFiClient = połączenie TCP, a PubSubClient = protokół MQTT na tym połączeniu.
WiFiClient espClient;           //Tworzy klienta TCP działającego na stosie Wi-Fi ESP32
PubSubClient client(espClient); //Tworzy klienta MQTT z biblioteki PubSubClient

// L9110 Control Pins (logika jak wcześniej: 2 piny = kierunek + PWM)
// Bez R_EN / L_EN
#define RPWM_Output    21
#define LPWM_Output    32 //good direction for me

// Logika światła (z komentarza: LOW = ON)
#define ON   LOW
#define OFF  HIGH

int conveyorValue;

// !! deklaracja (potrzebne przed setCallback)
void callback(char* topic, byte* payload, unsigned int length);

// Servo konfiguracja PWM przy użyciu biblioteki MCPWM
static const gpio_num_t SERVO_PIN = GPIO_NUM_19; // Pin 19 używany do sterowania pozycją
static const int SERVO_FREQ_HZ = 50;             // 20 ms
static const int US_MIN = 500;                   // MG90S typowo
static const int US_MAX = 2400;
static const uint32_t MOVE_MS = 3000;            // czas ruchu rozlozony na 3 s, by nie byl gwałtowny

// Stan ruchu (nieblokujący)
static int   s_us_start  = 1500;
static int   s_us_target = 1500;
static int   s_us_current = 1500;
static uint32_t s_t_start_ms = 0;
static bool  s_moving = false;

inline int clampi(int v, int lo, int hi){ return v<lo?lo:(v>hi?hi:v); }

inline int degToUs(int deg){
  deg = clampi(deg, 0, 180);
  return US_MIN + (US_MAX - US_MIN) * deg / 180;
}

inline void writeServoUS_now(int us){
  us = clampi(us, US_MIN, US_MAX);
  mcpwm_set_duty_in_us(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, us);
  s_us_current = us;
}

void startMoveToUS(int us_target){
  us_target = clampi(us_target, US_MIN, US_MAX);  // Ogranicza zakres do bezpiecznych granic serwa

  s_us_start   = s_us_current;                    // aktualna pozycja jako start
  s_us_target  = us_target;
  s_t_start_ms = millis();
  s_moving     = true;

  Serial.print("MOVE start_us="); Serial.print(s_us_start);
  Serial.print(" target_us=");    Serial.print(s_us_target);
  Serial.print(" dur_ms=");       Serial.println(MOVE_MS);
}

void updateServoMotion(){
  if(!s_moving){                                     // jeśli nie trwa ruch -> nic nie rób
    return;
  }

  uint32_t now = millis();                           // [ms] teraz  -> aktualny czas systemowy
  uint32_t dt  = now - s_t_start_ms;                 // [ms] dt     -> ile ms minęło od startu ruchu
                                                     // s_t_start_ms = timestamp, kiedy wywołano startMove...

  if(dt >= MOVE_MS){                                 // jeśli minął cały czas ruchu
    writeServoUS_now(s_us_target);                   // dojedź dokładnie do celu
    s_moving = false;                                // zakończ ruch
    Serial.print("MOVE done us="); Serial.println(s_us_target);
    return;
  }

  // interpolacja liniowa (płynny dojazd w czasie MOVE_MS)
  float alpha = (float)dt / (float)MOVE_MS;           // 0..1        -> postęp ruchu w czasie (3 sekundy)
  int us = s_us_start + (int)roundf((s_us_target - s_us_start) * alpha);
                                                      // us          -> pośrednia pozycja (rampa liniowa)
  writeServoUS_now(us);                               // wyślij PWM  -> aktualny krok ruchu
}


void servoInit(){
  mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0A, SERVO_PIN);  // MCPWM0A     -> przypięcite do pinu GPIO19

  mcpwm_config_t cfg = {};
  cfg.frequency     = SERVO_FREQ_HZ;                 // 50 Hz       -> serwo standardowe
  cfg.cmpr_a        = 0;                             // start duty  -> ustawiane potem w us
  cfg.counter_mode  = MCPWM_UP_COUNTER;              // licznik UP  -> standard tryb do generacji PWM (50Hz z cfg.frequency)
  cfg.duty_mode     = MCPWM_DUTY_MODE_0;             // tryb duty   -> standard PWM, bez obracania

  mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &cfg);     // UNIT0/T0    -> start MCPWM

  // Start w środku zakresu = 30°
  s_us_current = degToUs(30);                        // start pos   -> 30° w mikrosekundach
  s_us_start   = s_us_current;                       // spójność    -> start = current
  s_us_target  = s_us_current;                       // spójność    -> target = current

  writeServoUS_now(s_us_current);                    // ustaw PWM na serwo
  Serial.print("SERVO init us="); Serial.println(s_us_current);
}

//// Serial parser ////
void handleSerial(){
 //
}

bool attemptWiFiConnection() {
  int retry_count = 0;
  while (WiFi.status() != WL_CONNECTED && retry_count < 5) {
    delay(1000);
    Serial.println("Retrying Wi-Fi connection...");
    retry_count++;
  }
  return (WiFi.status() == WL_CONNECTED);
}

void connectWiFi() {
  Serial.println("Connecting to Wi-Fi...");
  WiFi.begin(ssid1, password1);
  if (!attemptWiFiConnection()) {
    Serial.println("Switching to another Wi-Fi...");
    WiFi.begin(ssid2, password2);
    if (!attemptWiFiConnection()) {
      Serial.println("Failed to connect to any Wi-Fi, restarting...");
      ESP.restart();
    }
  }
  Serial.println("WiFi connected");
  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP());
}

//  MQTT logika połączenia oraz subskrypcji tematów
void reconnectMQTT() {
  int retry_count = 0;
  while (!client.connected() && retry_count < 5) {
    Serial.print("Attempting MQTT connection...");
    String clientId = "ESP32Client-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("MQTT connected");
      // Subscribe to all needed topics
      //client.subscribe("/home/control/fanPWM"); // not used rn
      client.subscribe("/home/control/conveyorPWM");
      client.subscribe("/set_control_mode");
      client.subscribe("/home/control/light");    
      client.subscribe("/home/control/servo");    

client.subscribe("/home/control/DCpwm");
    } else {
      Serial.print("Failed, rc=");
      Serial.println(client.state());
      delay(5000);
      retry_count++;
    }
  }
  if (!client.connected()) {
    Serial.println("Failed to connect MQTT, restarting...");
    ESP.restart();
  }
}
// Prosta konfiguracja IP Brokera MQTT oraz podpięcie funkcji callback() wywoływanej automatycznie po odebraniu wiadomości z tematów MQTT
void configureMQTT() {
  client.setServer("192.168.100.170", 1883);
  client.setCallback(callback);
}

// MQTT callback - funkcja składająca wiadomości
void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  Serial.print("message arrived ["); Serial.print(topic); Serial.print("]: "); Serial.println(message);

  if (String(topic) == "/home/control/conveyorPWM") {
    int v = message.toInt();
    if (v >= -255 && v <= 255) {
      conveyorValue = v;
      int speed = abs(v);

      // L9110: kierunek + PWM realizowany na 2 pinach (bez pinów EN)
      if (v > 0) {
        analogWrite(LPWM_Output, speed);
        analogWrite(RPWM_Output, 0);
        Serial.print("Conveyor speed set to (forward): ");
        Serial.println(speed);
      } else if (v < 0) {
        analogWrite(LPWM_Output, 0);
        analogWrite(RPWM_Output, speed);
        Serial.print("Conveyor speed set to (reverse): ");
        Serial.println(speed);
      } else {
        analogWrite(LPWM_Output, 0);
        analogWrite(RPWM_Output, 0);
        Serial.println("Conveyor stopped");
      }
    } else {
      Serial.println("Invalid conveyor PWM received!");
    }
  }

  // Proste sterowanie LED ON/OFF przez MQTT
  if (String(topic) == "/home/control/light") {
    if (message == "OFF") {
      digitalWrite(LigtPin, OFF);
      Serial.println("Light turned OFF via MQTT");
    } else if (message == "ON") {
      digitalWrite(LigtPin, ON);
      Serial.println("Light turned ON via MQTT");
    } else {
      Serial.println("Invalid light command (expected 'ON' or 'OFF')");
    }
  }
  // Servo sterowane przez MQTT
  if (String(topic) == "/home/control/servo") {
    message.trim();
    if (message.startsWith("us:")) {
      int us = message.substring(3).toInt();
      if (us > 0) startMoveToUS(us);
      else Serial.println("Error. give us:500..2400");
    }
  } 
}

void setup() {
  delay(1000);
  Serial.begin(115200);

  Serial.println("Sterowanie: wpisz 0..180 lub us:500..2400 (ruch 3 s).");
  connectWiFi();
  configureMQTT();

  pinMode(LigtPin, OUTPUT);
  digitalWrite(LigtPin, OFF);     // light ON A-1A na L9110

  pinMode(RPWM_Output, OUTPUT);
  pinMode(LPWM_Output, OUTPUT);

  servoInit();
}

void loop() {
  if (!client.connected()) reconnectMQTT();
  client.loop();
  // Timer wyświetlający co sekundę ustawioną wartość PWM (serial monitor - debug)
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 1000) {
    lastPrint = millis();
    Serial.print("Conveyor PWM set to: ");
    Serial.println(conveyorValue);
  }
  updateServoMotion();           // Funkcja sterująca servo
  handleSerial();                // Dodana szeregowa komunikacja, obecnie nieużywana
}

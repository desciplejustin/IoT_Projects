#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.println();
  Serial.println("ESP8266 NodeMCU started");
}

void loop() {
  digitalWrite(LED_BUILTIN, LOW);  // LED on (active low)
  delay(500);
  digitalWrite(LED_BUILTIN, HIGH); // LED off
  delay(500);
  Serial.println("blink");
}

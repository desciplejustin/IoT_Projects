#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("IoT_Projects workspace root");
  Serial.println("Build from an individual project folder instead of the repository root.");
}

void loop() {
  delay(1000);
}

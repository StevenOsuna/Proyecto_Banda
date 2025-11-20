#include <Arduino.h>
#include "task_camara.h"

void setup()
{
  Serial.begin(115200);
  iniciarTaskCamara();
}

void loop()
{
  // Publicar resultados al ESP32-PLC
  mqttCam.publish(topic_color, color);
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", tamEstimado);
  mqttCam.publish(topic_tamano, buf);
}

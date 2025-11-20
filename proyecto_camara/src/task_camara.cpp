/* proyecto_camara/src/task_camara.cpp
   Firmware para ESP32-CAM (AiThinker / OV2640)
   - Captura imagen en RGB565
   - Muestrea pixeles para estimar promedio R,G,B
   - Publica por MQTT:
       topic "camara/color"  -> "rojo"/"verde"/"azul"/"desconocido"
       topic "camara/tamano" -> valor float (estimado, opcional)
   - Ligeramente optimizado para PSRAM si el módulo lo tiene
*/

#include "task_camara.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_camera.h"

// Ajusta estas credenciales al mismo WiFi del PLC
const char *ssid_cam = "INFINITUM7367_5";
const char *pass_cam = "GUagU5phw6";
const char *mqtt_server_cam = "192.168.1.70";
const char *topic_color = "plc/entrada/color";
const char *topic_tamano = "plc/entrada/tamano";
const char *topic_objeto = "plc/entrada/objeto";

WiFiClient espClientCam;
PubSubClient mqttCam(espClientCam);
TaskHandle_t taskCamaraHandle;

// Pines para el módulo AI-Thinker (OV2640)
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

void conectarMQTTCAM()
{
    while (!mqttCam.connected())
    {
        if (mqttCam.connect("ESP32_CAM_DEVICE"))
        {
            // nada por ahora
        }
        else
        {
            delay(500);
        }
    }
}

// Muestreo sencillo para promedio RGB desde frame RGB565
void calcularPromedioRGB(camera_fb_t *fb, uint32_t &R, uint32_t &G, uint32_t &B, int sampleStep = 10)
{
    R = G = B = 0;
    uint32_t count = 0;
    // RGB565: 2 bytes por pixel: (R:5,G:6,B:5)
    for (size_t i = 0; i + 1 < fb->len; i += 2 * sampleStep)
    {
        uint16_t pix = (uint16_t)fb->buf[i] | ((uint16_t)fb->buf[i + 1] << 8);
        uint8_t r5 = (pix >> 11) & 0x1F;
        uint8_t g6 = (pix >> 5) & 0x3F;
        uint8_t b5 = pix & 0x1F;
        // Expand to 8-bit approx
        uint32_t r8 = (r5 * 255) / 31;
        uint32_t g8 = (g6 * 255) / 63;
        uint32_t b8 = (b5 * 255) / 31;
        R += r8;
        G += g8;
        B += b8;
        count++;
    }
    if (count == 0)
        return;
    R /= count;
    G /= count;
    B /= count;
}

void taskCamara(void *parameter)
{
    // Configurar la cámara
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_RGB565; // RGB565 para muestreo
    // resolution: usar QQVGA o QVGA para balance velocidad/precisión
    config.frame_size = FRAMESIZE_QQVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;

    if (esp_camera_init(&config) != ESP_OK)
    {
        Serial.println("ERROR: No se pudo iniciar la camara");
        vTaskDelete(NULL);
        return;
    }
    Serial.println("Camara iniciada OK");

    // Conectar WiFi
    WiFi.begin(ssid_cam, pass_cam);
    Serial.print("Camara conectando Wifi");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000)
    {
        Serial.print(".");
        delay(200);
    }
    Serial.println("\nWiFi cam conectada");

    // Conectar MQTT
    mqttCam.setServer(mqtt_server_cam, 1883);
    conectarMQTTCAM();
    Serial.println("MQTT cam conectado");

    // Bucle principal
    for (;;)
    {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb)
        {
            Serial.println("fb null");
            vTaskDelay(500 / portTICK_PERIOD_MS);
            continue;
        }

        uint32_t R, G, B;
        calcularPromedioRGB(fb, R, G, B, 8); // muestreo: step = 8 para rapidez

        // Decide color dominante
        const char *color = "desconocido";
        if (R > G && R > B && R > 80)
            color = "rojo";
        else if (G > R && G > B && G > 80)
            color = "verde";
        else if (B > R && B > G && B > 80)
            color = "azul";

        // Estimación simple de tamaño (método: cantidad de "alto brillo" píxeles)
        // Nota: esto es aproximado. Mejor calibrar si es necesario.
        int tamEstimado = 0;
        // ejemplo simple: si R+G+B promedio alto -> más área iluminada -> asumimos "grande"
        uint32_t avg = (R + G + B) / 3;
        if (avg < 60)
            tamEstimado = 10;
        else if (avg < 120)
            tamEstimado = 30;
        else
            tamEstimado = 60;

        // Publicar resultados
        char payload[128];
        snprintf(payload, sizeof(payload),
                 "{\"tipo\":%d,\"color\":\"%s\",\"tamano\":%d,\"estado\":\"normal\"}",
                 tipoCajaActual, color, tamEstimado);

        mqttCam.publish("plc/entrada/objeto", payload);

        Serial.printf("Cam: R=%u G=%u B=%u -> %s, tam=%d, JSON enviado: %s\n",
                      R, G, B, color, tamEstimado, payload);

        esp_camera_fb_return(fb);
        vTaskDelay(800 / portTICK_PERIOD_MS); // ~1.2 fps
    }
}

void iniciarTaskCamara()
{
    xTaskCreatePinnedToCore(taskCamara, "taskCamara", 4096, NULL, 1, &taskCamaraHandle, 1);
}

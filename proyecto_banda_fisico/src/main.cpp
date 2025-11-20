/****************************************************
 *  PROYECTO: Banda transportadora tipo PLC con ESP32
 *  FUNCIONES:
 *   - Control de motor (PWM + dirección)
 *   - Conteo de objetos (ISR)
 *   - Límite de llenado de cajas
 *   - Comunicación MQTT
 *   - Desvío con servomotor
 *   - Tareas FreeRTOS
 ****************************************************/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <HTTPClient.h>

/*************** PINES *************************/
const int pinMotorPWM = 18;
const int pinMotorDir = 19;

const int pinServo = 5;

const int pinTrigger = 14;
const int pinEcho = 12;

const int pinBotonStart = 27;
const int pinSensorConteo = 26;

/*************** WIFI & MQTT ********************/
const char *ssid = "INFINITUM7367_5";
const char *password = "GUagU5phw6";

const char *mqtt_server = "7ec04495680644c1ae71385637191eb0.s1.eu.hivemq.cloud";

WiFiClient espClient;
PubSubClient client(espClient);

/*************** VARIABLES GLOBALES ************/
Servo servoDesvio;

// Estado motor
bool motorEstado = false;

// Conteo
volatile int contadorNormales = 0;
volatile int contadorDesviados = 0;

int limiteNormales = 10;
int limiteDesviados = 10;

int contadorCaja[2] = {0, 0}; // 2 cajas
int limiteCaja[2] = {10, 10};

bool paroPorLimite[2] = {false, false};

int tipoCajaActual = 0; // la caja donde irá el objeto actual
int proximaAccionDesvio = 0;

volatile int limiteAlcanzadoFlag = 0; // 1=normales, 2=desviados

// Desvío
volatile bool nuevoObjetoDetectado = false;
volatile int8_t proximaAccionDesvio = -1;

// camara
String ultimoColor = "desconocido";
String ultimoTamano = "desconocido";
String ultimoEstado = "normal";
int objetoPendienteActualizar = -1;

/*************** DECLARACIÓN DE FUNCIONES ********/
void IRAM_ATTR detectarConteo();
long medirDistanciaCM();
void desviarObjeto(bool activar);
void motorBanda(bool estado);
void callback(char *topic, byte *message, unsigned int length);
void reconnect();

// Tareas FreeRTOS
void Task_Sensores(void *pvParameters);
void Task_MQTT(void *pvParameters);

// Envíos MQTT
void enviarRegistroObjeto(int tipo);
void enviarEventoLimite(String caja, int total);

// ------------------------------------------------
//                 FUNCIONES
// ------------------------------------------------

/******** ISR DE CONTEO (NO MODIFICAR) ********/
void IRAM_ATTR detectarConteo()
{
  int8_t accion = proximaAccionDesvio;

  if (accion == 1) // desviados
  {
    contadorDesviados++;
    if (contadorDesviados >= limiteDesviados)
      limiteAlcanzadoFlag = 2;
  }
  else // normales
  {
    contadorNormales++;
    if (contadorNormales >= limiteNormales)
      limiteAlcanzadoFlag = 1;
  }

  proximaAccionDesvio = -1;
}

/******** MEDIR DISTANCIA ULTRASONICO ********/
long medirDistanciaCM()
{
  digitalWrite(pinTrigger, LOW);
  delayMicroseconds(2);
  digitalWrite(pinTrigger, HIGH);
  delayMicroseconds(10);
  digitalWrite(pinTrigger, LOW);

  long duracion = pulseIn(pinEcho, HIGH, 25000);
  return duracion * 0.034 / 2;
}

/******** SERVO DESVIADOR ********************/
void desviarObjeto(bool activar)
{
  if (activar)
    servoDesvio.write(90);
  else
    servoDesvio.write(0);
}

/******** MOTOR DE BANDA *********************/
void motorBanda(bool estado)
{
  motorEstado = estado;

  digitalWrite(pinMotorDir, HIGH);
  ledcWrite(0, estado ? 180 : 0);
}

/*********** CALLBACK MQTT ******************/
void callback(char *topic, byte *message, unsigned int length)
{
  String mensaje;

  for (int i = 0; i < length; i++)
    mensaje += (char)message[i];

  Serial.println("MQTT recibido en topic: " + String(topic) + " -> " + mensaje);

  // Control de la banda
  if (String(topic) == "banda/control")
  {
    if (mensaje == "start")
      motorBanda(true);
    else if (mensaje == "stop")
      motorBanda(false);
  }

  // Desvío de objetos
  if (String(topic) == "banda/desvio")
  {
    if (mensaje == "normal")
      proximaAccionDesvio = 0;
    else if (mensaje == "desviar")
      proximaAccionDesvio = 1;
  }

  // Datos de la cámara
  if (String(topic) == "plc/entrada/objeto")
  {
    StaticJsonDocument<128> doc;
    DeserializationError error = deserializeJson(doc, mensaje);

    if (!error)
    {
      int tipo = doc["tipo"] | 0;
      String color = doc["color"] | "desconocido";
      int tamano = doc["tamano"] | 0;
      String estado = doc["estado"] | "normal";

      Serial.println("Datos de camara recibidos (JSON):");
      Serial.println("TipoCaja: " + String(tipo));
      Serial.println("Color: " + color);
      Serial.println("Tamaño: " + String(tamano));
      Serial.println("Estado: " + estado);

      // Actualizar BD solo si hay objeto pendiente
      if (objetoPendienteActualizar != -1)
      {
        enviarObjetoBD(objetoPendienteActualizar, color, estado);
        objetoPendienteActualizar = -1;
      }
    }
  }
}

/*********** RECONEXIÓN MQTT *****************/
void reconnect()
{
  while (!client.connected())
  {
    Serial.print("Intentando conectar MQTT...");
    if (client.connect("bandaPLC"))
    {
      Serial.println("Conectado.");
      client.subscribe("banda/control");
      client.subscribe("banda/desvio");
      // Nuevas suscripciones para la ESP32-CAM
      client.subscribe("plc/entrada/color");  // color del objeto
      client.subscribe("plc/entrada/tamano"); // tamaño del objeto
      client.subscribe("plc/entrada/objeto");
    }
    else
    {
      Serial.print("Fallo, rc=");
      Serial.println(client.state());
      delay(1000);
    }
  }
}

/*********** Enviar registro individual ***********/
void enviarRegistroObjeto(int tipo)
{
  String payload = "{\"tipo\":";
  payload += tipo;
  payload += ",\"normales\":";
  payload += contadorNormales;
  payload += ",\"desviados\":";
  payload += contadorDesviados;
  payload += "}";

  client.publish("banda/registro", payload.c_str());
}

/*********** Enviar evento de límite ***********/
void enviarEventoLimite(String caja, int total)
{
  String payload = "{\"caja\":\"" + caja + "\",\"total\":" + String(total) + "}";
  client.publish("banda/alerta_limite", payload.c_str());
}

void enviarEventoLimiteBD(String evento, String caja, int total)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;
    http.begin("http://192.168.1.70/banda/guardar_evento.php"); // <-- IP de tu PC
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String datos = "evento=" + evento +
                   "&tipo_caja=" + caja +
                   "&contador_final=" + String(total);

    int codigo = http.POST(datos);

    Serial.println("BD respuesta evento: " + http.getString());
    http.end();
  }
}

void enviarObjetoBD(int tipo, String color, String estado)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;
    http.begin("http://192.168.1.70/banda/guardar_objeto.php"); // <-- IP de tu PC
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String datos = "tipo=" + String(tipo) +
                   "&color=" + color +
                   "&estado=" + estado;

    int codigo = http.POST(datos);

    Serial.println("BD respuesta objeto: " + http.getString());
    http.end();
  }
}

/*************** TAREA SENSORES (PLC) **************/
void Task_Sensores(void *pvParameters)
{
  for (;;)
  {
    long distancia = medirDistanciaCM();

    // ---- DETECCIÓN DE OBJETO ----
    if (distancia > 0 && distancia < 10) // objeto presente
    {
      detectarConteo(); // Bandera de nuevo objeto detectado

      objetoPendienteActualizar = tipoCajaActual;

      // ---- AUMENTAR CONTADOR ----
      contadorCaja[tipoCajaActual]++;

      Serial.print("Objeto detectado en caja: ");
      Serial.println(tipoCajaActual);

      // ---- DESVÍO DEL OBJETO ----
      if (proximaAccionDesvio == 1)
        desviarObjeto(true);
      else
        desviarObjeto(false);

      // ---- REVISAR LÍMITE ----
      if (contadorCaja[tipoCajaActual] >= limiteCaja[tipoCajaActual])
      {
        Serial.println(">>> LIMITE ALCANZADO <<<");

        enviarEventoLimite("Caja_" + String(tipoCajaActual),
                           contadorCaja[tipoCajaActual]);

        enviarEventoLimiteBD("Límite alcanzado",
                             "Caja_" + String(tipoCajaActual),
                             contadorCaja[tipoCajaActual]);

        paroPorLimite[tipoCajaActual] = true;
      }

      // Anti rebote para no contar doble
      vTaskDelay(300 / portTICK_PERIOD_MS);
    }

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

/*************** TAREA MQTT **********************/
void Task_MQTT(void *pvParameters)
{
  for (;;)
  {
    if (!client.connected())
      reconnect();

    client.loop();

    if (limiteAlcanzadoFlag != 0)
    {
      motorBanda(false);

      if (limiteAlcanzadoFlag == 1)
        enviarEventoLimite("normales", contadorNormales);
      enviarEventoLimiteBD("limite_alcanzado", "normales", contadorNormales);
      else if (limiteAlcanzadoFlag == 2)
          enviarEventoLimite("desviados", contadorDesviados);
      enviarEventoLimiteBD("limite_alcanzado", "desviados", contadorDesviados);

      limiteAlcanzadoFlag = 0;
    }

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// ------------------------------------------------
//                     SETUP
// ------------------------------------------------

void setup()
{
  Serial.begin(115200);

  // PWM motor
  ledcAttachPin(pinMotorPWM, 0);
  ledcSetup(0, 5000, 8);

  pinMode(pinMotorDir, OUTPUT);
  pinMode(pinTrigger, OUTPUT);
  pinMode(pinEcho, INPUT);

  pinMode(pinBotonStart, INPUT_PULLUP);

  // Servo
  servoDesvio.attach(pinServo);
  desviarObjeto(false);

  // Sensor de conteo
  pinMode(pinSensorConteo, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(pinSensorConteo), detectarConteo, FALLING);

  // WIFI
  WiFi.begin(ssid, password);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nWiFi conectado!");

  // MQTT
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  // Motor apagado
  motorBanda(false);

  // Tareas
  xTaskCreate(Task_Sensores, "Sensores", 3000, NULL, 1, NULL);
  xTaskCreate(Task_MQTT, "MQTT", 3000, NULL, 1, NULL);

  Serial.println("PLC ESP32 INICIADO.");
}

// ------------------------------------------------
//                     LOOP
// ------------------------------------------------

void loop()
{
  vTaskDelay(1000);
}

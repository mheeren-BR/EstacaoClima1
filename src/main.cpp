#include <Arduino.h>
#include <Adafruit_BMP085.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>
#include <Update.h>

#define DHT_PIN 4
#define DHT_TYPE DHT11
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
// #define SDA_PIN 21
// #define SCL_PIN 22
#endif

static const char *CONFIG_FILE = "/config.json";
static const char *FAILED_FILE = "/failed.json";
static const char *HOSTNAME = "estacao-clima";
static const char *FIRMWARE_VERSION = "1.1.0";
static const uint32_t WIFI_FALLBACK_TIMEOUT_SECONDS = 180;
static const uint32_t TIME_SYNC_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL;

struct AppConfig
{
  String apiUrl = "https://wheather-h5cvhjascmh7cnda.centralus-01.azurewebsites.net/api/InserirRegistro";
  String local = "Estacao 1";
  String apiKey = "Louvre";
  uint32_t readIntervalSeconds = 5;
  uint32_t sendIntervalMinutes = 30;
};

struct SensorReadings
{
  float dhtTemperature = NAN;
  float humidity = NAN;
  float bmpTemperature = NAN;
  float pressureHpa = NAN;
};

AppConfig config;
DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_BMP085 bmp;
WebServer server(80);

bool bmpReady = false;
uint32_t lastReadMs = 0;
uint32_t lastTimeSyncMs = 0;
time_t lastSendSlot = 0;
SensorReadings readings;
bool pendingFailures = false;

String urlEncode(const String &value)
{
  String encoded;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < value.length(); i++)
  {
    char c = value.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
    {
      encoded += c;
    }
    else
    {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

String apiUrlWithKey()
{
  if (config.apiKey.isEmpty())
    return config.apiUrl;
  String separator = config.apiUrl.indexOf('?') >= 0 ? "&" : "?";
  return config.apiUrl + separator + "apikey=" + urlEncode(config.apiKey);
}

String htmlEscape(const String &value)
{
  String escaped = value;
  escaped.replace("&", "&amp;");
  escaped.replace("\"", "&quot;");
  escaped.replace("<", "&lt;");
  escaped.replace(">", "&gt;");
  return escaped;
}

void applyConfigDefaults()
{
  if (config.readIntervalSeconds < 1)
    config.readIntervalSeconds = 5;
  if (config.sendIntervalMinutes < 1)
    config.sendIntervalMinutes = 30;
}

float formatDecimal2(float value)
{
  if (isnan(value))
    return 0;                         // retorna -1 se for NaN
  return roundf(value * 100) / 100.0; // arredonda para 2 casas decimais
}

bool saveConfig()
{
  JsonDocument doc;
  doc["apiUrl"] = config.apiUrl;
  doc["local"] = config.local;
  doc["apiKey"] = config.apiKey;
  doc["readIntervalSeconds"] = config.readIntervalSeconds;
  doc["sendIntervalMinutes"] = config.sendIntervalMinutes;
  doc["firmwareVersion"] = FIRMWARE_VERSION;

  File file = LittleFS.open(CONFIG_FILE, "w");
  if (!file)
    return false;
  serializeJsonPretty(doc, file);
  file.close();
  return true;
}

void loadConfig()
{
  if (!LittleFS.exists(CONFIG_FILE))
  {
    saveConfig();
    return;
  }

  File file = LittleFS.open(CONFIG_FILE, "r");
  if (!file)
    return;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error)
    return;

  config.apiUrl = doc["apiUrl"] | config.apiUrl;
  config.local = doc["local"] | config.local;
  config.apiKey = doc["apiKey"] | (doc["apiToken"] | config.apiKey);
  config.readIntervalSeconds = doc["readIntervalSeconds"] | config.readIntervalSeconds;
  config.sendIntervalMinutes = doc["sendIntervalMinutes"] | config.sendIntervalMinutes;
  applyConfigDefaults();
}

String formattedDateTime(time_t now)
{
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  return String(buffer);
}

String simplifiedTime(time_t now)
{
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buffer[17];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &timeinfo);
  return String(buffer);
}

bool syncTimeNow()
{
  configTime(-3 * 3600, 0, "a.ntp.br", "b.ntp.br", "pool.ntp.br");

  struct tm timeinfo;
  bool synced = getLocalTime(&timeinfo, 15000);
  if (synced)
  {
    lastTimeSyncMs = millis();
    Serial.println("Hora sincronizada via NTP brasileiro.");
  }
  else
  {
    Serial.println("Falha ao sincronizar hora.");
  }
  return synced;
}

bool internetAvailable()
{
  if (WiFi.status() != WL_CONNECTED)
    return false;

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  http.begin("http://clients3.google.com/generate_204");
  int code = http.GET();
  http.end();
  return code == 204 || (code >= 200 && code < 400);
}

JsonDocument makePayload(time_t now, const String &sensor, const String &parametro, float valor)
{
  JsonDocument doc;
  doc["data_hora"] = formattedDateTime(now);
  doc["hora_simplificada"] = simplifiedTime(now);
  doc["sensor"] = sensor;
  doc["local"] = config.local;
  doc["parametro"] = parametro;
  doc["valor"] = String(valor, 2);
  return doc;
}

bool postPayload(JsonDocument &payload)
{
  if (!internetAvailable())
    return false;
  if (config.apiUrl.isEmpty())
    return false;

  String body;
  serializeJson(payload, body);

  String url = apiUrlWithKey();

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(body);
  http.end();
  Serial.println("POST " + String(code));
  return code >= 200 && code < 300;
}

JsonArray loadFailureArray(JsonDocument &doc)
{
  if (!LittleFS.exists(FAILED_FILE))
    return doc.to<JsonArray>();

  File file = LittleFS.open(FAILED_FILE, "r");
  if (!file)
    return doc.to<JsonArray>();
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error || !doc.is<JsonArray>())
  {
    doc.clear();
    return doc.to<JsonArray>();
  }
  return doc.as<JsonArray>();
}

void saveFailureDocument(JsonDocument &doc)
{
  File file = LittleFS.open(FAILED_FILE, "w");
  if (!file)
    return;
  serializeJsonPretty(doc, file);
  file.close();
}

void enqueueFailure(JsonDocument &payload)
{
  JsonDocument doc;
  JsonArray failures = loadFailureArray(doc);
  JsonObject item = failures.add<JsonObject>();
  item.set(payload.as<JsonObject>());
  saveFailureDocument(doc);
  pendingFailures = true;
}

void resendFailures()
{
  if (!internetAvailable() || !LittleFS.exists(FAILED_FILE))
    return;

  JsonDocument doc;
  JsonArray failures = loadFailureArray(doc);
  if (failures.size() == 0)
    return;

  JsonDocument remaining;
  JsonArray remainingArray = remaining.to<JsonArray>();
  for (JsonObject failure : failures)
  {
    JsonDocument payload;
    payload.set(failure);
    if (!postPayload(payload))
    {
      JsonObject item = remainingArray.add<JsonObject>();
      item.set(failure);
    }
  }
  saveFailureDocument(remaining);
  pendingFailures = remainingArray.size() > 0;
}

void sendOrQueue(time_t now, const String &sensor, const String &parametro, float valor)
{
  if (isnan(valor))
    return;

  JsonDocument payload = makePayload(now, sensor, parametro, valor);
  if (!postPayload(payload))
  {
    enqueueFailure(payload);
  }
}

void sendCurrentReadings()
{
  time_t now = time(nullptr);
  if (now < 100000)
    return;

  resendFailures();
  sendOrQueue(now, "DHT11", "Temperatura", readings.dhtTemperature);
  sendOrQueue(now, "DHT11", "Umidade", readings.humidity);
  sendOrQueue(now, "BMP085", "Temperatura", readings.bmpTemperature);
  sendOrQueue(now, "BMP085", "Pressao", readings.pressureHpa);
}

void readSensors()
{
  readings.dhtTemperature = dht.readTemperature();
  readings.humidity = dht.readHumidity();

  if (bmpReady)
  {
    readings.bmpTemperature = bmp.readTemperature();
    readings.pressureHpa = bmp.readPressure() / 100.0F;
  }

  Serial.printf("DHT11: %.2f C / %.2f %% | BMP085: %.2f C / %.2f hPa\n",
                readings.dhtTemperature, readings.humidity,
                readings.bmpTemperature, readings.pressureHpa);
}

bool shouldSendNow(time_t now)
{
  if (now < 100000)
    return false;
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  uint32_t minuteOfDay = timeinfo.tm_hour * 60UL + timeinfo.tm_min;
  if (minuteOfDay % config.sendIntervalMinutes != 0 || timeinfo.tm_sec > 10)
    return false;

  time_t slot = now - timeinfo.tm_sec;
  if (slot == lastSendSlot)
    return false;

  lastSendSlot = slot;
  return true;
}

String loadTemplate(const char *path, const String &fallback)
{
  File file = LittleFS.open(path, "r");
  if (!file)
    return fallback;
  String content = file.readString();
  file.close();
  return content;
}

String lastReadingText()
{
  String text;
  text += "Hora: " + formattedDateTime(time(nullptr)) + "\n";
  text += "DHT11 T: " + String(readings.dhtTemperature, 2) + " C";
  text += " U: " + String(readings.humidity, 2) + " %\n";
  text += "BMP085 T: " + String(readings.bmpTemperature, 2) + " C";
  text += " P: " + String(readings.pressureHpa, 2) + " hPa";
  return text;
}

void refreshPendingFailures()
{
  if (!LittleFS.exists(FAILED_FILE))
  {
    pendingFailures = false;
    return;
  }
  JsonDocument doc;
  JsonArray failures = loadFailureArray(doc);
  pendingFailures = failures.size() > 0;
}

bool hasPendingFailures()
{
  return pendingFailures;
}

String ledStatusText()
{
  if (hasPendingFailures())
    return "1 piscada curta: falhas de envio pendentes";
  if (!internetAvailable())
    return "2 piscadas curtas: conexao indisponivel";
  if (time(nullptr) < 100000)
    return "3 piscadas curtas: hora nao sincronizada";
  if (!bmpReady)
    return "4 piscadas curtas: BMP085 nao encontrado";
  return "aceso fixo: operacao normal";
}

String renderIndexPage()
{
  String page = loadTemplate("/index.html", "<h1>Estacao Meteorologica</h1><p>Arquivo /index.html nao encontrado no LittleFS.</p>");
  page.replace("%HOSTNAME%", HOSTNAME);
  page.replace("%FIRMWARE_VERSION%", FIRMWARE_VERSION);
  page.replace("%API_URL%", htmlEscape(config.apiUrl));
  page.replace("%API_KEY%", htmlEscape(config.apiKey));
  page.replace("%LOCAL%", htmlEscape(config.local));
  page.replace("%READ_INTERVAL%", String(config.readIntervalSeconds));
  page.replace("%SEND_INTERVAL%", String(config.sendIntervalMinutes));
  page.replace("%LAST_READING%", htmlEscape(lastReadingText()));
  return page;
}

String renderLedPage()
{
  String page = loadTemplate("/led.html", "<h1>Padroes do LED</h1><p>Arquivo /led.html nao encontrado no LittleFS.</p>");
  page.replace("%LED_STATUS%", htmlEscape(ledStatusText()));
  page.replace("%FIRMWARE_VERSION%", FIRMWARE_VERSION);
  return page;
}

String renderOtaPage()
{
  String page = loadTemplate("/ota.html", "<h1>Atualizacao OTA</h1><p>Arquivo /ota.html nao encontrado no LittleFS.</p>");
  page.replace("%FIRMWARE_VERSION%", FIRMWARE_VERSION);
  return page;
}

enum LedPattern
{
  LED_NORMAL,
  LED_SEND_FAILURES,
  LED_CONNECTION_FAILURE,
  LED_TIME_FAILURE,
  LED_SENSOR_FAILURE
};

LedPattern currentLedPattern()
{
  if (hasPendingFailures())
    return LED_SEND_FAILURES;
  if (WiFi.status() != WL_CONNECTED)
    return LED_CONNECTION_FAILURE;
  if (time(nullptr) < 100000)
    return LED_TIME_FAILURE;
  if (!bmpReady)
    return LED_SENSOR_FAILURE;
  return LED_NORMAL;
}

void updateStatusLed()
{
  static uint32_t lastMs = 0;
  static uint8_t step = 0;
  static LedPattern previousPattern = LED_NORMAL;

  LedPattern pattern = currentLedPattern();
  if (pattern != previousPattern)
  {
    previousPattern = pattern;
    step = 0;
    lastMs = 0;
  }

  if (pattern == LED_NORMAL)
  {
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  uint8_t pulses = static_cast<uint8_t>(pattern);
  uint32_t nowMs = millis();
  if (nowMs - lastMs < 180)
    return;
  lastMs = nowMs;

  const uint8_t maxSteps = 12;
  if (step < pulses * 2)
  {
    digitalWrite(LED_BUILTIN, step % 2 == 0 ? HIGH : LOW);
  }
  else
  {
    digitalWrite(LED_BUILTIN, LOW);
  }
  step = (step + 1) % maxSteps;
}

void handleFirmwareUpload()
{
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START)
  {
    Serial.printf("Iniciando OTA: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
    {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
    {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (Update.end(true))
    {
      Serial.printf("OTA concluido: %u bytes\n", upload.totalSize);
    }
    else
    {
      Update.printError(Serial);
    }
  }
}

void clearPendingFailures()
{
  if (LittleFS.exists(FAILED_FILE))
  {
    LittleFS.remove(FAILED_FILE);
  }
  pendingFailures = false;
  Serial.println("Falhas de envio pendentes excluidas.");
}

void setupWebServer()
{
  server.on("/", HTTP_GET, []()
            { server.send(200, "text/html", renderIndexPage()); });
  server.on("/led.html", HTTP_GET, []()
            { server.send(200, "text/html", renderLedPage()); });
  server.on("/ota.html", HTTP_GET, []()
            { server.send(200, "text/html", renderOtaPage()); });
  server.on("/update", HTTP_POST, []()
            {
    bool success = !Update.hasError();
    server.sendHeader("Connection", "close");
    server.send(success ? 200 : 500, "text/plain", success ? "Atualizacao concluida. Reiniciando..." : "Falha na atualizacao OTA.");
    if (success) {
      delay(1000);
      ESP.restart();
    } }, handleFirmwareUpload);
  server.on("/config.json", HTTP_GET, []()
            {
    File file = LittleFS.open(CONFIG_FILE, "r");
    if (!file) {
      server.send(404, "application/json", "{}");
      return;
    }
    server.streamFile(file, "application/json");
    file.close(); });
  server.on("/failed.json", HTTP_GET, []()
            {
    File file = LittleFS.open(FAILED_FILE, "r");
    if (!file) {
      server.send(404, "application/json", "{}");
      return;
    }
    server.streamFile(file, "application/json");
    file.close(); });
  server.on("/clear-failures", HTTP_POST, []()
            {
    clearPendingFailures();
    server.sendHeader("Location", "/");
    server.send(303); });
  server.on("/save", HTTP_POST, []()
            {
    config.apiUrl = server.arg("apiUrl");
    config.local = server.arg("local");
    config.apiKey = server.arg("apiKey");
    config.readIntervalSeconds = server.arg("readIntervalSeconds").toInt();
    config.sendIntervalMinutes = server.arg("sendIntervalMinutes").toInt();
    applyConfigDefaults();
    saveConfig();
    server.sendHeader("Location", "/");
    server.send(303); });
  server.begin();
}

void setupWifi()
{
  WiFi.mode(WIFI_STA);
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(WIFI_FALLBACK_TIMEOUT_SECONDS);
  bool connected = wifiManager.autoConnect("EstacaoClima-Setup");
  if (!connected)
  {
    Serial.println("Timeout do portal WiFi. Reiniciando...");
    ESP.restart();
  }
}

void setup()
{
  Serial.begin(115200);
  //Wire.begin(SDA_PIN, SCL_PIN);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  LittleFS.begin(true);
  loadConfig();
  refreshPendingFailures();

  dht.begin();
  bmpReady = bmp.begin();
  if (!bmpReady)
    Serial.println("BMP085 nao encontrado.");

  setupWifi();
  syncTimeNow();

  if (MDNS.begin(HOSTNAME))
  {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS ativo: http://%s.local\n", HOSTNAME);
  }
  setupWebServer();
  readSensors();
  // clearPendingFailures();
}

void loop()
{
  server.handleClient();
  updateStatusLed();

  uint32_t nowMs = millis();
  if (nowMs - lastReadMs >= config.readIntervalSeconds * 1000UL)
  {
    lastReadMs = nowMs;
    readSensors();
  }

  if (nowMs - lastTimeSyncMs >= TIME_SYNC_INTERVAL_MS)
  {
    syncTimeNow();
  }

  time_t now = time(nullptr);
  if (shouldSendNow(now))
  {
    sendCurrentReadings();
  }
}

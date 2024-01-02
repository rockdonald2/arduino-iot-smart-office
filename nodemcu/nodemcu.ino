#include <ESP8266WiFi.h>
#include <SPI.h>
#include <ESP32_Supabase.h>
#include <ArduinoJson.h>
#include <arduino-timer.h>

static const String SUPA_URL = "https://<redacted>.supabase.co";
static const String SUPA_ANON_KEY = "<redacted>";
static const String SUPA_MEASUREMENT_TABLE = "measurements";
static const String SUPA_COMMANDS_TABLE = "commands";

static const char *SSID = "HUAWEI-J1BD2C";
static const char *PASSWORD = "<redacted>";

SPISettings spi_settings(100000, MSBFIRST, SPI_MODE0);
WiFiServer server(80);
Supabase db;
auto timer = timer_create_default();

struct Data {
  uint8_t temperature;
  uint8_t humidity;
  float lightness;
  bool shouldWrite;
};

volatile Data data = { 255, 255, -1, false };

struct Command {
  int id;
  uint8_t code;
};

volatile Command cmd = { -1, 255 };

static const byte CMD_AWAIT = 0xFF;
static const byte CMD_CHECKSUM = 0xFE;
static const byte CMD_GETTEMP = 0x01;
static const byte CMD_GETHUM = 0x02;
static const byte CMD_GETLIGHT = 0x03;

static const byte CMD_ROLL = 0x80;

void setup() {
  Serial.println(F("[cfg]------------"));
  Serial.begin(9600);
  SPI.begin();
  delay(10);
  Serial.println('\n');

  connectToWifi();
  startHttpServer();
  connectToSupabase();

  timer.every(500, handleHttpClient);              // look for HTTP clients every 500ms
  timer.every(1000, handleSpiCommunication);       // request data every 1s and other SPI tasks
  timer.every(15 * 1000, saveDataToDB);            // save data to DB every 15s
  timer.every(10 * 1000, checkForRemoteCommands);  // check for remote commands from UI every 10s

  Serial.println(F("[cfg]------------"));
}

void loop() {
  timer.tick();
}

void connectToSupabase() {
  db.begin(SUPA_URL, SUPA_ANON_KEY);
}

void connectToWifi() {
  WiFi.begin(SSID, PASSWORD);

  Serial.print(F("Connecting to "));
  Serial.print(SSID);
  Serial.println(F(" ..."));

  long i = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(++i);
    Serial.println(F(". retry"));
  }

  Serial.println();
  Serial.println(F("Connection established to WiFi!"));
  Serial.print(F("SSID is "));
  Serial.println(SSID);
  Serial.print(F("IP address: "));
  Serial.println(WiFi.localIP());

  WiFi.setAutoReconnect(true);
}

void startHttpServer() {
  server.begin();
  Serial.println(F("Server started"));

  Serial.print(F("Use this URL to connect: "));
  Serial.print(F("http://"));
  Serial.print(WiFi.localIP());
  Serial.println(F("/"));
}

bool handleHttpClient(void *argument) {
  WiFiClient client = server.accept();
  if (!client) {
    return true;
  }

  Serial.println(F("New client on HTTP interface"));
  while (!client.available()) {
    delay(1);
  }

  Serial.println(F("Requesting"));
  String request = client.readStringUntil('\r');
  Serial.println(request);
  client.flush();

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("");  // to separate headers and body
  client.println("<!DOCTYPE HTML>");
  client.println("<html>");
  client.println("<body>");
  client.println("In the office:");
  client.println("<ul>");
  client.print("<li>Temperature is ");
  client.print(data.temperature, DEC);
  client.println(" celsius</li>");
  client.print("<li>Humidity is ");
  client.print(data.humidity, DEC);
  client.println(" %</li>");
  client.print("<li>Lightness is ");
  client.print(data.lightness);
  client.println(" lux</li>");
  client.println("</ul>");
  client.println("</body>");
  client.println("</html>");

  delay(1);
  Serial.println(F("Client disonnected"));
  Serial.println(F(""));

  return true;
}

bool handleSpiCommunication(void *argument) {
  // if there is any command to execute, we need to pause collecting the data

  if (cmd.id != -1) {
    Serial.println(F("Skipping data collection as there is a remote command to execute"));
    handleRemoteCommand();
    return true;
  }

  Serial.println(F("Collecting data"));
  handleData();
  return true;
}

void handleData() {
  SPI.beginTransaction(spi_settings);

  sendSpiGetTempCmd();                 // request for temp
  uint8_t temp = sendSpiGetHumCmd();   // request for hum
  uint8_t hum = sendSpiGetLightCmd();  // request for 0th light

  uint8_t rawLight[4];
  rawLight[0] = sendSpiGetLightCmd();
  rawLight[1] = sendSpiGetLightCmd();
  rawLight[2] = sendSpiGetLightCmd();
  rawLight[3] = sendSpiChecksum();

  uint8_t checksum = sendSpiAwait();

  SPI.endTransaction();

  uint8_t validation;
  validation = temp ^ hum;
  validation ^= rawLight[0];
  validation ^= rawLight[1];
  validation ^= rawLight[2];
  validation ^= rawLight[3];

  Serial.print(F("Received checksum: "));
  Serial.println(checksum, DEC);
  Serial.print(F("Calculated validation checksum: "));
  Serial.println(validation, DEC);

  if (checksum == validation) {
    data.temperature = temp;
    data.humidity = hum;

    float f;
    memcpy(&f, rawLight, 4);
    data.lightness = f;

    Serial.print(F("Received temperature: "));
    Serial.println(data.temperature);
    Serial.print(F("Received humidity: "));
    Serial.println(data.humidity);
    Serial.print(F("Received lightness: "));
    Serial.println(data.lightness);

    data.shouldWrite = true;
  } else {
    Serial.println(F("Checksum validation failed, dismissing values"));
    data.shouldWrite = false;
  }
}

void handleRemoteCommand() {
  SPI.beginTransaction(spi_settings);

  if (cmd.code == CMD_ROLL) {
    sendSpiRollShadeCmd();
  } else {
    // what to do if invalid command comes in
    // it should be handled by the normal flow below, anyway the verdict won't be 0
  }

  uint8_t verdict = sendSpiAwait();

  SPI.endTransaction();

  // contract is to return hexa 0 which means success
  if (verdict == 0x00) {
    Serial.print(F("Remote command execution was successful for command code: "));
    Serial.println(cmd.code, DEC);

    int retCode = db.update(SUPA_COMMANDS_TABLE).eq("id", String(cmd.id)).doUpdate("{\"is_completed\": true}");
    db.urlQuery_reset();

    Serial.print(F("Received status code after remote command update: "));
    Serial.println(retCode);

    if (!isHttpOk(retCode)) {
      Serial.println(F("Something went wrong"));
    }
  } else {
    Serial.print(F("Remote command failed for command code: "));
    Serial.println(cmd.code, DEC);

    // we should delete it, or let it rerun?
  }

  cmd.id = -1;
  cmd.code = 255;
}

uint8_t sendSpiRollShadeCmd() {
  Serial.println(F("Sending roll shade command to SPI interface"));
  return SPI.transfer(CMD_ROLL);
}

uint8_t sendSpiGetTempCmd() {
  Serial.println(F("Sending get temp data command to SPI interface"));
  return SPI.transfer(CMD_GETTEMP);
}

uint8_t sendSpiGetHumCmd() {
  Serial.println(F("Sending get humidity data command to SPI interface"));
  return SPI.transfer(CMD_GETHUM);
}

uint8_t sendSpiGetLightCmd() {
  Serial.println(F("Sending get lightness data command to SPI interface"));
  return SPI.transfer(CMD_GETLIGHT);
}

uint8_t sendSpiChecksum() {
  Serial.println(F("Sending get checksum command to SPI interface"));
  return SPI.transfer(CMD_CHECKSUM);
}

uint8_t sendSpiAwait() {
  return SPI.transfer(CMD_AWAIT);
}

bool isHttpOk(int code) {
  return ((int)(code / 100)) == 2;
}

bool saveDataToDB(void *argument) {
  if (!data.shouldWrite) {
    Serial.println(F("Skipping writing data as shouldWrite flag is false"));
    return true;
  }

  Serial.println(F("Trying to save all the measurements into Supabase"));
  StaticJsonDocument<64> doc;

  if (data.temperature != 255) {
    doc["temperature"] = data.temperature;
  } else {
    doc["temperature"] = nullptr;
  }

  if (data.humidity != 255) {
    doc["humidity"] = data.humidity;
  } else {
    doc["humidity"] = nullptr;
  }

  if (isnormal(data.lightness)) {
    doc["lightness"] = data.lightness;
  } else {
    doc["lightness"] = nullptr;
  }

  String json;
  serializeJson(doc, json);

  int retCode = db.insert(SUPA_MEASUREMENT_TABLE, json, false);
  db.urlQuery_reset();

  Serial.print(F("Received status code: "));
  Serial.println(retCode);

  if (!isHttpOk(retCode)) {
    Serial.println(F("Something went wrong"));
  }

  return true;
}

bool checkForRemoteCommands(void *argument) {
  String queryResult = db.from(SUPA_COMMANDS_TABLE)
                         .select("id,command_code")
                         .eq("is_completed", "false")
                         .order("created_at", "asc", true)
                         .limit(1)
                         .doSelect();

  db.urlQuery_reset();

  if (queryResult.isEmpty()) {
    Serial.println(F("Nothing to execute remotely"));
    return true;
  }

  Serial.print(F("Received remote command: "));
  Serial.println(queryResult);

  StaticJsonDocument<96> queryDoc;
  DeserializationError error = deserializeJson(queryDoc, queryResult);

  if (error) {
    Serial.print(F("Deserialization of remote commands failed: "));
    Serial.println(error.c_str());
    return true;
  }

  if (queryDoc.containsKey("message")) {
    Serial.print(F("Remote command query returned with an error: "));
    const char *error = queryDoc["message"];
    Serial.println(error);
    return true;
  }

  JsonArray commands = queryDoc.as<JsonArray>();

  if (commands.size() == 0) {
    Serial.println(F("Nothing to execute remotely"));
    return true;
  }

  const char *commandCode = commands[0]["command_code"];  // "0x80"
  int commandId = commands[0]["id"];

  unsigned long convertedCommandCode = strtoul(commandCode, NULL, 16);

  cmd.id = commandId;

  if (convertedCommandCode <= UINT8_MAX) {
    cmd.code = (uint8_t)convertedCommandCode;
  } else {
    Serial.println(F("Invalid command received, skipping"));
    cmd.id = -1;
    cmd.code = 255;
  }

  return true;
}
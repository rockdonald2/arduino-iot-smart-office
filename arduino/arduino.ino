#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_TSL2561_U.h>
#include <LiquidCrystal_I2C.h>
#include <RtcDS3231.h>
#include <SimpleDHT.h>
#include <Stepper.h>
#include <SPI.h>

const int lcdSize = 21;
const int stepsPerRevolution = 32;
int stepCount = 0;

bool isAutoShade = true;
bool isShadeUp = true;
unsigned long shadePausedFor = 0;

const unsigned long currLoopDelay = 1;  // in seconds

const int warmWeatherThres = 20;
const int lightnessThres = 200;
const int humidityThres = 50;
const int pausedForThres = 60;

bool isFirstLcdWriteDone = false;
bool wasTempError = false;
bool wasLightError = false;

// float is 4 byte
struct Spi_Float {
  float value;
  uint8_t bytes[4];
  uint8_t transferred;
};
volatile Spi_Float spiLight = { 0.0, { 0 }, 0 };  // we could live without this,
// but we will use this to store what is the value we are sending and how many bytes we have transferred already

volatile uint8_t spiChecksum = 0;

static const byte CMD_AWAIT = 0xFF;
static const byte CMD_CHECKSUM = 0xFE;
static const byte CMD_GETTEMP = 0x01;
static const byte CMD_GETHUM = 0x02;
static const byte CMD_GETLIGHT = 0x03;
static const byte CMD_GETSHADEPOS = 0x04;

static const byte CMD_ROLL = 0x80;

struct Data {
  int temperature;
  int humidity;
  float lightness;
  uint8_t switchState;
};

volatile Data data = { 255, 255, -1.0, 0 };  // this will hold the last successful measurement
Data prevData = { 255, 255, -1.0, 0 };       // this will hold the last read (measurement), in case of anomalies it may hold the current (possibly) wrong data
volatile uint8_t currHour;                   // this will hold the currently read hour mark from the RTC

volatile bool switchInited = false;

// some globals for clarity
SimpleDHT11 dht11(A3);                                                               // it is wired at pin A3, temp sensor
LiquidCrystal_I2C lcd(0x27, 20, 4);                                                  // lcd screen
Adafruit_TSL2561_Unified tsl = Adafruit_TSL2561_Unified(TSL2561_ADDR_FLOAT, 12345);  // light sensor
RtcDS3231<TwoWire> rtc(Wire);                                                        // two wire protocol with real time clock component
SPISettings spi_settings(100000, MSBFIRST, SPI_MODE0);

Stepper stepper(stepsPerRevolution, 8, 6, 5, 4);

// calculates the char length of a char string
#define countof(a) (sizeof(a) / sizeof(a[0]))

// ran once
void setup(void) {
  Serial.begin(9600);

  Serial.println(F("[Light snr cfg]"));
  // Setup the sensor gain and integration time
  configureLightSensor();
  // problems: doesn't exist, is saturated should be handled

  Serial.println(F("[Lcd cfg]"));
  // configure lcd and clear it
  configureLcd();
  // problems: define an own character 5x8 matrix

  Serial.println(F("[RTC cfg]"));
  // configure rtc and check its configuration
  configureRtc();
  // problems: doesn't exist, is not configured (and the node mcu configures it using web connection)

  // for temp sensor no prev config needed

  // configure switch
  configureSwitch();

  // configure SPI
  configureSPI();
}

// ran in loop
void loop(void) {
  // clear LCD screen to prevent overflows

  Serial.println(F("[Light ev]"));
  lightEvent();

  Serial.println(F("[Rtc ev]"));
  rtcEvent();

  Serial.println(F("[Tmp ev]"));
  tempEvent();
  // problems: doesn't exist, throws error the checksum

  Serial.println(F("[Switch ev]"));
  data.switchState = getSwitchValue();
  Serial.print(F("Switch value is "));
  Serial.println(data.switchState);
  if (!switchInited) {
    prevData.switchState = data.switchState;
    switchInited = true;
  }  // to get them on the same page

  if (data.switchState != prevData.switchState) {
    changeShadeMode(false);
    prevData.switchState = data.switchState;
  }

  lcd.setCursor(13, 1);
  if (isAutoShade) {
    lcd.print("A");
  } else {
    lcd.print("M");
  }

  Serial.println(F("[Motor ev]"));
  motorEvent();

  if (!isFirstLcdWriteDone) {
    isFirstLcdWriteDone = true;
  }

  Serial.println(F("---"));

  // wait a second than loop it again
  delay(currLoopDelay * 1000);
  // sample taking should be 1Hz, once a second
  // the lcd should the display the switch as well, and all the errors

  // we should make a basic decision based on the collected data
  // if very dark, very light, etc. stop the motor etc.
  // if there were a manual override, it should remain set for a short period (e.g., 10 mins)
}

ISR(SPI_STC_vect) {
  byte command = SPDR;
  executeCommand(command);
}

void executeCommand(byte command) {
  // apparently you cannot put Serial calls into this, it will mess up the communication

  if (command == CMD_GETTEMP) {
    SPDR = data.temperature;

    updateChecksum(data.temperature);
  } else if (command == CMD_GETHUM) {
    SPDR = data.humidity;

    updateChecksum(data.humidity);
  } else if (command == CMD_GETLIGHT) {
    // when the first request comes in and the spiLight is uninitialized
    if (spiLight.transferred == 0) {
      spiLight.value = data.lightness;

      // break up float into 4 bytes with memcpy
      memcpy(spiLight.bytes, &spiLight.value, 4);
    }

    // send the nth byte
    updateChecksum(spiLight.bytes[spiLight.transferred]);
    SPDR = spiLight.bytes[spiLight.transferred++];

    // safety measure, shouldn't happen
    if (spiLight.transferred > 3) {
      spiLight.transferred = 0;
    }
  } else if (command == CMD_GETSHADEPOS) {
    SPDR = isShadeUp ? 0x01 : 0x00;

    updateChecksum(isShadeUp ? 0x01 : 0x00);
  } else if (command == CMD_CHECKSUM) {
    SPDR = spiChecksum;
  } else if (command == CMD_ROLL) {
    changeShadeMode(false);

    SPDR = 0x00;

    // no need to update checksum, this is a remote command
  } else {
    SPDR = CMD_AWAIT;

    spiChecksum = 0;
    // reset the sent lightness bytes, because we finished that command probably
    if (spiLight.transferred != 0) {
      spiLight.transferred = 0;
    }
  }
}

void updateChecksum(uint8_t value) {
  if (spiChecksum == 0) {
    spiChecksum = value;
  } else {
    spiChecksum ^= value;
  }
}

void rotateMotor() {
  stepCount = 0;

  for (int i = 0; i < 320; i++) {
    stepper.step(5);
    stepCount++;
    delay(10);
  }
}

void motorEvent() {
  clearLcdBlock(1, 15, 17);
  // the arduino makes its own decisions about the shade
  if (isAutoShade) {
    Serial.println(F("Auto shade mode is on"));

    // TODO: mi tortenik, ha 180-220 kozott ugral pl. a fenyerosseg
    if (data.lightness >= lightnessThres && isShadeUp && (currHour <= 20 && currHour >= 8)) {
      Serial.println(F("Rolling down shade as it is bright and daytime"));
      lcd.setCursor(16, 1);
      lcd.write(3);
      isShadeUp = false;
      rotateMotor();
    } else if (data.lightness < lightnessThres && !isShadeUp && (currHour <= 20 && currHour >= 8)) {
      Serial.println(F("Rolling up shade as it is dark and daytime"));
      lcd.setCursor(16, 1);
      lcd.write(2);
      isShadeUp = true;
      rotateMotor();
    } else if ((currHour > 20 || currHour < 8) && isShadeUp) {
      Serial.println(F("Rolling down shade as it is nighttime"));
      lcd.setCursor(16, 1);
      lcd.write(3);
      isShadeUp = false;
      rotateMotor();
    }
  } else {
    Serial.println(F("Auto shade mode is off, manual override"));
    // override the decision, if up lower, if down turn up

    // == 0 means we're on the first manual override, after this we just wait for the time to pass
    if (shadePausedFor == 0) {
      if (isShadeUp) {
        Serial.println(F("Rolling down shade on manual mode"));
        lcd.setCursor(16, 1);
        lcd.write(3);
        isShadeUp = false;
        rotateMotor();
      } else {
        Serial.println(F("Rolling up shade on manual mode"));
        lcd.setCursor(16, 1);
        lcd.write(2);
        isShadeUp = true;
        rotateMotor();
      }
    }

    shadePausedFor += currLoopDelay;
    Serial.print(F("Auto shade paused for: "));
    Serial.println(shadePausedFor);

    if (shadePausedFor > pausedForThres) {  // 60 * 1s
      changeShadeMode(true);
    }
  }
}

void outputDateTime(const RtcDateTime &dt) {
  char datestring[26];

  // construct proper formatted string
  snprintf_P(datestring,
             countof(datestring),
             PSTR("%02u/%02u/%04u  %02u:%02u:%02u"),
             dt.Day(),
             dt.Month(),
             dt.Year(),
             dt.Hour(),
             dt.Minute(),
             dt.Second());

  // output to lcd and serial
  lcd.print(datestring);
  Serial.print(datestring);
}

// handy routine to return true if there was an error
// but it will also print out an error message with the given topic
bool wasRtcError(const char *errorTopic = "") {
  uint8_t error = rtc.LastError();
  if (error != 0) {
    // we have a communications error
    Serial.print(F("["));
    Serial.print(errorTopic);
    Serial.print(F("] Comm error ("));
    Serial.print(error);
    Serial.print(F(") : "));

    switch (error) {
      case Rtc_Wire_Error_None:
        Serial.println(F("(none?!)"));
        break;
      case Rtc_Wire_Error_TxBufferOverflow:
        Serial.println(F("transmit buffer overflow"));
        break;
      case Rtc_Wire_Error_NoAddressableDevice:
        Serial.println(F("no device responded"));
        break;
      case Rtc_Wire_Error_UnsupportedRequest:
        Serial.println(F("device doesn't support request"));
        break;
      case Rtc_Wire_Error_Unspecific:
        Serial.println(F("unspecified error"));
        break;
      case Rtc_Wire_Error_CommunicationTimeout:
        Serial.println(F("communications timed out"));
        break;
    }
    return true;
  }
  return false;
}

// Configures the gain and integration time for the TSL2561 sensor (light)
void configureLightSensor(void) {
  tsl.enableAutoRange(true);
  tsl.setIntegrationTime(TSL2561_INTEGRATIONTIME_13MS);
}

void configureLcd(void) {
  lcd.begin(20, 4);
  lcd.init();  // initialize the lcd
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 1);
  lcd.print("Shade sys.");

  uint8_t daySym[] = { 0x00, 0x00, 0x0E, 0x1F, 0x1B, 0x1F, 0x0E, 0x00 };
  uint8_t nightSym[] = { 0x00, 0x04, 0x15, 0x0E, 0x1B, 0x0E, 0x15, 0x04 };
  uint8_t arrowUpSym[] = { 0x04, 0x0E, 0x15, 0x15, 0x04, 0x04, 0x04, 0x04 };
  uint8_t arrowDwnSym[] = { 0x04, 0x04, 0x04, 0x04, 0x15, 0x15, 0x0E, 0x04 };
  lcd.createChar(0, nightSym);
  lcd.createChar(1, daySym);
  lcd.createChar(2, arrowUpSym);
  lcd.createChar(3, arrowDwnSym);
}

void configureRtc(void) {
  rtc.Begin();
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(3000 /* us */, true /* reset_on_timeout */);
#endif

  RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
  outputDateTime(compiled);  // to check first time configuration, but it is necessary to configure the lcd first because of this call
  Serial.println();

  lcd.setCursor(0, 0);

  if (!rtc.IsDateTimeValid()) {
    if (!wasRtcError("setup IsDateTimeValid")) {
      // Common Causes:
      //    1) first time you ran and the device wasn't running yet
      //    2) the battery on the device is low or even missing

      Serial.println(F("RTC lost confidence in the DateTime!"));

      // following line sets the RTC to the date & time this sketch was compiled
      // it will also reset the valid flag internally unless the Rtc device is
      // having an issue

      rtc.SetDateTime(compiled);
    } else {
      lcd.print("00/00/0000  00:00:00");
    }
  }

  if (!rtc.GetIsRunning()) {
    if (!wasRtcError("setup GetIsRunning")) {
      Serial.println(F("RTC was not actively running, starting now"));
      rtc.SetIsRunning(true);
    } else {
      lcd.print("00/00/0000  00:00:00");
    }
  }

  RtcDateTime now = rtc.GetDateTime();
  if (!wasRtcError(("setup GetDateTime"))) {
    if (now < compiled) {
      Serial.println(F("RTC is older than compile time, updating DateTime"));
      rtc.SetDateTime(compiled);
    } else if (now > compiled) {
      Serial.println(F("RTC is newer than compile time, this is expected"));
    } else if (now == compiled) {
      Serial.println(F("RTC is the same as compile time, while not expected all is still fine"));
    }
  }

  // never assume the Rtc was last configured by you, so
  // just clear them to your needed state
  rtc.Enable32kHzPin(false);
  wasRtcError(("setup Enable32kHzPin"));
  rtc.SetSquareWavePin(DS3231SquareWavePin_ModeNone);
  wasRtcError(("setup SetSquareWavePin"));
}

void configureSwitch(void) {
  // we need to use input_pullup, otherwise it is a dead wire with not defined voltage
  pinMode(A0, INPUT_PULLUP);
}

bool testLightSensor(void) {
  return tsl.begin();
}

void clearLcdBlock(int rowIdx, int startColIdx, int endColIdx) {
  lcd.setCursor(startColIdx, rowIdx);
  for (int i = startColIdx; i <= endColIdx; ++i) {
    lcd.print(" ");
  }
}

void lightEvent(void) {
  const float acceptedLightnessDiff = 200.0;
  lcd.setCursor(0, 2);

  if (!testLightSensor()) {
    Serial.println(F("Probably the TSL2561 sensor is missing from the board"));
    lcd.print("err lght         ");
    wasLightError = true;
    // set initial data
    data.lightness = -1;
    return;
  }

  sensors_event_t lightEvent;  // get new light sensor event
  bool readingStatus = tsl.getEvent(&lightEvent);
  float current = lightEvent.light;

  if (data.lightness == -1) {
    data.lightness = current;
  }

  if (readingStatus && current) {
    const float absDiff = abs(current - data.lightness);

    if (absDiff > acceptedLightnessDiff) {
      Serial.print(F("Anomaly found, lightness value difference was above threshold: "));
      Serial.println(absDiff);

      if (prevData.lightness == -1) {
        prevData.lightness = current;
      } else {
        const float absDiffAnomaly = abs(current - prevData.lightness);

        // we try to enforce relative error of 20%
        // if the new measurement is within 20% error of the old, it is considered acceptable

        if ((absDiffAnomaly / prevData.lightness) <= 0.2) {
          prevData.lightness = data.lightness;
          data.lightness = current;
          Serial.println(F("Previous measurement proved it is correct"));
        } else {
          prevData.lightness = current;
        }
      }
    } else {
      prevData.lightness = data.lightness;
      data.lightness = current;
    }

    if ((prevData.lightness != data.lightness) || !isFirstLcdWriteDone || wasLightError) {
      clearLcdBlock(2, 0, 12);
      wasLightError = false;

      lcd.setCursor(0, 2);
      Serial.print(data.lightness);
      Serial.println(" lux");

      lcd.print(data.lightness);
      lcd.print("lux");

      clearLcdBlock(2, 13, 19);
      lcd.setCursor(12, 2);
      if (data.lightness > lightnessThres) {
        lcd.print("sunny");
      } else {
        lcd.print("dark ");
      }
    } else {
      Serial.println(F("Skipping lcd rewrite, same measurement"));
    }
  } else {
    /* If event.light = 0 lux the sensor is probably saturated
           and no reliable data could be generated! */
    Serial.println(F("Sensor overload or saturated"));
    clearLcdBlock(2, 0, 19);
    lcd.setCursor(0, 2);
    lcd.print("no data");

    // set initial data
    data.lightness = -1;
  }
}

void rtcEvent(void) {
  if (!rtc.IsDateTimeValid()) {
    if (!wasRtcError("loop IsDateTimeValid")) {
      // Common Causes:
      //    1) the battery on the device is low or even missing and the power line was disconnected
      Serial.println(F("RTC lost confidence in the DateTime!"));
    }
  }

  lcd.setCursor(0, 0);

  RtcDateTime now = rtc.GetDateTime();
  if (!wasRtcError("loop GetDateTime")) {
    outputDateTime(now);
    Serial.println();
    currHour = now.Hour();

    if (now.Hour() >= 21 || now.Hour() <= 7) {
      lcd.setCursor(19, 1);
      lcd.write(byte(0));
    } else {
      lcd.setCursor(19, 1);
      lcd.write(1);
    }
  } else {
    lcd.print("00/00/0000  00:00:00");
  }

  RtcTemperature temp = rtc.GetTemperature();
  if (!wasRtcError("loop GetTemperature")) {
    temp.Print(Serial);
    Serial.println(" *C");
  }
}

void tempEvent(void) {
  const int acceptedTempDiff = 3;
  const int acceptedHumidityDiff = 10;
  lcd.setCursor(0, 3);

  // read with raw sample data.
  byte temperature = 0;
  byte humidity = 0;

  int err = SimpleDHTErrSuccess;
  if ((err = dht11.read(&temperature, &humidity, NULL)) != SimpleDHTErrSuccess) {
    Serial.print("Read DHT11 failed, err=");
    Serial.print(SimpleDHTErrCode(err));
    Serial.print(",");
    Serial.println(SimpleDHTErrDuration(err));

    if (SimpleDHTErrCode(err) == SimpleDHTErrStartLow) {
      // this usually means that we couldn't find the sensor on the board
      Serial.println(F("Potentially the DHT11 sensor is missing from the board"));
      lcd.print("no tmp snsr         ");
    } else {
      Serial.println(F("Read error in temp sensor"));
      lcd.print("err rd tmp          ");  // otherwise print generic read error message
    }

    wasTempError = true;

    // put the initial state
    data.temperature = 255;
    data.humidity = 255;

    return;
  }

  if (data.temperature == 255) {
    data.temperature = (int)temperature;
  }

  const int absTempDiff = abs((int)temperature - data.temperature);
  if (absTempDiff >= acceptedTempDiff) {
    Serial.print(F("Anomaly found, temperature value difference was above threshold: "));
    Serial.println(absTempDiff);

    if (prevData.temperature == 255) {
      prevData.temperature = (int)temperature;
    } else {
      const float absTempDiffAnomaly = abs((int)temperature - prevData.temperature);

      if (absTempDiffAnomaly <= 1) {
        prevData.temperature = data.temperature;
        data.temperature = (int)temperature;
        Serial.println(F("Previous measurement proved it is correct"));
      } else {
        prevData.temperature = (int)temperature;
      }
    }
  } else {
    prevData.temperature = data.temperature;
    data.temperature = (int)temperature;
  }

  if (data.humidity == 255) {
    data.humidity = (int)humidity;
  }

  const int absHumDiff = abs((int)humidity - data.humidity);
  if (absHumDiff >= acceptedHumidityDiff) {
    Serial.print(F("Anomaly found, humidity value difference was above threshold: "));
    Serial.println(absHumDiff);

    if (prevData.humidity == 255) {
      prevData.humidity = (int)humidity;
    } else {
      const float absHumDiffAnomaly = abs((int)humidity - prevData.humidity);

      if (absHumDiffAnomaly <= 5) {
        prevData.humidity = data.humidity;
        data.humidity = (int)humidity;
        Serial.println(F("Previous measurement proved it is correct"));
      } else {
        prevData.humidity = (int)humidity;
      }
    }
  } else {
    prevData.humidity = data.humidity;
    data.humidity = (int)humidity;
  }

  if ((prevData.temperature != data.temperature) || !isFirstLcdWriteDone || wasTempError) {
    clearLcdBlock(3, 0, 5);
    lcd.setCursor(0, 3);

    Serial.print(data.temperature);
    Serial.print(" *C, ");

    lcd.print(data.temperature);
    lcd.print("*C  ");

    clearLcdBlock(3, 12, 16);
    lcd.setCursor(12, 3);
    if (temperature >= warmWeatherThres) {
      lcd.print("warm ");
    } else {
      lcd.print("cold ");
    }
  } else {
    Serial.println(F("Skipping rewrite on temp, same measurement"));
  }

  if ((prevData.humidity != data.humidity) || !isFirstLcdWriteDone || wasTempError) {
    clearLcdBlock(3, 6, 11);
    wasTempError = false;
    lcd.setCursor(5, 3);

    Serial.print(data.humidity);
    Serial.println(" H");

    lcd.print(" ");
    lcd.print(data.humidity);
    lcd.print("%H  ");

    clearLcdBlock(3, 17, 19);
    lcd.setCursor(16, 3);
    if (humidity >= humidityThres) {
      lcd.print(" hmd");
    } else {
      lcd.print(" dry");
    }
  } else {
    Serial.println(F("Skipping rewrite on humidity, same measurement"));
  }
}

int getSwitchValue() {
  return digitalRead(A0);
}

void configureSPI() {
  SPCR |= bit(SPE);
  pinMode(MISO, OUTPUT);
  SPI.attachInterrupt();
}

void changeShadeMode(bool autoMode) {
  isAutoShade = autoMode;
  shadePausedFor = 0;
}

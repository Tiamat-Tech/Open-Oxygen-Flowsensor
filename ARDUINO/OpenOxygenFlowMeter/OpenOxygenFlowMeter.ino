
#include <LiquidCrystal_I2C.h>
#include <SparkFun_SDP3x_Arduino_Library.h> // Click here to get the library: http://librarymanager/All#SparkFun_SDP3x
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <EEPROM.h>


// all connected to I2C: SDA: Pin 21, SCL: 22
SDP3X mySensor; //create an object of the SDP3X class
Adafruit_ADS1115 ads;  /* Use for the oxygensensor */
LiquidCrystal_I2C lcd(0x38, 16, 2); // set the LCD address to 0x3F for a 16 chars and 2 line display


int16_t adc0 = -1;
int16_t mv = 0;
float oxy_m = 2.62; // needs to be calibrated
float oxy_b = 3;  // needs to be calibrated (corresponds to the voltage at Oxygen level == 0%)
float adcToMV =  0.1875;

// variables for initial auto calibration to air
float oxyCalibration = 20.9;
boolean shouldDoWarmupCalibration = true; //change this to false to skip warmup / calibration phase
unsigned long startTime;
unsigned static long warmupDelayInMs = 1000 * 10; // 10s warmup time before calibrating to 21% oxygen level

// variables for negative flow calibration
float minAbsDiffPressure = 2; // minimal absolute value of pressure to trigger negative flow calibration
unsigned static long negativeFlowInitDelayInMs = 1000 * 2; // 2s before initiating negative flow calibration
unsigned static long calibrate100WarmupInMs = 1000 * 10; // 10s warmup time before calibrating to 100%
unsigned static long negativeFlowPermanentSaveDelayInMs = 1000 * 10; // 10s delay before storing 100% calibration value permanently
unsigned long negativeFlowStartTime = 0;

//variables for storing calibration
static uint8_t calibrationOxyMAddress = 0;

unsigned static long infoMsgDelay = 1000 * 2; // 2s delay to display an info message

uint8_t countdown = 0;

float flowRate = 0;
float diffPressure = 0;
float oxygenLevel = 0;

enum DeviceState {
  LAUNCH,
  RESTORE_CALIBRATION,
  CALIBRATION_AIR_WARMUP,
  CALIBRATION_AIR,
  MEASURING,
  CALIBRATION_100_WARMUP,
  CALIBRATION_100_DONE,
  CALIBRATION_100_SAVE_DELAY,
  CALIBRATION_100_SAVED
};

enum DeviceState state = LAUNCH;


// TODO: Need to add a button to start the calibration
void setup() {

  Serial.begin(115200);

  Serial.println("Open Oxygen starting..");

  Wire.begin();

  // Initialize sensor
  mySensor.stopContinuousMeasurement();
  if (mySensor.begin() == false)
  {
    Serial.println(F("SDP3X not detected. Check connections. Freezing..."));
    while (1)
      ; // Do nothing more
  }
  mySensor.startContinuousMeasurement(true, true); // Request continuous measurements with mass flow temperature compensation and with averaging
  delay(1);

  // oxygen sensor related
  ads.setGain(GAIN_TWOTHIRDS);  // 2/3x gain +/- 6.144V  1 bit = 3mV      0.1875mV (default)
  ads.begin();
  // Setup 3V comparator on channel 0
  ads.startComparator_SingleEnded(0, 1000);


  // Init display
  lcd.init();
  lcd.clear();
  lcd.backlight();
  lcd.setCursor(2, 1);
  lcd.print("     O2       ");
  lcd.setCursor(2, 0);
  lcd.print("Open Oxygen");

  if(!shouldDoWarmupCalibration) {
    state = MEASURING;
  }

  // read initial calibration values from flash
  if (!EEPROM.begin(sizeof(0.0f))) {
    Serial.println("failed to initialise EEPROM");
  } else if(EEPROM_readFloat(calibrationOxyMAddress) > 0) {
    Serial.println("Reading initial oxy_m value from flash: ");
    Serial.println(EEPROM_readFloat(calibrationOxyMAddress));
    state = RESTORE_CALIBRATION;
    shouldDoWarmupCalibration = false;
  }
}

void loop() {

  boolean doCalibration = false;

  long currentTime = millis();
  if(state == RESTORE_CALIBRATION) {
    if(currentTime - startTime > infoMsgDelay) {
      state = MEASURING;
    }
  } else if(shouldDoWarmupCalibration) {
    if(currentTime - startTime < warmupDelayInMs) {
      countdown = int((warmupDelayInMs - currentTime + startTime)/1000)+1;
      state = CALIBRATION_AIR_WARMUP;
    } else {
      // warmup is over, trigger calibration & skip warmup check in the future
      shouldDoWarmupCalibration = false;
      doCalibration = true;
      state = MEASURING;
    }
  }

  // update sensor values
  updateFlowRate();
  updateOxygenLevel(doCalibration);

  if(state != CALIBRATION_AIR_WARMUP && state != RESTORE_CALIBRATION) {
    handleNegativeFlow();
  }

  displayState();

  delay(300);
}

void handleNegativeFlow() {
  if(diffPressure < 0 && abs(diffPressure) >= minAbsDiffPressure) {
    // negative flow rate detected
    long currentTime = millis();
    
    if(negativeFlowStartTime == 0) {
      // negative flow starting
      negativeFlowStartTime = currentTime;
    }
    int timeDif = currentTime - negativeFlowStartTime;
    
    // wait for init delay to trigger negative flow action
    unsigned long relevantDelay = negativeFlowInitDelayInMs;
    if(timeDif > relevantDelay) {
      
      // negative flow calibration
      
      // check if warmup is finished
      relevantDelay += calibrate100WarmupInMs;
      if(timeDif < relevantDelay) {
        state = CALIBRATION_100_WARMUP;
      } else {
        
        // check if calibration already happened, if not, calibrate and reset flash
        if(state == CALIBRATION_100_WARMUP) {
          calibrateTo100();
          resetCalibrationInFlash();
          state = CALIBRATION_100_DONE;
        }
        
        // wait until "calibration done" message was displayed
        relevantDelay += infoMsgDelay;
        if(timeDif > relevantDelay) {
          
          // delay to confirm that the calibration should be permanently stored
          relevantDelay += negativeFlowPermanentSaveDelayInMs;
          if(timeDif < relevantDelay) {
            state = CALIBRATION_100_SAVE_DELAY;
          } else {
            // store calibration once
            if(state == CALIBRATION_100_SAVE_DELAY) {
              storeCalibrationToFlash();
              state = CALIBRATION_100_SAVED;
            }
          }
        }
      }
    }
    countdown = int((relevantDelay - timeDif)/1000)+1;
  } else {
    negativeFlowStartTime = 0;
  }
}

void displayState() {
  String line1 = "  Open Oxygen   ";
  String line2 = "                ";
  
  if(state == LAUNCH) {
    line2 = "Launching..     ";
  }
  else if(state == CALIBRATION_AIR_WARMUP) {
    line1 = "Calibr. to 20.9%    ";
    line2 = "in ";
    line2 = line2 + countdown + "s, raw: " + adc0 * adcToMV + "        ";
  }
  else if(state == RESTORE_CALIBRATION) {
    line2 = "Restoring calibration..   ";
  }
  else if(state == MEASURING) {
    line1 = F("Flow: ");
    line1 += String(flowRate, 5);
  
    line2 = F("Oxygen: ");
    line2 += String(oxygenLevel, 4);
  }
  else if(state == CALIBRATION_100_WARMUP) {
    line1 = "Calibr. to 100%   ";
    line2 = "in ";
    line2 = line2 + countdown + "s, raw: " + adc0 * adcToMV + "        ";
  }
  else if(state == CALIBRATION_100_DONE) {
    line1 = "Calibration to  ";
    line2 = "100% O2 done.   ";
  }
  else if(state == CALIBRATION_100_SAVE_DELAY) {
    line1 = "Saving calibr.  ";
    line2 = "perm. in ";
    line2 = line2 + countdown + "s..  ";
  }
  else if(state == CALIBRATION_100_SAVED) {
    line1 = "Calibr. to 100% ";
    line2 = "perm. saved.    ";
  }
  
  line1 = line1.substring(0, 16);
  line2 = line2.substring(0, 16);
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void updateFlowRate() {
  float temperature; // Storage for the temperature

  // Read the averaged differential pressure and temperature from the sensor
  mySensor.readMeasurement(&diffPressure, &temperature); // Read the measurement

  // convert the differential pressure into flow-rate
  flowRate = convert2slm(abs((float)diffPressure));

  Serial.print(F("Differential pressure is: "));
  Serial.print(diffPressure, 2);
  Serial.print(F(" (Pa);  Flow-rate: "));
  Serial.print(flowRate);
  Serial.println(F(" (slm)"));
}

void updateOxygenLevel(boolean doCalibration) {
  // Read the ADC value from the bus:
   adc0 = ads.getLastConversionResults();
  if(doCalibration) {
    String msg = "Calibrating oxy_m to ";
    msg += String(adc0) + "..";
    Serial.print(msg);
    oxy_m = (oxyCalibration - oxy_b) / (adcToMV * adc0);
    // TODO is oxy_m within plausible boundaries?
  }
  //Serial.print("AIN0: "); Serial.println(adc0);
  // http://cool-web.de/esp8266-esp32/ads1115-16bit-adc-am-esp32-voltmeter.htm
  oxygenLevel = adc0 * adcToMV * oxy_m + oxy_b;

  Serial.print("Oxygenlevel: "); Serial.print(oxygenLevel); Serial.println(" %");
  Serial.print("ADC: "); Serial.println(adc0);
}

void calibrateTo100() {
  oxyCalibration = 100;
  updateOxygenLevel(true);
}

void storeCalibrationToFlash() {
  Serial.println("Writing oxy_m to flash:");
  Serial.println(oxy_m);
  EEPROM_writeFloat(calibrationOxyMAddress, oxy_m);
  EEPROM.commit();
  Serial.println("Reading initial oxy_m value from flash: ");
  Serial.println(EEPROM_readFloat(calibrationOxyMAddress));
  
}

void resetCalibrationInFlash() {
  Serial.println("Resetting oxy_m in flash..");
  EEPROM_writeFloat(calibrationOxyMAddress, 0.0f);
  EEPROM.commit();
  Serial.println("Reading initial oxy_m value from flash: ");
  Serial.println(EEPROM_readFloat(calibrationOxyMAddress));
  
}

void EEPROM_writeFloat(int ee, float value)
{
    byte* p = (byte*)(void*)&value;
    for (int i = 0; i < sizeof(value); i++)
        EEPROM.write(ee++, *p++);
}

float EEPROM_readFloat(int ee)
{
    float value = 0.0f;
    byte* p = (byte*)(void*)&value;
    for (int i = 0; i < sizeof(value); i++)
        *p++ = EEPROM.read(ee++);
    return value;
}

float convert2slm(float dp) {
  // convert the differential presure dp into the standard liter per minute
  float a = -20.04843438;
  float b = 59.52168936;
  float c = 3.11050553;
  float d = 10.35186327;
  return a + sqrt(b + dp * d) * c;
}

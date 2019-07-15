#include <Arduino.h>
#include <PacketSerial.h>   // COBS packet serial library

/* Program specific headers */
#include "state.h"          // uC data storage
#include "timer.h"          // Timer functions
#include "RGBleds.h"        // Basic wrapper for OctoWS2811 library
#include "bq769x0CRC.h"

PacketSerial packetSerial;
// Dumpy Data
const uint8_t correct[2] = {12, 123};
const uint8_t other[2] = {0x00, 0x01};

// Loop Timer state initalized in timer.h
extern timer_data timer_state;

// LED state
const int ledPin =  LED_BUILTIN;// the number of the LED pin
uint8_t ledState = LOW;

#define BMS_ALERT_PIN 16      // attached to interrupt INT0
#ifdef SELF_START
#define BMS_BOOT_PIN 17       // Boots the bq76930 when begin is called
#else
#define BMS_BOOT_PIN -1       // Assumes that bq7630 is already booted
#endif
#define BMS_I2C_ADDRESS 0x18  // Adress of chip bq7693007DBTR
#define BMS_NUM_CELLS 10      // Number of cells attached to BMS
bq769x0 BMS(BMS_NUM_CELLS, bq76930, BMS_I2C_ADDRESS); // BMS object

uint8_t battVoltage[2] = {0,0};
uint8_t battCurrent[2] = {0,0};
uint8_t batteryStatus[4];

void onPacketReceived(const uint8_t* buffer, size_t size) {
  if(buffer[0] == 20) {
    packetSerial.send(correct, 2);
  }
  else if(buffer[0] == 01) {
    memcpy(&batteryStatus[0], &battVoltage[0], 2*sizeof(uint8_t));
    memcpy(&batteryStatus[2], &battCurrent[0], 2*sizeof(uint8_t));
    packetSerial.send(batteryStatus, 4);
  }
  else if(buffer[0] == 0xFF) {
    BMS.shutdown();
  }
  else {
    packetSerial.send(other, 2);
  }
}

void setup() {
  Serial1.setRX(3);
  Serial1.setTX(4);
  Serial1.begin(500000);
  packetSerial.setStream(&Serial1);
  packetSerial.setPacketHandler(&onPacketReceived);

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, ledState);
  
  timer_init();            // Initialze main loop timer

  Wire.begin(I2C_MASTER, 0x0, I2C_PINS_18_19, I2C_PULLUP_EXT, 100000);
  
  // //BMS Setup
  BMS.begin(&Wire, BMS_ALERT_PIN, BMS_BOOT_PIN);
  BMS.setTemperatureLimits(-20, 45, 0, 45);
  BMS.setShuntResistorValue(9); // value in mOhms
  BMS.setShortCircuitProtection(14000, 200);  // delay in us
  // BMS.setOvercurrentChargeProtection(8000, 200);  // delay in ms
  BMS.setOvercurrentDischargeProtection(8000, 320); // delay in ms
  BMS.setCellUndervoltageProtection(3000, 4); // delay in s
  BMS.setCellOvervoltageProtection(4400, 2);  // delay in s

  BMS.setBalancingThresholds(0, 4200, 20);  // minIdleTime_min, minCellV_mV, maxVoltageDiff_mV
  BMS.setIdleCurrentThreshold(100);
  BMS.enableAutoBalancing();
  BMS.enableDischarging();

  rgbSetup();
}

void loop() {
  /* 
   * Everythin in this if statement will run at 10ms
   * Make sure all tasks take less than 10ms
   * If you want to run something slower than 10ms,
   * create and if statement and modulo systime with 
   * the ms delay you want the task to run
   */
  

  if(timer_state.systime != timer_state.prev_systime) {
    noInterrupts();
    timer_state.prev_systime = timer_state.systime;
    interrupts();

    if(timer_state.systime % 2 == 0){
      rgbUpdate();
    }

    if(timer_state.systime % 50 == 0) {
      // Status LED
      ledState = !ledState;
      digitalWrite(ledPin, ledState);
    }

    if(timer_state.systime % 25 == 0) {
      BMS.update();
    }

    if(timer_state.systime % 50 == 0){
      int temp;

      temp = BMS.getBatteryVoltage();
      battVoltage[0] = (temp >> 8) & 0xFF;
      battVoltage[1] = (temp) & 0xFF;
      temp = BMS.getBatteryCurrent();
      battCurrent[0] = (temp >> 8) & 0xFF;
      battCurrent[1] = (temp) & 0xFF;
    }
  }
  /*
   * Put tasks that should run as fast as possible here
   */
  else {
    packetSerial.update();
  }
}
/*
    bq769x0.cpp - Battery management system based on bq769x0 for Arduino
    Copyright (C) 2015  Martin Jäger (m.jaeger@posteo.de)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as 
    published by the Free Software Foundation, either version 3 of the 
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this program. If not, see 
    <http://www.gnu.org/licenses/>.
*/

/*
  TODO:
  - Balancing algorithm
  - SOC calculation + coulomb counting

*/

#include <Arduino.h>
#include <math.h>     // log for thermistor calculation

#include "bq769x0CRC.h"
#include "registers.h"

// for the ISR to know the bq769x0 instance
bq769x0* bq769x0::instancePointer = 0;

// CRC
FastCRC8 CRC8;


#if BQ769X0_DEBUG

const char *byte2char(int x)
{
    static char b[9];
    b[0] = '\0';

    int z;
    for (z = 128; z > 0; z >>= 1)
    {
        strcat(b, ((x & z) == z) ? "1" : "0");
    }

    return b;
}

#endif

//----------------------------------------------------------------------------

bq769x0::bq769x0(uint8_t numCells, byte bqType, int bqI2CAddress)
{
  type = bqType;
  I2CAddress = bqI2CAddress;
  
  if (type == bq76920) {
    if(numCells > 5) {
      numberOfCells = 5;
    }
    else {
      numberOfCells = numCells;
    }
    
  }
  else if (type == bq76930) {
    if(numCells > 10) {
      numberOfCells = 10;
    }
    else {
      numberOfCells = numCells;
    } 
  }
  else {
    if(numCells > 15) {
      numberOfCells = 15;
    }
    else {
      numberOfCells = numCells;
    }   
  }
  
  // prevent errors if someone reduced MAX_NUMBER_OF_CELLS accidentally
  if (numberOfCells > MAX_NUMBER_OF_CELLS) {
    numberOfCells = MAX_NUMBER_OF_CELLS;
  }
}


//-----------------------------------------------------------------------------

int bq769x0::begin(i2c_t3 *theWire, byte alertPin, byte bootPin)
{
  //Wire.begin();        // join I2C bus
  _wire = theWire;

  // initialize variables
  for (byte i = 0; i < numberOfCells; i++) {
    cellVoltages[i] = 0;
  }
  
  // Boot IC if pin is defined (else: manual boot via push button has to be 
  // done before calling this method)
  if (bootPin >= 0)
  {
    pinMode(bootPin, OUTPUT);
    digitalWrite(bootPin, HIGH);
    delay(5);   // wait 5 ms for device to receive boot signal (datasheet: max. 2 ms)
    pinMode(bootPin, INPUT);     // don't disturb temperature measurement
    delay(10);  // wait for device to boot up completely (datasheet: max. 10 ms)
  }
 
  // test communication
  writeRegister(CC_CFG, 0x19);       // should be set to 0x19 according to datasheet
  if (readRegister(CC_CFG) == 0x19)
  {
    // initial settings for bq769x0
    writeRegister(SYS_CTRL1, B00010000);  // switch die temp (no thermistor) and ADC on
    writeRegister(SYS_CTRL2, B01000000);  // switch CC_EN on

    // attach ALERT interrupt to this instance
    instancePointer = this;
    attachInterrupt(digitalPinToInterrupt(alertPin), bq769x0::alertISR, RISING);

    // get ADC offset and gain
    adcOffset = (signed int) readRegister(ADCOFFSET);  // convert from 2's complement
    adcGain = 365 + (((readRegister(ADCGAIN1) & B00001100) << 1) | 
      ((readRegister(ADCGAIN2) & B11100000) >> 5)); // uV/LSB
    
    return 0;
  }
  else
  {
    return 1;
  }
}

//----------------------------------------------------------------------------
// Fast function to check whether BMS has an error
// (returns 0 if everything is OK)

int bq769x0::checkStatus()
{
  if (alertInterruptFlag == false && errorStatus == 0) {
    return 0;
  }
  else {
    
    regSYS_STAT_t sys_stat;
    sys_stat.regByte = readRegister(SYS_STAT);

    if (sys_stat.bits.CC_READY == 1) {
      updateCurrent(true);  // automatically clears CC ready flag	
    }
    
    // Serious error occured
    if (sys_stat.regByte & B00111111)
    {
      if (alertInterruptFlag == true) {
        secSinceErrorCounter = 0;
      }
      errorStatus = sys_stat.regByte;
      
      int secSinceInterrupt = (millis() - interruptTimestamp) / 1000;
      
      // check for overrun of millis() or very slow running program
      if (abs(secSinceInterrupt - secSinceErrorCounter) > 2) {
        secSinceErrorCounter = secSinceInterrupt;
      }
      
      // called only once per second
      if (secSinceInterrupt >= secSinceErrorCounter)
      {
        if (sys_stat.regByte & B00100000) { // XR error
          // datasheet recommendation: try to clear after waiting a few seconds
          if (secSinceErrorCounter % 3 == 0) {
            #if BQ769X0_DEBUG
            Serial.println(F("Attempting to clear XR error"));
            #endif
            writeRegister(SYS_STAT, B00100000);
          }
        }
        if (sys_stat.regByte & B00010000) { // Alert error
          if (secSinceErrorCounter % 10 == 0) {
            #if BQ769X0_DEBUG
            Serial.println(F("Attempting to clear Alert error"));
            #endif
            writeRegister(SYS_STAT, B00010000);
          }
        }
        if (sys_stat.regByte & B00001000) { // UV error
          updateVoltages();
          if (cellVoltages[idCellMinVoltage] > minCellVoltage) {
            #if BQ769X0_DEBUG
            Serial.println(F("Attempting to clear UV error"));
            #endif
            writeRegister(SYS_STAT, B00001000);
          }
        }
        if (sys_stat.regByte & B00000100) { // OV error
          updateVoltages();
          if (cellVoltages[idCellMaxVoltage] < maxCellVoltage) {
            #if BQ769X0_DEBUG
            Serial.println(F("Attempting to clear OV error"));
            #endif
            writeRegister(SYS_STAT, B00000100);
          }
        }
        if (sys_stat.regByte & B00000010) { // SCD
          if (secSinceErrorCounter % 60 == 0) {
            #if BQ769X0_DEBUG
            Serial.println(F("Attempting to clear SCD error"));
            #endif
            writeRegister(SYS_STAT, B00000010);
          }
        }
        if (sys_stat.regByte & B00000001) { // OCD
          if (secSinceErrorCounter % 60 == 0) {
            #if BQ769X0_DEBUG
            Serial.println(F("Attempting to clear OCD error"));
            #endif
            writeRegister(SYS_STAT, B00000001);
            enableDischarging();
          }
        }
        
        secSinceErrorCounter++;
      }
    }
    else {
      errorStatus = 0;
    }
    
    return errorStatus;

  }

}

//----------------------------------------------------------------------------
// should be called at least once every 250 ms to get correct coulomb counting

void bq769x0::update()
{
  updateCurrent(false);  // will only read new current value if alert was triggered
  delayMicroseconds(100);
  updateVoltages();
  updateBalancingSwitches();
}

//----------------------------------------------------------------------------
// puts BMS IC into SHIP mode (i.e. switched off)

void bq769x0::shutdown()
{
  writeRegister(SYS_CTRL1, 0x0);
  writeRegister(SYS_CTRL1, 0x1);
  writeRegister(SYS_CTRL1, 0x2);
}

//----------------------------------------------------------------------------

bool bq769x0::enableCharging()
{
  if (checkStatus() == 0 &&
    cellVoltages[idCellMaxVoltage] < maxCellVoltage)
  {
    byte sys_ctrl2;
    sys_ctrl2 = readRegister(SYS_CTRL2);
    writeRegister(SYS_CTRL2, sys_ctrl2 | B00000001);  // switch CHG on
    #if BQ769X0_DEBUG
    Serial.println("Enabling CHG FET");
    #endif
    return true;
  }
  else {
    return false;
  }
}

//----------------------------------------------------------------------------

bool bq769x0::enableDischarging()
{
  if (checkStatus() == 0 )
    // &&
    // cellVoltages[idCellMinVoltage] > minCellVoltage)
  {
    byte sys_ctrl2;
    sys_ctrl2 = readRegister(SYS_CTRL2);
    writeRegister(SYS_CTRL2, sys_ctrl2 | B00000010);  // switch DSG on
    return true;
  }
  else {
    return false;
  }
}

//----------------------------------------------------------------------------

void bq769x0::enableAutoBalancing(void)
{
  autoBalancingEnabled = true;
}


//----------------------------------------------------------------------------

void bq769x0::setBalancingThresholds(int idleTime_min, int absVoltage_mV, byte voltageDifference_mV)
{
  balancingMinIdleTime_s = idleTime_min * 60;
  balancingMinCellVoltage_mV = absVoltage_mV;
  balancingMaxVoltageDifference_mV = voltageDifference_mV;
}

//----------------------------------------------------------------------------
// sets balancing registers if balancing is allowed 
// (sufficient idle time + voltage)

byte bq769x0::updateBalancingSwitches(void)
{
  long idleSeconds = (millis() - idleTimestamp) / 1000;
  byte numberOfSections = numberOfCells/5;
  
  // check for millis() overflow
  if (idleSeconds < 0) {
    idleTimestamp = 0;
    idleSeconds = millis() / 1000;
  }
    
  // check if balancing allowed
  if (checkStatus() == 0 &&
    idleSeconds >= balancingMinIdleTime_s && 
    cellVoltages[idCellMaxVoltage] > balancingMinCellVoltage_mV &&
    (cellVoltages[idCellMaxVoltage] - cellVoltages[idCellMinVoltage]) > balancingMaxVoltageDifference_mV)
  {
    balancingActive = true;
    
    regCELLBAL_t cellbal;
    byte balancingFlags;
    byte balancingFlagsTarget;
    
    for (int section = 0; section < numberOfSections; section++)
    {
      balancingFlags = 0;
      for (int i = 0; i < 5; i++)
      {
        if ((cellVoltages[section*5 + i] - cellVoltages[idCellMinVoltage]) > balancingMaxVoltageDifference_mV) {
          
          // try to enable balancing of current cell
          balancingFlagsTarget = balancingFlags | (1 << i);

          // check if attempting to balance adjacent cells
          bool adjacentCellCollision = 
            ((balancingFlagsTarget << 1) & balancingFlags) ||
            ((balancingFlags << 1) & balancingFlagsTarget);
            
          if (adjacentCellCollision == false) {
            balancingFlags = balancingFlagsTarget;
          }          
        }
      }
      
      // set balancing register for this section
      writeRegister(CELLBAL1+section, balancingFlags);
    }
  }
  else if (balancingActive == true)
  {  
    // clear all CELLBAL registers
    for (int section = 0; section < numberOfSections; section++)
    {
      writeRegister(CELLBAL1+section, 0x0);
    }
    
    balancingActive = false;
  }
}

void bq769x0::setShuntResistorValue(int res_mOhm)
{
  shuntResistorValue_mOhm = res_mOhm;
}

void bq769x0::setThermistorBetaValue(int beta_K)
{
  thermistorBetaValue = beta_K;
}

void bq769x0::setTemperatureLimits(int minDischarge_degC, int maxDischarge_degC, 
  int minCharge_degC, int maxCharge_degC)
{
  // Temperature limits (°C/10)
  minCellTempDischarge = minDischarge_degC * 10;
  maxCellTempDischarge = maxDischarge_degC * 10;
  minCellTempCharge = minCharge_degC * 10;
  maxCellTempCharge = maxCharge_degC * 10;  
}

void bq769x0::setIdleCurrentThreshold(int current_mA)
{
  idleCurrentThreshold = current_mA;
}


//----------------------------------------------------------------------------

long bq769x0::setShortCircuitProtection(long current_mA, int delay_us)
{
  regPROTECT1_t protect1;
  
  // only RSNS = 1 considered
  protect1.bits.RSNS = 1;

  protect1.bits.SCD_THRESH = 0;
  for (int i = sizeof(SCD_threshold_setting)/sizeof(SCD_threshold_setting[0])-1; i > 0; i--) {
    if (current_mA * shuntResistorValue_mOhm / 1000 >= SCD_threshold_setting[i]) {
      protect1.bits.SCD_THRESH = i;
      break;
    }
  }
  
  protect1.bits.SCD_DELAY = 0;
  for (int i = sizeof(SCD_delay_setting)/sizeof(SCD_delay_setting[0])-1; i > 0; i--) {
    if (delay_us >= SCD_delay_setting[i]) {
      protect1.bits.SCD_DELAY = i;
      break;
    }
  }
  
  writeRegister(PROTECT1, protect1.regByte);
  
  // returns the actual current threshold value
  return (long)SCD_threshold_setting[protect1.bits.SCD_THRESH] * 1000 / 
    shuntResistorValue_mOhm;
}

//----------------------------------------------------------------------------

long bq769x0::setOvercurrentChargeProtection(long current_mA, int delay_ms)
{
  // ToDo: Software protection for charge overcurrent
}

//----------------------------------------------------------------------------

long bq769x0::setOvercurrentDischargeProtection(long current_mA, int delay_ms)
{
  regPROTECT2_t protect2;
  long tempValue = (current_mA * shuntResistorValue_mOhm) / 1000;

  // Remark: RSNS must be set to 1 in PROTECT1 register

  protect2.bits.OCD_THRESH = 0;
  for (int i = sizeof(OCD_threshold_setting)/sizeof(OCD_threshold_setting[0])-1; i > 0; i--) {
    if (tempValue >= OCD_threshold_setting[i]) {
      protect2.bits.OCD_THRESH = i;
      break;
    }
  }
  
  protect2.bits.OCD_DELAY = 0;
  for (int i = sizeof(OCD_delay_setting)/sizeof(OCD_delay_setting[0])-1; i > 0; i--) {
    if (delay_ms >= OCD_delay_setting[i]) {
      protect2.bits.OCD_DELAY = i;
      break;
    }
  }
  
  writeRegister(PROTECT2, protect2.regByte);
 
  // returns the actual current threshold value
  return (long)OCD_threshold_setting[protect2.bits.OCD_THRESH] * 1000 / 
    shuntResistorValue_mOhm;
}


//----------------------------------------------------------------------------

int bq769x0::setCellUndervoltageProtection(int voltage_mV, int delay_s)
{
  regPROTECT3_t protect3;
  byte uv_trip = 0;
  
  minCellVoltage = voltage_mV;
  
  protect3.regByte = readRegister(PROTECT3);
  
  uv_trip = ((long)((voltage_mV - adcOffset) / (adcGain*1000)) >> 4) & 0x00FF;
  uv_trip += 1;   // always round up for lower cell voltage
  writeRegister(UV_TRIP, uv_trip);
  
  protect3.bits.UV_DELAY = 0;
  for (int i = sizeof(UV_delay_setting)/sizeof(UV_delay_setting[0])-1; i > 0; i--) {
    if (delay_s >= UV_delay_setting[i]) {
      protect3.bits.UV_DELAY = i;
      break;
    }
  }
  
  writeRegister(PROTECT3, protect3.regByte);
  
  // returns the actual current threshold value
  return ((long)1 << 12 | uv_trip << 4) * adcGain / 1000 + adcOffset;
}

//----------------------------------------------------------------------------

int bq769x0::setCellOvervoltageProtection(int voltage_mV, int delay_s)
{
  regPROTECT3_t protect3;
  byte ov_trip = 0;

  maxCellVoltage = voltage_mV;
  
  protect3.regByte = readRegister(PROTECT3);
  
  ov_trip = ((long)((voltage_mV - adcOffset) / (adcGain*1000)) >> 4) & 0x00FF;
  writeRegister(OV_TRIP, ov_trip);
    
  protect3.bits.OV_DELAY = 0;
  for (int i = sizeof(OV_delay_setting)/sizeof(OV_delay_setting[0])-1; i > 0; i--) {
    if (delay_s >= OV_delay_setting[i]) {
      protect3.bits.OV_DELAY = i;
      break;
    }
  }
  
  writeRegister(PROTECT3, protect3.regByte);
 
  // returns the actual current threshold value
  return (((long)1 << 13 | ov_trip << 4) * adcGain + (adcOffset*1000))/1000;
}


//----------------------------------------------------------------------------

int bq769x0::getBatteryCurrent()
{
  return batCurrent;
}

//----------------------------------------------------------------------------

int bq769x0::getBatteryVoltage()
{
  return batVoltage;
}

//----------------------------------------------------------------------------

int bq769x0::getMaxCellVoltage()
{
  return cellVoltages[idCellMaxVoltage];
}

//----------------------------------------------------------------------------

int bq769x0::getCellVoltage(byte idCell)
{
  return cellVoltages[idCell];
}


//----------------------------------------------------------------------------

float bq769x0::getTemperatureDegC(byte channel)
{
  if (channel >= 1 && channel <= 3) {
    return (float)temperatures[channel-1] / 10.0;
  }
  else
    return -273.15;   // Error: Return absolute minimum temperature
}

//----------------------------------------------------------------------------

float bq769x0::getTemperatureDegF(byte channel)
{
  return getTemperatureDegC(channel) * 1.8 + 32;
}


//----------------------------------------------------------------------------

void bq769x0::updateTemperatures()
{
  float tmp = 0;
  int adcVal = 0;
  int vtsx = 0;
  unsigned long rts = 0;
  
  _wire->beginTransmission(I2CAddress);
  _wire->write(0x2C);
  _wire->endTransmission();
  
  if (_wire->requestFrom(I2CAddress, 2) == 2)
  {
    // calculate R_thermistor according to bq769x0 datasheet
    adcVal = ((_wire->read() & B00111111) << 8) | _wire->read();
    vtsx = adcVal * 0.382; // mV
    rts = 10000.0 * vtsx / (3300.0 - vtsx); // Ohm
        
    // Temperature calculation using Beta equation
    // - According to bq769x0 datasheet, only 10k thermistors should be used
    // - 25°C reference temperature for Beta equation assumed
    tmp = 1.0/(1.0/(273.15+25) + 1.0/thermistorBetaValue*log(rts/10000.0)); // K
    
    temperatures[0] = (tmp - 273.15) * 10.0;
  }
}


//----------------------------------------------------------------------------
// If ignoreCCReadFlag == true, the current is read independent of an interrupt
// indicating the availability of a new CC reading

void bq769x0::updateCurrent(bool ignoreCCReadyFlag)
{
  int16_t adcVal = 0;
  regSYS_STAT_t sys_stat;
  sys_stat.regByte = readRegister(SYS_STAT);
  
  if (ignoreCCReadyFlag == true || sys_stat.bits.CC_READY == 1)
  {
    adcVal = (readRegister(0x32) << 8) | readRegister(0x33);
    batCurrent = (long)(adcVal * 844) / (long)(100*shuntResistorValue_mOhm);  // mA

    // if (batCurrent > -10 && batCurrent < 10)
    // {
    //   batCurrent = 0;
    // }
    
    // reset idleTimestamp
    if (abs(batCurrent) > idleCurrentThreshold) {
      idleTimestamp = millis();
    }

    // no error occured which caused alert
    if (!(sys_stat.regByte & B00111111)) {
      alertInterruptFlag = false;
    }

    writeRegister(SYS_STAT, B10000000);  // Clear CC ready flag	
  }
}

//----------------------------------------------------------------------------
// reads all cell voltages to array cellVoltages[4] and updates batVoltage

void bq769x0::updateVoltages()
{
  long adcVal = 0;
  
  // read battery pack voltage
  adcVal = (readRegister(BAT_HI_BYTE) << 8) | readRegister(BAT_LO_BYTE);
  batVoltage = (4 * adcGain * adcVal) / 1000 + (numberOfCells * adcOffset);
  
  // read cell voltages
  _wire->beginTransmission(I2CAddress);
  _wire->write(VC1_HI_BYTE);
  _wire->endTransmission();
  
 _wire->requestFrom(I2CAddress, 2 * numberOfCells);

  idCellMaxVoltage = 0;
  idCellMinVoltage = 0;
  for (int i = 0; i < numberOfCells; i++)
  {
    adcVal = (_wire->read() << 8) | _wire->read();
    cellVoltages[i] = (adcVal * adcGain)/1000 + adcOffset;

    if (cellVoltages[i] > cellVoltages[idCellMaxVoltage]) {
      idCellMaxVoltage = i;
    }
    if (cellVoltages[i] < cellVoltages[idCellMinVoltage] && cellVoltages[i] > 500) {
      idCellMinVoltage = i;
    }
  }
}

//----------------------------------------------------------------------------

void bq769x0::writeRegister(byte address, uint8_t data)
{
  uint8_t crcData[3] = {(I2CAddress<<1), address, data};
  _wire->beginTransmission(I2CAddress);
  _wire->write(address);
  _wire->write(data);
  _wire->write(CRC8.smbus(crcData,3));
  _wire->endTransmission();
}

//----------------------------------------------------------------------------

int bq769x0::readRegister(byte address)
{  
  _wire->beginTransmission(I2CAddress);
  _wire->write(address);
  _wire->endTransmission();
  _wire->requestFrom(I2CAddress, 2); // TODO: add CRC check for incoming data
  return _wire->read();
}

//----------------------------------------------------------------------------
// the actual ISR, called by static function alertISR()

void bq769x0::setAlertInterruptFlag()
{
  interruptTimestamp = millis();
  alertInterruptFlag = true;
}

//----------------------------------------------------------------------------
// The bq769x0 drives the ALERT pin high if the SYS_STAT register contains
// a new value (either new CC reading or an error)

void bq769x0::alertISR()
{
  if (instancePointer != 0)
  {
    instancePointer->setAlertInterruptFlag();
  }
}


#if BQ769X0_DEBUG

//----------------------------------------------------------------------------
// for debug purposes

void bq769x0::printRegisters()
{
  Serial.print(F("0x00 SYS_STAT:  "));
  Serial.println(byte2char(readRegister(SYS_STAT)));

  Serial.print(F("0x01 CELLBAL1:  "));
  Serial.println(byte2char(readRegister(CELLBAL1)));

  Serial.print(F("0x04 SYS_CTRL1: "));
  Serial.println(byte2char(readRegister(SYS_CTRL1)));
  
  Serial.print(F("0x05 SYS_CTRL2: "));
  Serial.println(byte2char(readRegister(SYS_CTRL2)));
  
  Serial.print(F("0x06 PROTECT1:  "));
  Serial.println(byte2char(readRegister(PROTECT1)));
  
  Serial.print(F("0x07 PROTECT2:  "));
  Serial.println(byte2char(readRegister(PROTECT2)));
  
  Serial.print(F("0x08 PROTECT3   "));
  Serial.println(byte2char(readRegister(PROTECT3)));
  
  Serial.print(F("0x09 OV_TRIP:   "));
  Serial.println(byte2char(readRegister(OV_TRIP)));
  
  Serial.print(F("0x0A UV_TRIP:   "));
  Serial.println(byte2char(readRegister(UV_TRIP)));
  
  Serial.print(F("0x0B CC_CFG:    "));
  Serial.println(byte2char(readRegister(CC_CFG)));

  Serial.print(F("0x32 CC_HI:     "));
  Serial.println(byte2char(readRegister(CC_HI_BYTE)));

  Serial.print(F("0x33 CC_LO:     "));
  Serial.println(byte2char(readRegister(CC_LO_BYTE)));
/*
  Serial.print(F("0x50 ADCGAIN1:  "));
  Serial.println(byte2char(readRegister(ADCGAIN1)));

  Serial.print(F("0x51 ADCOFFSET: "));
  Serial.println(byte2char(readRegister(ADCOFFSET)));

  Serial.print(F("0x59 ADCGAIN2:  "));
  Serial.println(byte2char(readRegister(ADCGAIN2)));
  */
}

#endif

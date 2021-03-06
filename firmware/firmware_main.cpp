/**************************************************************************************************************************/
#include <bluefruit.h>
//#include <Bluefruit_FileIO.h>
#include "firmware.h"

#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

/**************************************************************************************************************************/
// Keyboard Matrix
byte rows[] MATRIX_ROW_PINS;        // Contains the GPIO Pin Numbers defined in keyboard_config.h
byte columns[] MATRIX_COL_PINS;     // Contains the GPIO Pin Numbers defined in keyboard_config.h

const uint8_t boot_mode_commands [BOOT_MODE_COMMANDS_COUNT][2] BOOT_MODE_COMMANDS;

KeyScanner keys;

bool isReportedReleased = true;
uint8_t monitoring_state = STATE_BOOT_INITIALIZE;

/**************************************************************************************************************************/
// put your setup code here, to run once:
/**************************************************************************************************************************/
void setup() {

  LOG_LV1("BLEMIC","Starting %s" ,DEVICE_NAME);

  setupGpio();                                                                // checks that NFC functions on GPIOs are disabled.

  Scheduler.startLoop(monitoringloop);                                        // Starting second loop task for monitoring tasks
  Scheduler.startLoop(keyscanningloop);                                       // Starting third loop task for key scanning

  setupBluetooth();

  #if BACKLIGHT_PWM_ON == 1 //setup PWM module
    setupPWM();
  #endif
  // Set up keyboard matrix and start advertising
  setupKeymap();
  setupMatrix();
  startAdv();
};
/**************************************************************************************************************************/
//
/**************************************************************************************************************************/
void setupMatrix(void) {
    //inits all the columns as INPUT
   for (const auto& column : columns) {
      LOG_LV2("BLEMIC","Setting to INPUT Column: %i" ,column);
      pinMode(column, INPUT);
    }

   //inits all the rows as INPUT_PULLUP
   for (const auto& row : rows) {
      LOG_LV2("BLEMIC","Setting to INPUT_PULLUP Row: %i" ,row);
      pinMode(row, INPUT_PULLUP);
    }
};
/**************************************************************************************************************************/
// Keyboard Scanning
/**************************************************************************************************************************/
void scanMatrix() {
  uint32_t pindata = 0;
  for(int j = 0; j < MATRIX_ROWS; ++j) {
    //set the current row as OUPUT and LOW
    pinMode(rows[j], OUTPUT);
    #if DIODE_DIRECTION == COL2ROW
    digitalWrite(rows[j], LOW);                                       // 'enables' a specific row to be "low"
    #else
    digitalWrite(rows[j], HIGH);                                       // 'enables' a specific row to be "HIGH"
    #endif
    //loops thru all of the columns
    for (int i = 0; i < MATRIX_COLS; ++i) {
          #if DIODE_DIRECTION == COL2ROW
          pinMode(columns[i], INPUT_PULLUP);                              // 'enables' the column High Value on the diode; becomes "LOW" when pressed
          #else
          pinMode(columns[i], INPUT_PULLDOWN);                              // 'enables' the column High Value on the diode; becomes "LOW" when pressed
          #endif
    }

      delay(1);   // using the FreeRTOS delay function reduced power consumption from 1.5mA down to 0.9mA
      // need for the GPIO lines to settle down electrically before reading.
     /*#ifdef NRFX_H__  // Added to support BSP 0.9.0
         nrfx_coredep_delay_us(1);
      #else            // Added to support BSP 0.8.6
        nrf_delay_us(1);
      #endif*/

      pindata = NRF_GPIO->IN;                                         // read all pins at once
     for (int i = 0; i < MATRIX_COLS; ++i) {
      KeyScanner::scanMatrix((pindata>>(columns[i]))&1, millis(), j, i);       // This function processes the logic values and does the debouncing
      pinMode(columns[i], INPUT);                                     //'disables' the column that just got looped thru
     }
    pinMode(rows[j], INPUT);                                          //'disables' the row that was just scanned
   }                                                                  // done scanning the matrix
};
/**************************************************************************************************************************/
// Communication with computer and other boards
/**************************************************************************************************************************/
void sendKeyPresses() {
   KeyScanner::getReport();                                            // get state data - Data is in KeyScanner::currentReport
   if (!(KeyScanner::reportEmpty))  //any key presses anywhere?
   {
        sendKeys(KeyScanner::currentReport);
        isReportedReleased = false;
        LOG_LV1("MXSCAN","SEND: %i %i %i %i %i %i %i %i %i %i" ,millis(),KeyScanner::currentReport[0], KeyScanner::currentReport[1],KeyScanner::currentReport[2],KeyScanner::currentReport[3], KeyScanner::currentReport[4],KeyScanner::currentReport[5], KeyScanner::currentReport[6],KeyScanner::currentReport[7] );
    }
   else                                                                  //NO key presses anywhere
   {
    if ((!isReportedReleased)){
      sendRelease(KeyScanner::currentReport);
      isReportedReleased = true;                                         // Update flag so that we don't re-issue the message if we don't need to.
      LOG_LV1("MXSCAN","RELEASED: %i %i %i %i %i %i %i %i %i %i" ,millis(),KeyScanner::currentReport[0], KeyScanner::currentReport[1],KeyScanner::currentReport[2],KeyScanner::currentReport[3], KeyScanner::currentReport[4],KeyScanner::currentReport[5], KeyScanner::currentReport[6],KeyScanner::currentReport[7] );
    }
   }
  #if BLE_PERIPHERAL ==1   | BLE_CENTRAL ==1                            /**************************************************/
    if(KeyScanner::layerChanged)                                               //layer comms
    {
        sendlayer(KeyScanner::localLayer);
        LOG_LV1("MXSCAN","Layer %i  %i" ,millis(),KeyScanner::localLayer);
        KeyScanner::layerChanged = false;                                      // mark layer as "not changed" since last update
    }
  #endif                                                                /**************************************************/
}
/**************************************************************************************************************************/
// put your main code here, to run repeatedly:
/**************************************************************************************************************************/
void loop() {
  // put your main code here, to run repeatedly:

  unsigned long timesincelastkeypress = millis() - KeyScanner::getLastPressed();

  #if SLEEP_ACTIVE == 1
    gotoSleep(timesincelastkeypress,Bluefruit.connected());
  #endif

  #if BLE_CENTRAL == 1
    if ((timesincelastkeypress<10)&&(!Bluefruit.Central.connected()&&(!Bluefruit.Scanner.isRunning())))
    {
      Bluefruit.Scanner.start(0);                                                     // 0 = Don't stop scanning after 0 seconds  ();
    }
  #endif

  #if BACKLIGHT_PWM_ON == 1
    updatePWM(timesincelastkeypress);
  #endif

  if (monitoring_state == STATE_BOOT_MODE)
  {
      KeyScanner::getReport();                                            // get state data - Data is in KeyScanner::currentReport
      if (!(KeyScanner::reportEmpty))
      {
        for (int i = 0; i < BOOT_MODE_COMMANDS_COUNT; ++i)          // loop through BOOT_MODE_COMMANDS and compare with the first key being pressed - assuming only 1 key will be pressed when in boot mode.
        {
          if(KeyScanner::currentReport[1] == boot_mode_commands[i][0])
          {
            monitoring_state = boot_mode_commands[i][1];
          }
        }
      }
  }
  delay(HIDREPORTINGINTERVAL*4);
};
/**************************************************************************************************************************/
// put your key scanning code here, to run repeatedly:
/**************************************************************************************************************************/
void keyscanningloop () {
  #if MATRIX_SCAN == 1
    scanMatrix();
  #endif
  #if SEND_KEYS == 1
    sendKeyPresses();    // how often does this really run?
  #endif

  delay(HIDREPORTINGINTERVAL);
}
//********************************************************************************************//
//* Battery Monitoring Task - runs infrequently - except in boot mode                        *//
//********************************************************************************************//
void monitoringloop() {
  switch(monitoring_state)
  {
    case STATE_BOOT_INITIALIZE:
        monitoring_state = STATE_BOOT_MODE;
      break;
    case STATE_BOOT_MODE:
      if (millis()>BOOT_MODE_DELAY) {monitoring_state = STATE_MONITOR_MODE;}
      delay(25); // adds a delay to minimize power consumption during boot mode.
      break;
    case STATE_BOOT_CLEAR_BONDS:
       // Serial.println();
       // Serial.println("----- Before -----\n");
       // bond_print_list(BLE_GAP_ROLE_PERIPH);
       // bond_print_list(BLE_GAP_ROLE_CENTRAL);
      //  Bluefruit.clearBonds();
      //  Bluefruit.Central.clearBonds();
        InternalFS.format();  // using formatting instead of clearbonds due to the potential issue with corrupted file system and the keybord being stuck not being able to pair and save bonds.

      //  Serial.println();
       // Serial.println("----- After  -----\n");

       // bond_print_list(BLE_GAP_ROLE_PERIPH);
       // bond_print_list(BLE_GAP_ROLE_CENTRAL);
        monitoring_state = STATE_MONITOR_MODE;
      break;
    case STATE_BOOT_SERIAL_DFU:
        enterSerialDfu();
      break;
    case STATE_BOOT_WIRELESS_DFU:
        enterOTADfu();
      break;
    case STATE_MONITOR_MODE:
                #if BLE_LIPO_MONITORING == 1
                  updateBattery();
                #endif
                delay(30000);                                             // wait 30 seconds before a new battery update.  Needed to minimize power consumption.
      break;
    case STATE_BOOT_UNKNOWN:
      break;
    default:
      break;

  }
};
//********************************************************************************************//
//* Idle Task - runs when there is nothing to do                                             *//
//* Any impact of placing code here on current consumption?                                  *//
//********************************************************************************************//
void rtos_idle_callback(void) {
  // Don't call any other FreeRTOS blocking API()
  // Perform background task(s) here
  sd_app_evt_wait();  // puts the nrf52 to sleep when there is nothing to do.  You need this to reduce power consumption. (removing this will increase current to 8mA)
};

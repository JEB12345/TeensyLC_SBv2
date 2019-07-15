#include <Arduino.h>
#include "timer.h"

IntervalTimer milliLoop;       // Non-interupt loop for timing in milliseconds
timer_data timer_state;     // Must call this as extern in main file

 void timer_init() {
     // Setup the main 10ms loop timer
     milliLoop.begin(mainTimer, 10000);
 }

 // Timer update loop
void mainTimer() {
  timer_state.systime++;
}
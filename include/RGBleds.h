#ifndef RGBLEDS_H
#define RGBLEDS_H

#include <Arduino.h>
#include <OctoWS2811.h>     // High performance WS2811/WS2812 library for Teensy3.X

#include "makeColor.h"

void rgbSetup();

void rgbUpdate();

#endif // RGBLEDS_H
#include "RGBleds.h"

const uint8_t ledsPerStrip = 20;

// LED strip setup configs
DMAMEM int displayMemory[ledsPerStrip*6]; // OctoWS2811 library docs state you need times 6 # LEDs
int drawingMemory[ledsPerStrip*6];
const int RGBconfig = WS2811_GRB | WS2811_800kHz;
OctoWS2811 leds(ledsPerStrip, displayMemory, drawingMemory, RGBconfig);

int rainbowColors[180];
uint8_t color = 0;

void rgbSetup() {
    leds.begin();
    for (int i=0; i<180; i++) {
        int hue = i * 2;
        int saturation = 100;
        int lightness = 50;
        // pre-compute the 180 rainbow colors
        rainbowColors[i] = makeColor(hue, saturation, lightness);
    }
}

// For testing, this function will update the RGB leds to a gradient pattern down the stip of LEDs
// Call rgbUpdate at whatever rate you want the LEDs to cycle through colors.
void rgbUpdate() {
    int x, y;
    const uint8_t phaseShift = 10;
    for (x=0; x < ledsPerStrip; x++) {
    int index = (color + x + phaseShift/2) % 180;
    // leds.setPixel(x, rainbowColors[1]);
    leds.setPixel(x, rainbowColors[index]);
    }
    leds.show();
    color++;
    if(color >= 180) {
    color = 0;
    }
}
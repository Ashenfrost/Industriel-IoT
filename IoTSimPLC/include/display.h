#pragma once

// ---------- Configuration (1.3" SH1106 OLED, 128x64, I2C, driven with U8g2) ----------

// I2C pins. Use the same OLED_SDA / OLED_SCL values as in the game console project
// if that wiring is the same.
const int DISPLAY_SDA_PIN = 21;
const int DISPLAY_SCL_PIN = 22;

// I2C address, only used to check that the display is present (U8g2 uses 0x3C by default)
#define DISPLAY_I2C_ADDR 0x3C

const unsigned long DISPLAY_REFRESH_MS = 500;

// ---------- Interface ----------

// Start the display. If it is not found, the rest of the program keeps running.
void displayInit(void);

// Two lines of text, for status messages during startup (e.g. "Connecting...")
void displayShowMessage(const char* line1, const char* line2);

// Call every loop(). Redraws readings and warnings every DISPLAY_REFRESH_MS.
void displayUpdate(void);

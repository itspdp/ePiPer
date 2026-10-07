#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <OpenFontRender.h>

// Bring in the massive splash image array
#include "splash.h"

// ==========================================
// 1. HARDWARE CONFIGURATION
// ==========================================
#define EPD_PWR       7
#define EPD_CS        45
#define EPD_DC        46
#define EPD_RES       47
#define EPD_BUSY      48

#define SPI_MOSI      11
#define SPI_MISO      -1
#define SPI_SCK       12

#define SD_PWR        42
#define SD_CS         10
#define SD_MOSI       40
#define SD_MISO       13
#define SD_SCK        39

GxEPD2_BW<GxEPD2_420_SE0420NQ04, GxEPD2_420_SE0420NQ04::HEIGHT> display(
    GxEPD2_420_SE0420NQ04(EPD_CS, EPD_DC, EPD_RES, EPD_BUSY)
);

SPIClass sdSPI(FSPI);

// ==========================================
// 2. TEXT ENGINE (FreeType + HarfBuzz)
// ==========================================
OpenFontRender render;
uint8_t* font_buffer = nullptr;

void myDrawPixel(int32_t x, int32_t y, uint16_t color) {
    display.drawPixel(x, y, color);
}

void showBootScreen() {
    display.setRotation(0); 
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        int x = (400 - LOGO_WIDTH) / 2;
        int y = (300 - LOGO_HEIGHT) / 2;
        display.drawBitmap(x, y, epipar_pi_logo, LOGO_WIDTH, LOGO_HEIGHT, GxEPD_BLACK);
    } while (display.nextPage());
    Serial.println("Boot screen drawn.");
}

bool mountSDCard() {
    pinMode(SD_PWR, OUTPUT);
    digitalWrite(SD_PWR, HIGH);
    delay(100);

    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sdSPI, 4000000)) {
        Serial.println("ERROR: SD Card Mount Failed!");
        return false;
    }
    return true;
}

bool initTextEngine() {
    File fontFile = SD.open("/notodev.ttf", FILE_READ);
    if (!fontFile) {
        Serial.println("ERROR: Could not find /notodev.ttf on SD card!");
        return false;
    }

    size_t fileSize = fontFile.size();
    Serial.printf("Loading font size: %d bytes into PSRAM...\n", fileSize);

    font_buffer = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);
    if (font_buffer == nullptr) {
        Serial.println("ERROR: PSRAM Allocation Failed!");
        fontFile.close();
        return false;
    }

    fontFile.read(font_buffer, fileSize);
    fontFile.close();

    render.setSerial(Serial);
    if (render.loadFont(font_buffer, fileSize)) {
        Serial.println("ERROR: FreeType failed to parse the font!");
        return false;
    }

    render.setDrawPixel(myDrawPixel);
    render.setFontColor(GxEPD_BLACK);
    render.setFontSize(48); 
    
    Serial.println("SUCCESS: Text Engine Initialized!");
    return true;
}

// ==========================================
// 3. MAIN LIFECYCLE
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, HIGH); 
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);
    display.init(115200, true, 2, false);
    
    // 1. Draw the splash image from splash.h
    showBootScreen();
    
    // 2. Mount SD and Init Text Engine
    if (mountSDCard()) {
        if (initTextEngine()) {
            
            // 3. Draw Devanagari Text Test
            display.setRotation(0);
            display.firstPage();
            do {
                display.fillScreen(GxEPD_WHITE);
                
                render.setCursor(20, 50);
                render.cprintf("ePiPer Reader");

                // Test complex Conjuncts
                render.setCursor(20, 120);
                render.cprintf("क्ष त्र ज्ञ श्र");
                
                // Test Matras
                render.setCursor(20, 190);
                render.cprintf("क कि की कु कू");
                
            } while (display.nextPage());
        }
    }
}

void loop() {
    delay(1000);
}
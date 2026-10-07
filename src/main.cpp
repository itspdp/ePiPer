#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <OpenFontRender.h>

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

OpenFontRender render;
uint8_t* font_buffer = nullptr;

// ==========================================
// 2. HELPER FUNCTIONS
// ==========================================
void myDrawPixel(int32_t x, int32_t y, uint16_t color) {
    display.drawPixel(x, y, color);
}

// Fallback function to display errors on the e-paper using the built-in ASCII font
void showErrorOnScreen(const char* errorMsg) {
    Serial.println(errorMsg);
    display.setRotation(0);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setTextSize(2);
        display.setCursor(20, 50);
        display.print("SYSTEM ERROR:");
        display.setCursor(20, 100);
        display.print(errorMsg);
    } while (display.nextPage());
}

// ==========================================
// 3. STORAGE & DYNAMIC FONT ENGINE
// ==========================================
bool mountSDCard() {
    pinMode(SD_PWR, OUTPUT);
    digitalWrite(SD_PWR, HIGH);
    delay(100);

    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sdSPI, 4000000)) {
        showErrorOnScreen("SD Card Mount Failed. Insert FAT32 Card.");
        return false;
    }
    return true;
}

bool loadFirstAvailableFont() {
    File root = SD.open("/");
    File file = root.openNextFile();
    String fontFilename = "";

    // Scan SD root for the first .ttf file
    while (file) {
        String filename = String(file.name());
        if (!file.isDirectory() && (filename.endsWith(".ttf") || filename.endsWith(".TTF"))) {
            fontFilename = "/" + filename;
            file.close();
            break;
        }
        file.close();
        file = root.openNextFile();
    }
    
    if (fontFilename == "") {
        showErrorOnScreen("No .ttf file found on SD Card root.");
        return false;
    }

    Serial.println("Auto-detected font: " + fontFilename);
    File fontFile = SD.open(fontFilename.c_str(), FILE_READ);
    if (!fontFile) {
        showErrorOnScreen("Found .ttf but failed to open it.");
        return false;
    }

    size_t fileSize = fontFile.size();
    Serial.printf("Allocating %d bytes in PSRAM...\n", fileSize);

    font_buffer = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);
    if (font_buffer == nullptr) {
        showErrorOnScreen("PSRAM Out of Memory! Font too large.");
        fontFile.close();
        return false;
    }

    fontFile.read(font_buffer, fileSize);
    fontFile.close();

    render.setSerial(Serial);
    if (render.loadFont(font_buffer, fileSize)) {
        showErrorOnScreen("FreeType failed to parse the font file.");
        return false;
    }

    render.setDrawPixel(myDrawPixel);
    render.setFontColor(GxEPD_BLACK);
    render.setFontSize(48); 
    
    Serial.println("SUCCESS: Dynamic Font Engine Ready.");
    return true;
}

// ==========================================
// 4. MAIN LIFECYCLE
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, HIGH); 
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);
    display.init(115200, true, 2, false);
    
    // Draw Splash Screen
    display.setRotation(0); 
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.drawBitmap((400 - LOGO_WIDTH) / 2, (300 - LOGO_HEIGHT) / 2, epipar_pi_logo, LOGO_WIDTH, LOGO_HEIGHT, GxEPD_BLACK);
    } while (display.nextPage());
    
    // Init System
    if (mountSDCard() && loadFirstAvailableFont()) {
        
        display.setRotation(0);
        display.firstPage();
        do {
            display.fillScreen(GxEPD_WHITE);
            
            // Use drawString instead of cprintf for UTF-8 safety
            render.drawString("ePiPer Reader", 20, 50);
            render.drawString("क्ष त्र ज्ञ श्र", 20, 120);
            render.drawString("क कि की कु कू", 20, 190);
            
        } while (display.nextPage());
    }
}

void loop() {
    delay(1000);
}
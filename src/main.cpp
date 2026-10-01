#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>

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
// 2. BOOT LOGO (PROGMEM)
// ==========================================
const int LOGO_WIDTH = 64;
const int LOGO_HEIGHT = 64;

const unsigned char epipar_pi_logo[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf8,
    0x1f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf8,
    0x00, 0x38, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x00,
    0x00, 0x38, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x00,
    0x00, 0x38, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x00,
    0x00, 0x38, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x00,
    0x00, 0x38, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x00,
    0x00, 0x38, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x00,
    0x00, 0x38, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x00,
    0x00, 0x38, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x00,
    0x00, 0x38, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x00,
    0x00, 0x38, 0x00, 0x00, 0x00, 0x01, 0xc0, 0x00,
    0x00, 0x3c, 0x00, 0x00, 0x00, 0x03, 0x80, 0x00,
    0x00, 0x1f, 0x80, 0x00, 0x00, 0x07, 0x00, 0x00
};

void showBootScreen() {
    display.setRotation(0);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        int x = (400 - LOGO_WIDTH) / 2;
        int y = (300 - LOGO_HEIGHT) / 2;
        display.drawBitmap(x, y, epipar_pi_logo, LOGO_WIDTH, LOGO_HEIGHT, GxEPD_BLACK);
    } while (display.nextPage());
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
    
    Serial.println("SUCCESS: SD Card Mounted!");
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD Card Size: %llu MB\n", cardSize);
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n--- ePiPer Booting ---");

    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, HIGH);

    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);
    display.init(115200, true, 2, false);

    showBootScreen();
    mountSDCard();

    Serial.println("Baseline setup complete.");
}

void loop() {
    delay(1000);
}
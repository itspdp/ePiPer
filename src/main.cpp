#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <vector>

// 1. Hardware Definition
#define EPD_PWR       7
#define EPD_CS        45
#define EPD_DC        46
#define EPD_RES       47
#define EPD_BUSY      48

#define SPI_MOSI      11
#define SPI_MISO      -1
#define SPI_SCK       12

#define BTN_UP        6  
#define BTN_DOWN      4  
#define BTN_SELECT    5  
#define BTN_MENU      2
#define BTN_EXIT      1

// 2. Modular Includes (MUST come before instantiating extern variables)
#include "splash.h"
#include "system_storage.h"
#include "text_engine.h"

// 3. Global Instantiations for Headers
GxEPD2_BW<GxEPD2_420_SE0420NQ04, GxEPD2_420_SE0420NQ04::HEIGHT> display(
    GxEPD2_420_SE0420NQ04(EPD_CS, EPD_DC, EPD_RES, EPD_BUSY)
);
SPIClass sdSPI(FSPI);
std::vector<String> bookList;
uint8_t* font_buffer = nullptr;
FT_Library ft_library;
FT_Face ft_face;
hb_font_t *hb_font;

// 4. UI State
int currentSelection = 0;
bool inReadingMode = false;
unsigned long lastButtonPress = 0;

// Font Cycler State
int fontSizes[] = {16, 24, 32, 48}; 
int currentFontIndex = 2; // Defaults to 32px

void showError(const char* msg) {
    Serial.println(msg);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setTextSize(2);
        display.setCursor(20, 50);
        display.print("SYSTEM ERROR:");
        display.setCursor(20, 100);
        display.print(msg);
    } while (display.nextPage());
}

void showBootScreen() {
    display.setFullWindow();
    display.setRotation(0); 
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.drawBitmap((400 - LOGO_WIDTH) / 2, (300 - LOGO_HEIGHT) / 2, epipar_pi_logo, LOGO_WIDTH, LOGO_HEIGHT, GxEPD_BLACK);
    } while (display.nextPage());
}

void drawLibraryMenu(bool partialRefresh = false) {
    display.setRotation(0);
    if (partialRefresh) display.setPartialWindow(0, 0, 400, 300);
    else display.setFullWindow();
    
    setFontSize(24); 
    
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.fillRect(0, 0, 400, 40, GxEPD_BLACK);
        drawShapedText("ePiPer Library", 10, 28, GxEPD_WHITE);
        
        if (bookList.empty()) {
            drawShapedText("No books found on SD.", 10, 80);
            continue;
        }

        int visibleItems = 6;
        int startIndex = 0;
        if (currentSelection >= visibleItems) startIndex = currentSelection - visibleItems + 1; 

        for (int i = startIndex; i < (int)bookList.size() && i < startIndex + visibleItems; i++) {
            int y_pos = 75 + ((i - startIndex) * 40); 
            if (i == currentSelection) {
                display.fillRoundRect(5, y_pos - 24, 380, 34, 4, GxEPD_BLACK);
                drawShapedText(bookList[i].c_str(), 15, y_pos, GxEPD_WHITE);
            } else {
                drawShapedText(bookList[i].c_str(), 15, y_pos, GxEPD_BLACK);
            }
        }
        
        if (bookList.size() > visibleItems) {
            int barHeight = max(20, (260 * visibleItems) / (int)bookList.size());
            int barY = 40 + ((currentSelection * (260 - barHeight)) / (bookList.size() - 1));
            display.drawFastVLine(395, 40, 260, GxEPD_BLACK); 
            display.fillRoundRect(392, barY, 6, barHeight, 3, GxEPD_BLACK); 
        }
    } while (display.nextPage());
}

void openBook() {
    inReadingMode = true;
    display.setFullWindow(); 
    display.clearScreen(); // Deep hardware wipe to fix E-Ink ghosting
    
    int activeFontSize = fontSizes[currentFontIndex];
    setFontSize(activeFontSize);
    
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        
        String filePath = bookList[currentSelection];
        if (!filePath.startsWith("/")) filePath = "/" + filePath;

        File file = SD.open(filePath.c_str());
        
        if (!file) {
            drawShapedText("Error reading file.", 10, 50);
        } else if (filePath.endsWith(".epub") || filePath.endsWith(".EPUB")) {
            drawShapedText("EPUB parser offline.", 10, 50);
            drawShapedText("Requires Phase 4 caching.", 10, 90);
            file.close();
        } else {
            int max_width = 390; 
            int line_height = activeFontSize + (activeFontSize / 3);
            int x_pos = 10;
            int y_pos = activeFontSize + 10; 
            
            while (file.available() && y_pos < 290) {
                String word = file.readStringUntil(' '); 
                if (word.length() == 0) continue;
                
                word += " "; 
                int word_width = getTextWidth(word.c_str());
                
                if (x_pos + word_width > max_width) {
                    x_pos = 10;
                    y_pos += line_height;
                    if (y_pos >= 290) break; 
                }
                
                drawShapedText(word.c_str(), x_pos, y_pos);
                x_pos += word_width;
            }
            file.close();
        }
    } while (display.nextPage());
}

void setup() {
    Serial.begin(115200);
    
    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_SELECT, INPUT_PULLUP);
    pinMode(BTN_MENU, INPUT_PULLUP);
    pinMode(BTN_EXIT, INPUT_PULLUP);

    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, HIGH); 
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);
    display.init(115200, true, 2, false);
    
    showBootScreen();
    
    if (mountSDCard()) {
        if(initRawEngine()) {
            scanForBooks();
            drawLibraryMenu(false); 
        } else {
            showError("Text Engine Failed");
        }
    } else {
        showError("SD Mount Failed");
    }
}

void loop() {
    unsigned long currentMillis = millis();
    
    if (currentMillis - lastButtonPress > 250) { 
    
        if (digitalRead(BTN_UP) == LOW) {
            lastButtonPress = currentMillis;
            if (!inReadingMode && !bookList.empty()) {
                currentSelection--;
                if (currentSelection < 0) currentSelection = bookList.size() - 1;
                drawLibraryMenu(true);
            }
        }
        
        if (digitalRead(BTN_DOWN) == LOW) {
            lastButtonPress = currentMillis;
            if (!inReadingMode && !bookList.empty()) {
                currentSelection++;
                if (currentSelection >= (int)bookList.size()) currentSelection = 0;
                drawLibraryMenu(true);
            }
        }
        
        if (digitalRead(BTN_SELECT) == LOW) {
            lastButtonPress = currentMillis;
            if (!inReadingMode && !bookList.empty()) {
                openBook();
            }
        }
        
        if (digitalRead(BTN_EXIT) == LOW) {
            lastButtonPress = currentMillis;
            if (inReadingMode) {
                inReadingMode = false;
                display.clearScreen(); // Deep wipe before returning to menu
                drawLibraryMenu(false); 
            }
        }

        if (digitalRead(BTN_MENU) == LOW) {
            lastButtonPress = currentMillis;
            if (inReadingMode) {
                currentFontIndex++;
                if (currentFontIndex > 3) currentFontIndex = 0;
                openBook(); 
            }
        }
    }
    delay(10); 
}
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

// 2. Modular Includes
#include "splash.h"
#include "system_storage.h"
#include "text_engine.h"

// 3. Global Instantiations
GxEPD2_BW<GxEPD2_420_SE0420NQ04, GxEPD2_420_SE0420NQ04::HEIGHT> display(
    GxEPD2_420_SE0420NQ04(EPD_CS, EPD_DC, EPD_RES, EPD_BUSY)
);
SPIClass sdSPI(FSPI);
std::vector<String> bookList;
uint8_t* font_buffer = nullptr;
FT_Library ft_library;
FT_Face ft_face;
hb_font_t *hb_font;

// 4. UI & Pagination State
int currentSelection = 0;
bool inReadingMode = false;
unsigned long lastButtonPress = 0;

int fontSizes[] = {20, 26, 34, 48}; 
int currentFontIndex = 2; 

std::vector<uint32_t> pageOffsets;
int currentPage = 0;

// ==========================================
// RENDERERS
// ==========================================
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

void renderPage() {
    int activeFontSize = fontSizes[currentFontIndex];
    setFontSize(activeFontSize);
    
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        
        String filePath = bookList[currentSelection];
        if (!filePath.startsWith("/")) filePath = "/" + filePath;
        File file = SD.open(filePath.c_str());

        if (file) {
            file.seek(pageOffsets[currentPage]);

            int max_width = 390; 
            int line_height = activeFontSize + (activeFontSize / 3);
            int x_pos = 10;
            int y_pos = activeFontSize + 10; 
            bool isNewLine = true;
            
            while (file.available()) {
                uint32_t currentOffset = file.position();
                String word = "";
                bool paragraphBreak = false;
                bool hasSpace = false;

                // 1. Precise Character Tokenizer (Fixes ignored line breaks)
                while (file.available()) {
                    char c = file.read();
                    if (c == ' ') {
                        hasSpace = true;
                        break;
                    } else if (c == '\n') {
                        paragraphBreak = true;
                        break;
                    } else if (c != '\r') {
                        word += c; 
                    }
                }

                if (word.length() == 0 && !paragraphBreak) continue;

                // 2. Markdown Headers
                if (isNewLine && word.startsWith("#")) {
                    setFontSize(activeFontSize + 12);
                    line_height = (activeFontSize + 12) * 1.3;
                    word = word.substring(1); 
                }

                if (word.length() > 0) {
                    // FIX: Calculate width WITHOUT the space to prevent premature wrapping
                    int pure_word_width = getTextWidth(word.c_str());

                    if (x_pos + pure_word_width > max_width && x_pos > 10) {
                        x_pos = 10;
                        y_pos += line_height;
                    }

                    if (y_pos >= 290) {
                        if (currentPage + 1 >= (int)pageOffsets.size()) {
                            pageOffsets.push_back(currentOffset); 
                        }
                        break; 
                    }

                    // Append space for actual drawing
                    if (hasSpace) word += " ";
                    drawShapedText(word.c_str(), x_pos, y_pos);
                    x_pos += getTextWidth(word.c_str()); // Advance by full width
                }

                isNewLine = false;

                // 3. True Paragraph Breaks
                if (paragraphBreak) {
                    x_pos = 10;
                    y_pos += line_height;
                    isNewLine = true;
                    setFontSize(activeFontSize); 
                    line_height = activeFontSize + (activeFontSize / 3);

                    if (y_pos >= 290) {
                        if (file.available() && currentPage + 1 >= (int)pageOffsets.size()) {
                            pageOffsets.push_back(file.position());
                        }
                        break;
                    }
                }
            }
            file.close();
        }
    } while (display.nextPage());
}

void openBook() {
    inReadingMode = true;
    
    pageOffsets.clear();
    pageOffsets.push_back(0); 
    currentPage = 0;
    
    renderPage();
}

// ==========================================
// LIFECYCLE
// ==========================================
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
    
    if (mountSDCard() && initRawEngine()) {
        scanForBooks();
        drawLibraryMenu(false); 
    }
}

void loop() {
    unsigned long currentMillis = millis();
    
    if (currentMillis - lastButtonPress > 250) { 
    
        if (digitalRead(BTN_UP) == LOW) {
            lastButtonPress = currentMillis;
            if (inReadingMode) {
                if (currentPage > 0) {
                    currentPage--;
                    renderPage();
                }
            } else if (!bookList.empty()) {
                currentSelection--;
                if (currentSelection < 0) currentSelection = bookList.size() - 1;
                drawLibraryMenu(true);
            }
        }
        
        if (digitalRead(BTN_DOWN) == LOW) {
            lastButtonPress = currentMillis;
            if (inReadingMode) {
                if (currentPage + 1 < (int)pageOffsets.size()) {
                    currentPage++;
                    renderPage();
                }
            } else if (!bookList.empty()) {
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
                drawLibraryMenu(false); 
            }
        }

        if (digitalRead(BTN_MENU) == LOW) {
            lastButtonPress = currentMillis;
            if (inReadingMode) {
                currentFontIndex++;
                if (currentFontIndex > 3) currentFontIndex = 0;
                
                uint32_t savedOffset = pageOffsets[currentPage];
                pageOffsets.clear();
                pageOffsets.push_back(savedOffset);
                currentPage = 0;
                
                renderPage(); 
            }
        }
    }
    delay(10); 
}
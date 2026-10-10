#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <vector>
#include <Preferences.h> // Added for permanent bookmarking

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
Preferences prefs;

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
            uint32_t fileSize = file.size();

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

                // Clean Markdown Bolding
                word.replace("**", "");
                word.replace("__", "");

                if (word.length() == 0 && !paragraphBreak) continue;

                // Smart Markdown Proportional Headers
                if (isNewLine && word.startsWith("#")) {
                    int hashCount = 0;
                    while (word.length() > hashCount && word[hashCount] == '#') hashCount++;
                    
                    if (hashCount > 0 && hashCount <= 6) {
                        float scaleMap[] = {1.6, 1.4, 1.3, 1.2, 1.1, 1.1}; // Proportional scaling
                        int scaledSize = activeFontSize * scaleMap[hashCount - 1];
                        setFontSize(scaledSize);
                        line_height = scaledSize * 1.3;
                        word = word.substring(hashCount); // Strip hashes
                    }
                }

                if (word.length() > 0) {
                    int pure_word_width = getTextWidth(word.c_str());

                    if (x_pos + pure_word_width > max_width && x_pos > 10) {
                        x_pos = 10;
                        y_pos += line_height;
                    }

                    // Stop rendering at y=270 to leave room for the Status Bar
                    if (y_pos >= 270) {
                        if (currentPage + 1 >= (int)pageOffsets.size()) {
                            pageOffsets.push_back(currentOffset); 
                        }
                        break; 
                    }

                    if (hasSpace) word += " ";
                    drawShapedText(word.c_str(), x_pos, y_pos);
                    x_pos += getTextWidth(word.c_str()); 
                }

                isNewLine = false;

                if (paragraphBreak) {
                    x_pos = 10;
                    y_pos += line_height;
                    isNewLine = true;
                    setFontSize(activeFontSize); 
                    line_height = activeFontSize + (activeFontSize / 3);

                    if (y_pos >= 270) {
                        if (file.available() && currentPage + 1 >= (int)pageOffsets.size()) {
                            pageOffsets.push_back(file.position());
                        }
                        break;
                    }
                }
            }

            // --- CONSUMER UI: STATUS BAR ---
            display.fillRect(0, 275, 400, 25, GxEPD_BLACK);
            setFontSize(16);
            
            // Draw Truncated Book Title
            String title = bookList[currentSelection];
            if (title.length() > 25) title = title.substring(0, 22) + "...";
            drawShapedText(title.c_str(), 10, 293, GxEPD_WHITE);

            // Draw Percentage
            int percent = (file.position() * 100) / fileSize;
            String progress = String(percent) + "%";
            drawShapedText(progress.c_str(), 350, 293, GxEPD_WHITE);
            
            file.close();
        }
    } while (display.nextPage());
}

void openBook() {
    inReadingMode = true;
    
    // Save current book to memory
    prefs.putInt("bookIndex", currentSelection);

    if (pageOffsets.empty()) {
        pageOffsets.push_back(0); 
        currentPage = 0;
    }
    
    renderPage();
}

// ==========================================
// LIFECYCLE
// ==========================================
void setup() {
    Serial.begin(115200);
    prefs.begin("epiper", false);
    
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
        
        // Auto-Resume Last Read Book
        int savedBook = prefs.getInt("bookIndex", -1);
        uint32_t savedOffset = prefs.getUInt("pageOffset", 0);
        
        if (savedBook >= 0 && savedBook < (int)bookList.size()) {
            currentSelection = savedBook;
            pageOffsets.clear();
            pageOffsets.push_back(savedOffset);
            currentPage = 0;
            openBook();
        } else {
            drawLibraryMenu(false); 
        }
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
                    prefs.putUInt("pageOffset", pageOffsets[currentPage]); // Save Bookmark
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
                    prefs.putUInt("pageOffset", pageOffsets[currentPage]); // Save Bookmark
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
                pageOffsets.clear(); // Fresh start if opened manually from menu
                openBook();
            }
        }
        
        if (digitalRead(BTN_EXIT) == LOW) {
            lastButtonPress = currentMillis;
            if (inReadingMode) {
                inReadingMode = false;
                prefs.putInt("bookIndex", -1); // Clear active book so it boots to menu next time
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
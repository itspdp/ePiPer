#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <vector>
#include <Preferences.h>
#include <time.h> // Internal RTC

// ==========================================
// 1. HARDWARE DEFINITION
// ==========================================
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

// ==========================================
// 2. MODULAR INCLUDES
// ==========================================
#include "splash.h"
#include "system_storage.h"
#include "text_engine.h"

// ==========================================
// 3. GLOBAL INSTANTIATIONS
// ==========================================
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

// System State
int currentSelection = 0;
bool inReadingMode = false;
unsigned long lastButtonPress = 0;

int fontSizes[] = {20, 26, 34, 48}; 
int currentFontIndex = 2; 

std::vector<uint32_t> pageOffsets;
int currentPage = 0;

// ==========================================
// 4. OS UI MANAGER NAMESPACE
// ==========================================
namespace UIManager {
    String currentTime = "12:00 PM"; // Fallback until NTP sync
    bool wifiEnabled = false;
    bool syncActive = false;

    // Simulates clock progression until we connect to a real NTP server
    void updateInternalClock() {
        unsigned long totalMinutes = millis() / 60000;
        int hours = (12 + (totalMinutes / 60)) % 12;
        if (hours == 0) hours = 12;
        int mins = totalMinutes % 60;
        char timeBuf[10];
        snprintf(timeBuf, sizeof(timeBuf), "%d:%02d %s", hours, mins, (millis() % 86400000 < 43200000) ? "PM" : "AM");
        currentTime = String(timeBuf);
    }

    void drawTopBar() {
        display.fillRect(0, 0, 400, 25, GxEPD_BLACK);
        setFontSize(14);
        
        // Left: Wi-Fi Status
        String wifiStr = wifiEnabled ? "Wi-Fi: ON" : "Wi-Fi: OFF";
        drawShapedText(wifiStr.c_str(), 10, 18, GxEPD_WHITE);
        
        // Center: System Status
        if (syncActive) {
            drawShapedText("Syncing...", 170, 18, GxEPD_WHITE);
        }

        // Right: Right-Aligned Clock
        updateInternalClock();
        int timeWidth = getTextWidth(currentTime.c_str());
        drawShapedText(currentTime.c_str(), 390 - timeWidth, 18, GxEPD_WHITE);
    }

    void drawBottomBar(String title, int percent) {
        display.fillRect(0, 275, 400, 25, GxEPD_BLACK);
        setFontSize(14);
        
        if (title.length() > 25) title = title.substring(0, 22) + "...";
        drawShapedText(title.c_str(), 10, 293, GxEPD_WHITE);

        String progress = String(percent) + "%";
        int progWidth = getTextWidth(progress.c_str());
        drawShapedText(progress.c_str(), 390 - progWidth, 293, GxEPD_WHITE);
    }
}

// ==========================================
// 5. ENGINE RENDERERS
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
    if (partialRefresh) display.setPartialWindow(0, 25, 400, 275); // Protect Top Bar
    else display.setFullWindow();
    
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        
        if (!partialRefresh) {
            UIManager::drawTopBar();
        }

        setFontSize(24);
        
        // Sub-Header for Library
        display.fillRect(0, 25, 400, 35, GxEPD_WHITE);
        display.drawFastHLine(0, 60, 400, GxEPD_BLACK);
        drawShapedText("Local Storage (SD)", 10, 50, GxEPD_BLACK);
        
        if (bookList.empty()) {
            drawShapedText("No books found on SD.", 10, 100);
            continue;
        }

        int visibleItems = 5;
        int startIndex = 0;
        if (currentSelection >= visibleItems) startIndex = currentSelection - visibleItems + 1; 

        for (int i = startIndex; i < (int)bookList.size() && i < startIndex + visibleItems; i++) {
            int y_pos = 95 + ((i - startIndex) * 40); 
            if (i == currentSelection) {
                display.fillRoundRect(5, y_pos - 24, 380, 34, 4, GxEPD_BLACK);
                drawShapedText(bookList[i].c_str(), 15, y_pos, GxEPD_WHITE);
            } else {
                drawShapedText(bookList[i].c_str(), 15, y_pos, GxEPD_BLACK);
            }
        }
        
        if (bookList.size() > visibleItems) {
            int barHeight = max(20, (215 * visibleItems) / (int)bookList.size());
            int barY = 60 + ((currentSelection * (215 - barHeight)) / (bookList.size() - 1));
            display.drawFastVLine(395, 60, 215, GxEPD_BLACK); 
            display.fillRoundRect(392, barY, 6, barHeight, 3, GxEPD_BLACK); 
        }
    } while (display.nextPage());
}

void renderPage() {
    String filePath = bookList[currentSelection];
    if (!filePath.startsWith("/")) filePath = "/" + filePath;

    // Strict EPUB Guardrail (Requires Phase 5 Decompressor)
    if (filePath.endsWith(".epub") || filePath.endsWith(".EPUB")) {
        display.setFullWindow();
        display.firstPage();
        do {
            display.fillScreen(GxEPD_WHITE);
            UIManager::drawTopBar();
            
            setFontSize(24);
            drawShapedText("EPUB Engine Required", 10, 80);
            
            setFontSize(16);
            drawShapedText("EPUBs are compressed ZIP archives.", 10, 120);
            drawShapedText("We must add a decompression library to", 10, 150);
            drawShapedText("platformio.ini before extracting the text.", 10, 180);
            
            setFontSize(20);
            drawShapedText("Press EXIT to return.", 10, 240);
        } while (display.nextPage());
        return;
    }

    int activeFontSize = fontSizes[currentFontIndex];
    setFontSize(activeFontSize);
    
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        UIManager::drawTopBar();
        
        File file = SD.open(filePath.c_str());
        if (file) {
            file.seek(pageOffsets[currentPage]);
            uint32_t fileSize = file.size();

            int max_width = 390; 
            int line_height = activeFontSize + (activeFontSize / 3);
            int x_pos = 10;
            int y_pos = 25 + activeFontSize + 10; // Start below top bar
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

                word.replace("**", "");
                word.replace("__", "");

                if (word.length() == 0 && !paragraphBreak) continue;

                if (isNewLine && word.startsWith("#")) {
                    int hashCount = 0;
                    while (word.length() > hashCount && word[hashCount] == '#') hashCount++;
                    
                    if (hashCount > 0 && hashCount <= 6) {
                        float scaleMap[] = {1.6, 1.4, 1.3, 1.2, 1.1, 1.1}; 
                        int scaledSize = activeFontSize * scaleMap[hashCount - 1];
                        setFontSize(scaledSize);
                        line_height = scaledSize * 1.3;
                        word = word.substring(hashCount); 
                    }
                }

                if (word.length() > 0) {
                    int pure_word_width = getTextWidth(word.c_str());

                    if (x_pos + pure_word_width > max_width && x_pos > 10) {
                        x_pos = 10;
                        y_pos += line_height;
                    }

                    // Strict Sandboxing: Stop rendering before hitting the Bottom Bar
                    if (y_pos >= 265) {
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

                    if (y_pos >= 265) {
                        if (file.available() && currentPage + 1 >= (int)pageOffsets.size()) {
                            pageOffsets.push_back(file.position());
                        }
                        break;
                    }
                }
            }

            int percent = (file.position() * 100) / fileSize;
            UIManager::drawBottomBar(bookList[currentSelection], percent);
            
            file.close();
        }
    } while (display.nextPage());
}

void openBook() {
    inReadingMode = true;
    prefs.putInt("bookIndex", currentSelection);

    if (pageOffsets.empty()) {
        pageOffsets.push_back(0); 
        currentPage = 0;
    }
    
    renderPage();
}

// ==========================================
// 6. LIFECYCLE
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
                    prefs.putUInt("pageOffset", pageOffsets[currentPage]); 
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
                // If it's an EPUB, block pagination
                String f = bookList[currentSelection];
                if (f.endsWith(".epub") || f.endsWith(".EPUB")) return;

                if (currentPage + 1 < (int)pageOffsets.size()) {
                    currentPage++;
                    prefs.putUInt("pageOffset", pageOffsets[currentPage]); 
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
                pageOffsets.clear(); 
                openBook();
            }
        }
        
        if (digitalRead(BTN_EXIT) == LOW) {
            lastButtonPress = currentMillis;
            if (inReadingMode) {
                inReadingMode = false;
                prefs.putInt("bookIndex", -1); 
                drawLibraryMenu(false); 
            }
        }

        if (digitalRead(BTN_MENU) == LOW) {
            lastButtonPress = currentMillis;
            if (inReadingMode) {
                // If it's an EPUB, block font changing
                String f = bookList[currentSelection];
                if (f.endsWith(".epub") || f.endsWith(".EPUB")) return;

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
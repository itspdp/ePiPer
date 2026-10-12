#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <vector>
#include <Preferences.h>
#include <time.h> 

// ==========================================
// 1. HARDWARE & NETWORK CONFIG
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

const char* WIFI_SSID = "Your_WiFi_Name";
const char* WIFI_PASS = "Your_WiFi_Password";

// ==========================================
// 2. MODULAR INCLUDES
// ==========================================
#include "splash.h"
#include "system_storage.h"
#include "text_engine.h"
#include "wifi_manager.h" 

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

int currentSelection = 0;
bool inReadingMode = false;

int fontSizes[] = {20, 26, 34, 48}; 
int currentFontIndex = 2; 

std::vector<uint32_t> pageOffsets;
int currentPage = 0;

unsigned long menuPressTime = 0;
bool menuPressed = false;
bool menuLongHandled = false;

unsigned long exitPressTime = 0;
bool exitPressed = false;
bool exitLongHandled = false;

unsigned long lastActionTime = 0; 

// Layout Constants for Perfect Cache Synchronization
#define TOP_BAR_H 20
#define BOT_BAR_Y 276

// ==========================================
// 4. THE SD CACHING ENGINE
// ==========================================
bool loadCache(String cachePath) {
    File cacheFile = SD.open(cachePath.c_str(), FILE_READ);
    if (!cacheFile) return false;
    pageOffsets.clear();
    while (cacheFile.available()) {
        uint32_t offset;
        if (cacheFile.read((uint8_t*)&offset, sizeof(offset)) == sizeof(offset)) {
            pageOffsets.push_back(offset);
        }
    }
    cacheFile.close();
    return !pageOffsets.empty();
}

void saveCache(String cachePath) {
    File cacheFile = SD.open(cachePath.c_str(), FILE_WRITE);
    if (cacheFile) {
        for (uint32_t offset : pageOffsets) {
            cacheFile.write((uint8_t*)&offset, sizeof(offset));
        }
        cacheFile.close();
    }
}

void buildCache(String filePath, String cachePath, int activeFontSize) {
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        setFontSize(24);
        drawShapedText("Paginating Document...", 10, 100);
        setFontSize(16);
        drawShapedText("Caching precise line-breaks to SD Card.", 10, 150);
        drawShapedText("This ensures flawless reverse-scrolling.", 10, 180);
    } while (display.nextPage());

    pageOffsets.clear();
    pageOffsets.push_back(0);
    
    File file = SD.open(filePath.c_str());
    if (!file) return;

    int max_width = 390; 
    int line_height = activeFontSize + (activeFontSize / 3);
    int x_pos = 10;
    int y_pos = TOP_BAR_H + activeFontSize + 6; 
    bool isNewLine = true;

    while (file.available()) {
        uint32_t currentOffset = file.position();
        String word = "";
        bool paragraphBreak = false;
        bool hasSpace = false;

        while (file.available()) {
            char c = file.read();
            if (c == ' ') { hasSpace = true; break; }
            else if (c == '\n') { paragraphBreak = true; break; }
            else if (c != '\r') { word += c; }
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
            
            if (y_pos >= BOT_BAR_Y) {
                pageOffsets.push_back(currentOffset);
                x_pos = 10;
                y_pos = TOP_BAR_H + activeFontSize + 6; 
            }
            
            if (hasSpace) word += " ";
            x_pos += getTextWidth(word.c_str());
        }

        isNewLine = false;

        if (paragraphBreak) {
            x_pos = 10;
            y_pos += line_height;
            isNewLine = true;
            setFontSize(activeFontSize); 
            line_height = activeFontSize + (activeFontSize / 3);

            if (y_pos >= BOT_BAR_Y) {
                if (file.available()) pageOffsets.push_back(file.position());
                x_pos = 10;
                y_pos = TOP_BAR_H + activeFontSize + 6; 
            }
        }
    }
    file.close();
    saveCache(cachePath);
    setFontSize(activeFontSize); 
}

// ==========================================
// 5. OS UI MANAGER NAMESPACE
// ==========================================
namespace UIManager {
    String currentTime = "12:00 PM"; 
    bool isCharging() { return true; } 
    int getBatteryPercentage() { return 100; } 

    void updateInternalClock() {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 10)) { 
            char timeBuf[10];
            strftime(timeBuf, sizeof(timeBuf), "%I:%M %p", &timeinfo);
            currentTime = String(timeBuf);
        } else {
            unsigned long totalMinutes = millis() / 60000;
            int hours = (12 + (totalMinutes / 60)) % 12;
            if (hours == 0) hours = 12;
            int mins = totalMinutes % 60;
            char timeBuf[10];
            snprintf(timeBuf, sizeof(timeBuf), "%d:%02d %s", hours, mins, (millis() % 86400000 < 43200000) ? "PM" : "AM");
            currentTime = String(timeBuf);
        }
    }

    void drawTopBar() {
        display.fillRect(0, 0, 400, TOP_BAR_H, GxEPD_BLACK); 
        setFontSize(14); 
        
        String wifiStr = isWifiConnected ? currentIP : "Wi-Fi: OFF";
        drawShapedText(wifiStr.c_str(), 10, 15, GxEPD_WHITE);

        updateInternalClock();
        String battStr = isCharging() ? "[USB]" : "[" + String(getBatteryPercentage()) + "%]";
        String rightHUD = battStr + "  " + currentTime;
        
        int hudWidth = getTextWidth(rightHUD.c_str());
        drawShapedText(rightHUD.c_str(), 390 - hudWidth, 15, GxEPD_WHITE);
    }

    void drawBottomBar(String title, int currentPg, int totalPg) {
        display.fillRect(0, BOT_BAR_Y, 400, 24, GxEPD_BLACK); 
        setFontSize(18); // Minimum required height for Devanagari "क्ष" without boxing
        
        title.replace(".txt", "");
        title.replace(".TXT", "");
        title.replace(".md", "");
        title.replace(".MD", "");
        title.replace(".epub", "");
        title.replace(".EPUB", "");

        if (title.length() > 22) title = title.substring(0, 19) + "...";
        drawShapedText(title.c_str(), 10, 294, GxEPD_WHITE);

        String progress = String(currentPg + 1) + " / " + String(totalPg);
        int progWidth = getTextWidth(progress.c_str());
        drawShapedText(progress.c_str(), 390 - progWidth, 294, GxEPD_WHITE);
    }
}

// ==========================================
// 6. ENGINE RENDERERS
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
    if (partialRefresh) display.setPartialWindow(0, TOP_BAR_H, 400, 300 - TOP_BAR_H); 
    else display.setFullWindow();
    
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        
        if (!partialRefresh) UIManager::drawTopBar();

        setFontSize(24);
        display.fillRect(0, TOP_BAR_H, 400, 36, GxEPD_WHITE);
        display.drawFastHLine(0, TOP_BAR_H + 36, 400, GxEPD_BLACK);
        drawShapedText("Local Storage (SD)", 10, TOP_BAR_H + 24, GxEPD_BLACK);
        
        if (bookList.empty()) {
            drawShapedText("No books found on SD.", 10, 110);
            continue;
        }

        // Restored denser gaps
        int visibleItems = 6;
        int startIndex = 0;
        if (currentSelection >= visibleItems) startIndex = currentSelection - visibleItems + 1; 

        for (int i = startIndex; i < (int)bookList.size() && i < startIndex + visibleItems; i++) {
            int y_pos = 90 + ((i - startIndex) * 34); 
            
            String displayName = bookList[i];
            displayName.replace(".txt", "");
            displayName.replace(".TXT", "");
            displayName.replace(".md", "");
            displayName.replace(".MD", "");
            displayName.replace(".epub", "");
            displayName.replace(".EPUB", "");

            if (i == currentSelection) {
                display.fillRoundRect(5, y_pos - 24, 380, 32, 4, GxEPD_BLACK);
                drawShapedText(displayName.c_str(), 15, y_pos - 2, GxEPD_WHITE);
            } else {
                drawShapedText(displayName.c_str(), 15, y_pos - 2, GxEPD_BLACK);
            }
        }
        
        if (bookList.size() > visibleItems) {
            int barHeight = max(20, (230 * visibleItems) / (int)bookList.size());
            int barY = 65 + ((currentSelection * (230 - barHeight)) / (bookList.size() - 1));
            display.drawFastVLine(395, 60, 240, GxEPD_BLACK); 
            display.fillRoundRect(392, barY, 6, barHeight, 3, GxEPD_BLACK); 
        }
    } while (display.nextPage());
}

void renderPage() {
    String filePath = bookList[currentSelection];
    if (!filePath.startsWith("/")) filePath = "/" + filePath;

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
    
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        UIManager::drawTopBar();
        setFontSize(activeFontSize);
        
        File file = SD.open(filePath.c_str());
        if (file) {
            file.seek(pageOffsets[currentPage]);

            int max_width = 390; 
            int line_height = activeFontSize + (activeFontSize / 3);
            int x_pos = 10;
            int y_pos = TOP_BAR_H + activeFontSize + 6; 
            bool isNewLine = true;
            
            while (file.available()) {
                String word = "";
                bool paragraphBreak = false;
                bool hasSpace = false;

                while (file.available()) {
                    char c = file.read();
                    if (c == ' ') { hasSpace = true; break; }
                    else if (c == '\n') { paragraphBreak = true; break; }
                    else if (c != '\r') { word += c; }
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
                    if (y_pos >= BOT_BAR_Y) break; 
                    
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
                    if (y_pos >= BOT_BAR_Y) break; 
                }
            }
            
            UIManager::drawBottomBar(bookList[currentSelection], currentPage, pageOffsets.size());
            file.close();
        }
    } while (display.nextPage());
}

void openBook() {
    inReadingMode = true;
    prefs.putInt("bookIndex", currentSelection);

    String filePath = bookList[currentSelection];
    if (!filePath.startsWith("/")) filePath = "/" + filePath;

    if (filePath.endsWith(".epub") || filePath.endsWith(".EPUB")) {
        renderPage();
        return;
    }

    String cacheName = bookList[currentSelection];
    cacheName.replace("/", "_");
    cacheName.replace(".txt", "");
    cacheName.replace(".md", "");
    String cachePath = "/cache/" + cacheName + "_f" + String(currentFontIndex) + ".idx";

    if (!SD.exists("/cache")) SD.mkdir("/cache");

    if (!loadCache(cachePath)) {
        buildCache(filePath, cachePath, fontSizes[currentFontIndex]);
    }

    if (currentPage >= (int)pageOffsets.size()) {
        currentPage = 0;
    }
    renderPage();
}

// ==========================================
// 7. LIFECYCLE
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
        uint32_t savedPage = prefs.getUInt("pageIndex", 0);
        
        if (savedBook >= 0 && savedBook < (int)bookList.size()) {
            currentSelection = savedBook;
            currentPage = savedPage;
            openBook();
        } else {
            drawLibraryMenu(false); 
        }
    }
}

void loop() {
    handleWiFiClient(); 
    unsigned long currentMillis = millis();

    if (currentMillis - lastActionTime > 250) { 
        if (digitalRead(BTN_UP) == LOW) {
            lastActionTime = currentMillis;
            if (inReadingMode) {
                if (currentPage > 0) {
                    currentPage--;
                    prefs.putUInt("pageIndex", currentPage); 
                    renderPage();
                }
            } else if (!bookList.empty()) {
                currentSelection--;
                if (currentSelection < 0) currentSelection = bookList.size() - 1;
                drawLibraryMenu(true);
            }
        }
        
        if (digitalRead(BTN_DOWN) == LOW) {
            lastActionTime = currentMillis;
            if (inReadingMode) {
                String f = bookList[currentSelection];
                if (f.endsWith(".epub") || f.endsWith(".EPUB")) return;

                if (currentPage + 1 < (int)pageOffsets.size()) {
                    currentPage++;
                    prefs.putUInt("pageIndex", currentPage); 
                    renderPage();
                }
            } else if (!bookList.empty()) {
                currentSelection++;
                if (currentSelection >= (int)bookList.size()) currentSelection = 0;
                drawLibraryMenu(true);
            }
        }
        
        if (digitalRead(BTN_SELECT) == LOW) {
            lastActionTime = currentMillis;
            if (!inReadingMode && !bookList.empty()) {
                currentPage = 0; 
                openBook();
            }
        }
    }

    if (digitalRead(BTN_MENU) == LOW) {
        if (!menuPressed) {
            menuPressTime = currentMillis;
            menuPressed = true;
            menuLongHandled = false;
        } else if (!menuLongHandled && (currentMillis - menuPressTime > 800)) {
            menuLongHandled = true;
            if (isWifiConnected) {
                disableWiFi();
            } else {
                display.fillRect(0, 0, 400, TOP_BAR_H, GxEPD_BLACK);
                setFontSize(14);
                drawShapedText("Connecting to Wi-Fi...", 10, 15, GxEPD_WHITE);
                display.setPartialWindow(0, 0, 400, TOP_BAR_H);
                display.nextPage(); 
                initWiFi(WIFI_SSID, WIFI_PASS);
            }
            if (!inReadingMode) drawLibraryMenu(false);
            else renderPage();
        }
    } else {
        if (menuPressed) {
            if (!menuLongHandled && (currentMillis - menuPressTime > 50)) {
                if (inReadingMode) {
                    String f = bookList[currentSelection];
                    if (!f.endsWith(".epub") && !f.endsWith(".EPUB")) {
                        currentFontIndex++;
                        if (currentFontIndex > 3) currentFontIndex = 0;
                        
                        uint32_t savedOffset = pageOffsets[currentPage];
                        openBook(); 
                        
                        currentPage = 0;
                        for (int i = 0; i < (int)pageOffsets.size(); i++) {
                            if (pageOffsets[i] <= savedOffset) currentPage = i;
                            else break;
                        }
                        prefs.putUInt("pageIndex", currentPage);
                        renderPage(); 
                    }
                }
            }
            menuPressed = false;
        }
    }

    // BI-DIRECTIONAL JUMP LOGIC
    if (digitalRead(BTN_EXIT) == LOW) {
        if (!exitPressed) {
            exitPressTime = currentMillis;
            exitPressed = true;
            exitLongHandled = false;
        } else if (!exitLongHandled && (currentMillis - exitPressTime > 800)) {
            exitLongHandled = true;
            
            if (inReadingMode) {
                if (currentPage == 0) {
                    currentPage = pageOffsets.size() > 0 ? pageOffsets.size() - 1 : 0; // JUMP BOTTOM
                } else {
                    currentPage = 0; // JUMP TOP
                }
                prefs.putUInt("pageIndex", currentPage);
                
                display.setFullWindow();
                display.firstPage();
                do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());
                
                renderPage();
            } else {
                display.setFullWindow();
                display.firstPage();
                do { display.fillScreen(GxEPD_BLACK); } while (display.nextPage());
                drawLibraryMenu(false);
            }
        }
    } else {
        if (exitPressed) {
            if (!exitLongHandled && (currentMillis - exitPressTime > 50)) {
                if (inReadingMode) {
                    inReadingMode = false;
                    prefs.putInt("bookIndex", -1); 
                    drawLibraryMenu(false); 
                }
            }
            exitPressed = false;
        }
    }

    delay(10); 
}
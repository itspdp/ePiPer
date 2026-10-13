#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <vector>
#include <Preferences.h>
#include <time.h> 
#include <nvs_flash.h> // Required for Hardware Factory Reset

// ==========================================
// 1. HARDWARE CONFIG
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
#include "wifi_manager.h" 
#include "epub_engine.h" // Phase 8: EPUB Extraction Engine

// ==========================================
// 3. GLOBAL INSTANTIATIONS
// ==========================================
GxEPD2_BW<GxEPD2_420_SE0420NQ04, GxEPD2_420_SE0420NQ04::HEIGHT> display(
    GxEPD2_420_SE0420NQ04(EPD_CS, EPD_DC, EPD_RES, EPD_BUSY)
);
SPIClass sdSPI(FSPI);
Preferences prefs;

std::vector<FileNode> fileList;
String currentDirectory = "/";

uint8_t* font_buffer = nullptr;
FT_Library ft_library;
FT_Face ft_face;
hb_font_t *hb_font;

int currentSelection = 0;
bool inReadingMode = false;

int fontSizes[] = {20, 26, 34, 48}; 
int currentFontIndex = 2; 

// Font-Agnostic Navigation Arrays
std::vector<uint32_t> paraCache; 
uint32_t currentByteOffset = 0;

unsigned long menuPressTime = 0, exitPressTime = 0, lastActionTime = 0;
bool menuPressed = false, menuLongHandled = false;
bool exitPressed = false, exitLongHandled = false;

#define TOP_BAR_H 20
#define BOT_BAR_Y 276

// ==========================================
// 4. FONT-AGNOSTIC PARAGRAPH CACHING
// ==========================================
bool loadParagraphCache(String cachePath) {
    File cacheFile = SD.open(cachePath.c_str(), FILE_READ);
    if (!cacheFile) return false;
    paraCache.clear();
    while (cacheFile.available()) {
        uint32_t offset;
        if (cacheFile.read((uint8_t*)&offset, sizeof(offset)) == sizeof(offset)) {
            paraCache.push_back(offset);
        }
        yield(); // WATCHDOG SAFETY: Pet the dog during large cache loads
    }
    cacheFile.close();
    return !paraCache.empty();
}

void buildParagraphCache(String filePath, String cachePath) {
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        setFontSize(24);
        drawShapedText("Indexing Document...", 10, 100);
        setFontSize(16);
        drawShapedText("Generating a font-agnostic paragraph map.", 10, 150);
        drawShapedText("This ensures flawless multi-size caching.", 10, 180);
    } while (display.nextPage());

    paraCache.clear();
    paraCache.push_back(0);
    
    File file = SD.open(filePath.c_str());
    if (!file) return;

    // Track loops to pet the watchdog efficiently
    int loops = 0; 
    while (file.available()) {
        char c = file.read();
        if (c == '\n' && file.available()) {
            paraCache.push_back(file.position());
        }
        
        // WATCHDOG SAFETY: Pet the dog every 10,000 characters
        if (loops++ % 10000 == 0) yield(); 
    }
    file.close();

    File cacheFile = SD.open(cachePath.c_str(), FILE_WRITE);
    if (cacheFile) {
        for (uint32_t offset : paraCache) {
            cacheFile.write((uint8_t*)&offset, sizeof(offset));
            yield(); // WATCHDOG SAFETY
        }
        cacheFile.close();
    }
}

// O(1) Reverse Pagination Simulation
uint32_t calculatePreviousPageOffset(String filePath, uint32_t currentOffset, int activeFontSize) {
    if (currentOffset == 0 || paraCache.empty()) return 0;

    int paraIndex = 0;
    for (size_t i = 0; i < paraCache.size(); i++) {
        if (paraCache[i] >= currentOffset) break;
        paraIndex = i;
    }

    // Step back 4 paragraphs to guarantee enough text to fill a preceding screen
    int startPara = max(0, paraIndex - 4);
    uint32_t simOffset = paraCache[startPara];
    
    uint32_t lastPageStart = simOffset;
    uint32_t prevPageStart = simOffset;

    File file = SD.open(filePath.c_str());
    if (!file) return 0;
    file.seek(simOffset);

    int max_width = 390; 
    int line_height = activeFontSize + (activeFontSize / 3);
    int x_pos = 10;
    int y_pos = TOP_BAR_H + activeFontSize + 6; 
    bool isNewLine = true;
    
    int loops = 0; // Watchdog counter

    while (file.available()) {
        if (loops++ % 5000 == 0) yield(); // WATCHDOG SAFETY during word-wrap simulation

        if (file.position() >= currentOffset) {
            file.close();
            return prevPageStart; 
        }

        uint32_t tokenStart = file.position();
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
                line_height = (activeFontSize * scaleMap[hashCount - 1]) * 1.3;
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
                prevPageStart = lastPageStart;
                lastPageStart = tokenStart;
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
            line_height = activeFontSize + (activeFontSize / 3);
            if (y_pos >= BOT_BAR_Y) {
                if (file.available()) {
                    prevPageStart = lastPageStart;
                    lastPageStart = file.position();
                }
                x_pos = 10;
                y_pos = TOP_BAR_H + activeFontSize + 6; 
            }
        }
    }
    file.close();
    return prevPageStart;
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
        
        String wifiStr;
        if (isAPMode) wifiStr = "AP: " + currentIP;
        else if (isWifiConnected) wifiStr = currentIP;
        else wifiStr = "Wi-Fi: OFF";
        
        drawShapedText(wifiStr.c_str(), 10, 15, GxEPD_WHITE);

        updateInternalClock();
        String battStr = isCharging() ? "[USB]" : "[" + String(getBatteryPercentage()) + "%]";
        String rightHUD = battStr + "  " + currentTime;
        
        int hudWidth = getTextWidth(rightHUD.c_str());
        drawShapedText(rightHUD.c_str(), 390 - hudWidth, 15, GxEPD_WHITE);
    }

    void drawBottomBar(String title, int percent) {
        display.fillRect(0, BOT_BAR_Y, 400, 24, GxEPD_BLACK); 
        setFontSize(18); 
        
        title.replace(".txt", "");
        title.replace(".TXT", "");
        title.replace(".md", "");
        title.replace(".MD", "");
        title.replace(".epub", "");
        title.replace(".EPUB", "");

        if (title.length() > 22) title = title.substring(0, 19) + "...";
        drawShapedText(title.c_str(), 10, 294, GxEPD_WHITE);

        String progress = String(percent) + "%";
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
        
        String pathTitle = currentDirectory == "/" ? "Local Storage (SD)" : currentDirectory;
        if (pathTitle.length() > 25) pathTitle = "..." + pathTitle.substring(pathTitle.length() - 22);
        drawShapedText(pathTitle.c_str(), 10, TOP_BAR_H + 24, GxEPD_BLACK);
        
        if (fileList.empty()) {
            drawShapedText("Folder is empty.", 10, 110);
            continue;
        }

        int visibleItems = 6;
        int startIndex = 0;
        if (currentSelection >= visibleItems) startIndex = currentSelection - visibleItems + 1; 

        for (int i = startIndex; i < (int)fileList.size() && i < startIndex + visibleItems; i++) {
            int y_pos = 90 + ((i - startIndex) * 34); 
            
            String displayName = fileList[i].name;
            if (fileList[i].isDir && displayName != "[..] UP") displayName = "[" + displayName + "]";
            else {
                displayName.replace(".txt", "");
                displayName.replace(".TXT", "");
                displayName.replace(".md", "");
                displayName.replace(".MD", "");
                displayName.replace(".epub", "");
                displayName.replace(".EPUB", "");
            }

            if (i == currentSelection) {
                display.fillRoundRect(5, y_pos - 24, 380, 32, 4, GxEPD_BLACK);
                drawShapedText(displayName.c_str(), 15, y_pos - 2, GxEPD_WHITE);
            } else {
                drawShapedText(displayName.c_str(), 15, y_pos - 2, GxEPD_BLACK);
            }
        }
        
        if (fileList.size() > visibleItems) {
            int barHeight = max(20, (230 * visibleItems) / (int)fileList.size());
            int barY = 65 + ((currentSelection * (230 - barHeight)) / (fileList.size() - 1));
            display.drawFastVLine(395, 60, 240, GxEPD_BLACK); 
            display.fillRoundRect(392, barY, 6, barHeight, 3, GxEPD_BLACK); 
        }
    } while (display.nextPage());
}

void renderPage(String targetFilePath) {
    int activeFontSize = fontSizes[currentFontIndex];
    uint32_t nextByteOffset = currentByteOffset;
    uint32_t fileSize = 1;
    
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        UIManager::drawTopBar();
        setFontSize(activeFontSize);
        
        File file = SD.open(targetFilePath.c_str());
        if (file) {
            fileSize = file.size();
            file.seek(currentByteOffset);

            int max_width = 390; 
            int line_height = activeFontSize + (activeFontSize / 3);
            int x_pos = 10;
            int y_pos = TOP_BAR_H + activeFontSize + 6; 
            bool isNewLine = true;
            
            int loops = 0; // Watchdog counter
            
            while (file.available()) {
                if (loops++ % 5000 == 0) yield(); // WATCHDOG SAFETY during active reading render

                uint32_t tokenStart = file.position();
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
                        nextByteOffset = tokenStart;
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
                    if (y_pos >= BOT_BAR_Y) {
                        nextByteOffset = file.position();
                        break; 
                    }
                }
            }
            if (!file.available()) nextByteOffset = file.position(); 
            
            int percent = (currentByteOffset * 100) / fileSize;
            UIManager::drawBottomBar(fileList[currentSelection].name, percent);
            file.close();
        }
    } while (display.nextPage());

    if (nextByteOffset > currentByteOffset) currentByteOffset = nextByteOffset;
}

void openBook() {
    inReadingMode = true;
    prefs.putString("lastBook", fileList[currentSelection].fullPath);

    String filePath = fileList[currentSelection].fullPath;
    String cleanName = fileList[currentSelection].name;
    cleanName.replace(".txt", "");
    cleanName.replace(".md", "");
    cleanName.replace(".epub", "");
    cleanName.replace(".EPUB", "");
    
    // If it's an EPUB, we target the hidden TXT extraction on the SD Card
    String targetRenderPath = filePath;

    if (!SD.exists("/cache")) SD.mkdir("/cache");

    if (filePath.endsWith(".epub") || filePath.endsWith(".EPUB")) {
        targetRenderPath = "/cache/" + cleanName + "_epub.txt";
        
        // Only unzip if the text cache doesn't already exist
        if (!SD.exists(targetRenderPath.c_str())) {
            display.setFullWindow();
            display.firstPage();
            do {
                display.fillScreen(GxEPD_WHITE);
                setFontSize(24);
                drawShapedText("Extracting EPUB Archive...", 10, 100);
                setFontSize(16);
                drawShapedText("Unzipping HTML and stripping web tags.", 10, 150);
                drawShapedText("This happens only ONCE per book.", 10, 180);
            } while (display.nextPage());

            if (!convertEpubToTxt(filePath, targetRenderPath)) {
                // Failsafe if ZIP is corrupt
                inReadingMode = false;
                drawLibraryMenu(false);
                return;
            }
        }
    }

    String cachePath = "/cache/" + cleanName + "_para.idx";

    if (!loadParagraphCache(cachePath)) {
        buildParagraphCache(targetRenderPath, cachePath); // Pass targetRenderPath to handle both standard and EPUB files cleanly!
    }
    
    currentByteOffset = prefs.getUInt("pageByte", 0);
    renderPage(targetRenderPath);
}

// ==========================================
// 7. LIFECYCLE
// ==========================================
void setup() {
    Serial.begin(115200);
    
    // ----------------------------------------------------
    // HARDWARE FACTORY RESET (HOLD EXIT ON BOOT)
    // ----------------------------------------------------
    pinMode(BTN_EXIT, INPUT_PULLUP);
    delay(100); // Debounce physical boot button hold
    if (digitalRead(BTN_EXIT) == LOW) {
        Serial.println("FACTORY RESET INITIATED.");
        
        nvs_flash_erase(); // Nuke the bookmark partition
        nvs_flash_init();
        
        pinMode(EPD_PWR, OUTPUT);
        digitalWrite(EPD_PWR, HIGH); 
        SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);
        display.init(115200, true, 2, false);
        display.setFullWindow();
        display.firstPage();
        do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());

        if (SD.begin(SD_CS, sdSPI, 4000000)) {
            File cacheDir = SD.open("/cache");
            if (cacheDir && cacheDir.isDirectory()) {
                File file = cacheDir.openNextFile();
                while(file){
                    String filepath = String("/cache/") + file.name();
                    file.close();
                    SD.remove(filepath.c_str());
                    file = cacheDir.openNextFile();
                }
                cacheDir.close();
            }
        }
        
        Serial.println("RESET COMPLETE. PLEASE REBOOT.");
        while(true) delay(100); // Lock device until power cycle
    }
    // ----------------------------------------------------

    prefs.begin("epiper", false);
    
    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_SELECT, INPUT_PULLUP);
    pinMode(BTN_MENU, INPUT_PULLUP);

    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, HIGH); 
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);
    display.init(115200, true, 2, false);
    
    showBootScreen();
    
    if (mountSDCard() && initRawEngine()) {
        scanDirectory("/");
        
        String savedBook = prefs.getString("lastBook", "");
        if (savedBook != "") {
            for (size_t i = 0; i < fileList.size(); i++) {
                if (fileList[i].fullPath == savedBook) {
                    currentSelection = i;
                    openBook();
                    return;
                }
            }
        }
        drawLibraryMenu(false); 
    }
}

void loop() {
    handleWiFiClient(); 
    unsigned long currentMillis = millis();

    if (currentMillis - lastActionTime > 250) { 
        if (digitalRead(BTN_UP) == LOW) {
            lastActionTime = currentMillis;
            if (inReadingMode) {
                if (currentByteOffset > 0) {
                    String targetRenderPath = fileList[currentSelection].fullPath;
                    if (targetRenderPath.endsWith(".epub") || targetRenderPath.endsWith(".EPUB")) {
                        String cleanName = fileList[currentSelection].name;
                        cleanName.replace(".epub", ""); cleanName.replace(".EPUB", "");
                        targetRenderPath = "/cache/" + cleanName + "_epub.txt";
                    }
                    currentByteOffset = calculatePreviousPageOffset(targetRenderPath, currentByteOffset, fontSizes[currentFontIndex]);
                    prefs.putUInt("pageByte", currentByteOffset); 
                    renderPage(targetRenderPath);
                }
            } else if (!fileList.empty()) {
                currentSelection--;
                if (currentSelection < 0) currentSelection = fileList.size() - 1;
                drawLibraryMenu(true);
            }
        }
        
        if (digitalRead(BTN_DOWN) == LOW) {
            lastActionTime = currentMillis;
            if (inReadingMode) {
                String targetRenderPath = fileList[currentSelection].fullPath;
                if (targetRenderPath.endsWith(".epub") || targetRenderPath.endsWith(".EPUB")) {
                    String cleanName = fileList[currentSelection].name;
                    cleanName.replace(".epub", ""); cleanName.replace(".EPUB", "");
                    targetRenderPath = "/cache/" + cleanName + "_epub.txt";
                }
                prefs.putUInt("pageByte", currentByteOffset); 
                renderPage(targetRenderPath);
            } else if (!fileList.empty()) {
                currentSelection++;
                if (currentSelection >= (int)fileList.size()) currentSelection = 0;
                drawLibraryMenu(true);
            }
        }
        
        if (digitalRead(BTN_SELECT) == LOW) {
            lastActionTime = currentMillis;
            if (!inReadingMode && !fileList.empty()) {
                if (fileList[currentSelection].isDir) {
                    scanDirectory(fileList[currentSelection].fullPath);
                    currentSelection = 0;
                    drawLibraryMenu(false);
                } else {
                    currentByteOffset = 0; 
                    openBook();
                }
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
                initWiFi(); 
            }
            if (!inReadingMode) drawLibraryMenu(false);
            else {
                String targetRenderPath = fileList[currentSelection].fullPath;
                if (targetRenderPath.endsWith(".epub") || targetRenderPath.endsWith(".EPUB")) {
                    String cleanName = fileList[currentSelection].name;
                    cleanName.replace(".epub", ""); cleanName.replace(".EPUB", "");
                    targetRenderPath = "/cache/" + cleanName + "_epub.txt";
                }
                uint32_t holdByte = currentByteOffset;
                currentByteOffset = calculatePreviousPageOffset(targetRenderPath, holdByte, fontSizes[currentFontIndex]);
                renderPage(targetRenderPath);
            }
        }
    } else {
        if (menuPressed) {
            if (!menuLongHandled && (currentMillis - menuPressTime > 50)) {
                if (inReadingMode) {
                    String targetRenderPath = fileList[currentSelection].fullPath;
                    if (targetRenderPath.endsWith(".epub") || targetRenderPath.endsWith(".EPUB")) {
                        String cleanName = fileList[currentSelection].name;
                        cleanName.replace(".epub", ""); cleanName.replace(".EPUB", "");
                        targetRenderPath = "/cache/" + cleanName + "_epub.txt";
                    }
                    
                    currentFontIndex++;
                    if (currentFontIndex > 3) currentFontIndex = 0;
                    
                    uint32_t holdByte = currentByteOffset;
                    currentByteOffset = calculatePreviousPageOffset(targetRenderPath, holdByte, fontSizes[currentFontIndex]);
                    prefs.putUInt("pageByte", currentByteOffset);
                    renderPage(targetRenderPath); 
                }
            }
            menuPressed = false;
        }
    }

    if (digitalRead(BTN_EXIT) == LOW) {
        if (!exitPressed) {
            exitPressTime = currentMillis;
            exitPressed = true;
            exitLongHandled = false;
        } else if (!exitLongHandled && (currentMillis - exitPressTime > 800)) {
            exitLongHandled = true;
            
            if (inReadingMode) {
                if (currentByteOffset == 0) {
                    currentByteOffset = paraCache.empty() ? 0 : paraCache.back();
                } else {
                    currentByteOffset = 0; 
                }
                prefs.putUInt("pageByte", currentByteOffset);
                
                display.setFullWindow();
                display.firstPage();
                do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());
                
                String targetRenderPath = fileList[currentSelection].fullPath;
                if (targetRenderPath.endsWith(".epub") || targetRenderPath.endsWith(".EPUB")) {
                    String cleanName = fileList[currentSelection].name;
                    cleanName.replace(".epub", ""); cleanName.replace(".EPUB", "");
                    targetRenderPath = "/cache/" + cleanName + "_epub.txt";
                }
                renderPage(targetRenderPath);
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
                    prefs.putString("lastBook", ""); 
                    scanDirectory(currentDirectory); 
                    drawLibraryMenu(false); 
                }
            }
            exitPressed = false;
        }
    }

    delay(10); 
}
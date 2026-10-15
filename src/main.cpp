#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <vector>
#include <Preferences.h>
#include <time.h> 
#include <nvs_flash.h>

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

#define BTN_UP        6  
#define BTN_DOWN      4  
#define BTN_SELECT    5  
#define BTN_MENU      2
#define BTN_EXIT      1

// ==========================================
// 2. FOUNDATIONAL DATA STRUCTURES
// ==========================================
// System storage defines FileNode, which is needed by the global variables below.
#include "system_storage.h"

// Text engine includes FreeType and HarfBuzz headers, which define FT_Library, etc.
#include "text_engine.h"

std::vector<FileNode> fileList;
String currentDirectory = "/";

// ==========================================
// 3. GLOBAL STATE DEFINITIONS
// ==========================================
GxEPD2_BW<GxEPD2_420_SE0420NQ04, GxEPD2_420_SE0420NQ04::HEIGHT> display(
    GxEPD2_420_SE0420NQ04(EPD_CS, EPD_DC, EPD_RES, EPD_BUSY)
);
SPIClass sdSPI(FSPI);
Preferences prefs;

// Typography Global Pointers (Types now defined by text_engine.h)
uint8_t* font_buffer = nullptr;
FT_Library ft_library;
FT_Face ft_face;
hb_font_t *hb_font;

// Reading State
int currentSelection = 0;
bool inReadingMode = false;
int fontSizes[] = {20, 26, 34, 48}; 
int currentFontIndex = 2; 

// Pagination Cache and active read-head
std::vector<uint32_t> paraCache; 
uint32_t currentByteOffset = 0;

// Button Debounce Timers
unsigned long menuPressTime = 0, exitPressTime = 0, lastActionTime = 0;
bool menuPressed = false, menuLongHandled = false;
bool exitPressed = false, exitLongHandled = false;

// ==========================================
// 4. ENGINE INCLUDES
// ==========================================
// Now that all globals and structs are defined, load the UI and Logic engines
#include "splash.h"
#include "ui_engine.h"
#include "pagination_engine.h"
#include "epub_engine.h"
#include "wifi_manager.h" 

// ==========================================
// 5. CORE RENDERER
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
            int loops = 0; 
            
            while (file.available()) {
                if (loops++ % 5000 == 0) yield(); 

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

                word.replace("**", ""); word.replace("__", "");
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

// ==========================================
// 6. BOOK INITIALIZER
// ==========================================
void openBook() {
    inReadingMode = true;
    prefs.putString("lastBook", fileList[currentSelection].fullPath);

    String filePath = fileList[currentSelection].fullPath;
    String cleanName = fileList[currentSelection].name;
    cleanName.replace(".txt", ""); cleanName.replace(".md", "");
    cleanName.replace(".epub", ""); cleanName.replace(".EPUB", "");
    
    String targetRenderPath = filePath;
    if (!SD.exists("/cache")) SD.mkdir("/cache");

    if (filePath.endsWith(".epub") || filePath.endsWith(".EPUB")) {
        targetRenderPath = "/cache/" + cleanName + "_epub.txt";
        
        if (!SD.exists(targetRenderPath.c_str())) {
            display.setPartialWindow(0, BOT_BAR_Y, 400, 24);
            display.firstPage();
            do {
                display.fillRect(0, BOT_BAR_Y, 400, 24, GxEPD_BLACK);
                setFontSize(18);
                drawShapedText("Extracting EPUB Archive...", 10, 294, GxEPD_WHITE);
            } while (display.nextPage());

            if (!convertEpubToTxt(filePath, targetRenderPath)) {
                inReadingMode = false;
                UIManager::drawLibraryMenu(false);
                return;
            }
        }
    }

    String cachePath = "/cache/" + cleanName + "_para.idx";
    if (!loadParagraphCache(cachePath)) {
        buildParagraphCache(targetRenderPath, cachePath); 
    }
    
    currentByteOffset = prefs.getUInt("pageByte", 0);
    renderPage(targetRenderPath);
}

// ==========================================
// 7. OS LIFECYCLE
// ==========================================
void setup() {
    Serial.begin(115200);
    
    // HARDWARE FACTORY RESET (HOLD EXIT ON BOOT)
    pinMode(BTN_EXIT, INPUT_PULLUP);
    delay(100); 
    if (digitalRead(BTN_EXIT) == LOW) {
        nvs_flash_erase(); 
        nvs_flash_init();
        
        pinMode(EPD_PWR, OUTPUT);
        digitalWrite(EPD_PWR, HIGH); 
        SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);
        display.init(115200, true, 2, false);
        UIManager::drawFullscreenMessage("Factory Reset", "Memory Wiped. Please Reboot.");

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
        while(true) delay(100); 
    }

    prefs.begin("epiper", false);
    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_SELECT, INPUT_PULLUP);
    pinMode(BTN_MENU, INPUT_PULLUP);

    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, HIGH); 
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);
    display.init(115200, true, 2, false);
    
    // Fixed: Standard Global Function Call
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
        UIManager::drawLibraryMenu(false); 
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
                UIManager::drawLibraryMenu(true);
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
                UIManager::drawLibraryMenu(true);
            }
        }
        
        if (digitalRead(BTN_SELECT) == LOW) {
            lastActionTime = currentMillis;
            if (!inReadingMode && !fileList.empty()) {
                if (fileList[currentSelection].isDir) {
                    scanDirectory(fileList[currentSelection].fullPath);
                    currentSelection = 0;
                    UIManager::drawLibraryMenu(false);
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
            if (!inReadingMode) UIManager::drawLibraryMenu(false);
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
                String targetRenderPath = fileList[currentSelection].fullPath;
                if (targetRenderPath.endsWith(".epub") || targetRenderPath.endsWith(".EPUB")) {
                    String cleanName = fileList[currentSelection].name;
                    cleanName.replace(".epub", ""); cleanName.replace(".EPUB", "");
                    targetRenderPath = "/cache/" + cleanName + "_epub.txt";
                }

                if (currentByteOffset == 0) {
                    if (paraCache.size() > 4) {
                        currentByteOffset = calculatePreviousPageOffset(targetRenderPath, paraCache[paraCache.size() - 4], fontSizes[currentFontIndex]);
                    } else {
                        currentByteOffset = 0;
                    }
                } else {
                    currentByteOffset = 0; 
                }
                
                prefs.putUInt("pageByte", currentByteOffset);
                renderPage(targetRenderPath);
                
            } else {
                display.setFullWindow();
                display.firstPage();
                do { display.fillScreen(GxEPD_BLACK); } while (display.nextPage());
                UIManager::drawLibraryMenu(false);
            }
        }
    } else {
        if (exitPressed) {
            if (!exitLongHandled && (currentMillis - exitPressTime > 50)) {
                if (inReadingMode) {
                    inReadingMode = false;
                    prefs.putString("lastBook", ""); 
                    scanDirectory(currentDirectory); 
                    UIManager::drawLibraryMenu(false); 
                }
            }
            exitPressed = false;
        }
    }

    delay(10); 
}
#ifndef UI_ENGINE_H
#define UI_ENGINE_H

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include "system_storage.h"
#include "text_engine.h"
#include <time.h> 

// ---------------------------------------------------------
// UI LAYOUT CONSTANTS
// ---------------------------------------------------------
#define TOP_BAR_H 20
#define BOT_BAR_Y 276

// ---------------------------------------------------------
// EXTERNAL STATE VARIABLES (Defined in main.cpp)
// ---------------------------------------------------------
extern GxEPD2_BW<GxEPD2_420_SE0420NQ04, GxEPD2_420_SE0420NQ04::HEIGHT> display;
extern std::vector<FileNode> fileList;
extern String currentDirectory;
extern int currentSelection;
extern bool isWifiConnected;
extern bool isAPMode;
extern String currentIP;

// ---------------------------------------------------------
// UI MANAGER NAMESPACE
// ---------------------------------------------------------
namespace UIManager {
    String currentTime = "12:00 PM"; 
    
    // Hardware abstractions for future Battery/Power IC integration
    inline bool isCharging() { return true; } 
    inline int getBatteryPercentage() { return 100; } 

    // Synchronizes with the ESP32 internal RTC (updated via NTP when online)
    inline void updateInternalClock() {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 10)) { 
            char timeBuf[10];
            strftime(timeBuf, sizeof(timeBuf), "%I:%M %p", &timeinfo);
            currentTime = String(timeBuf);
        } else {
            // Fallback: estimate time based on millis() if NTP failed
            unsigned long totalMinutes = millis() / 60000;
            int hours = (12 + (totalMinutes / 60)) % 12;
            if (hours == 0) hours = 12;
            int mins = totalMinutes % 60;
            char timeBuf[10];
            snprintf(timeBuf, sizeof(timeBuf), "%d:%02d %s", hours, mins, (millis() % 86400000 < 43200000) ? "PM" : "AM");
            currentTime = String(timeBuf);
        }
    }

    // Renders the System Status Header (Wi-Fi, Battery, Time)
    inline void drawTopBar() {
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

    // Renders the Reading Progress Footer (Book Title, % Read)
    inline void drawBottomBar(String title, int percent) {
        display.fillRect(0, BOT_BAR_Y, 400, 24, GxEPD_BLACK); 
        setFontSize(18); 
        
        // Clean extensions for the UI
        title.replace(".txt", ""); title.replace(".TXT", "");
        title.replace(".md", ""); title.replace(".MD", "");
        title.replace(".epub", ""); title.replace(".EPUB", "");

        if (title.length() > 22) title = title.substring(0, 19) + "...";
        drawShapedText(title.c_str(), 10, 294, GxEPD_WHITE);

        String progress = String(percent) + "%";
        int progWidth = getTextWidth(progress.c_str());
        drawShapedText(progress.c_str(), 390 - progWidth, 294, GxEPD_WHITE);
    }

    // Renders the Main File Explorer Menu
    inline void drawLibraryMenu(bool partialRefresh = false) {
        display.setRotation(0);
        
        // Use partial refresh for rapid scrolling, full refresh for directory changes
        if (partialRefresh) display.setPartialWindow(0, TOP_BAR_H, 400, 300 - TOP_BAR_H); 
        else display.setFullWindow();
        
        display.firstPage();
        do {
            display.fillScreen(GxEPD_WHITE);
            if (!partialRefresh) drawTopBar();

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
            // Scroll logic: keep selection at the bottom of the viewport when moving down
            if (currentSelection >= visibleItems) startIndex = currentSelection - visibleItems + 1; 

            for (int i = startIndex; i < (int)fileList.size() && i < startIndex + visibleItems; i++) {
                int y_pos = 90 + ((i - startIndex) * 34); 
                
                String displayName = fileList[i].name;
                if (fileList[i].isDir && displayName != "[..] UP") displayName = "[" + displayName + "]";
                else {
                    displayName.replace(".txt", ""); displayName.replace(".TXT", "");
                    displayName.replace(".md", ""); displayName.replace(".MD", "");
                    displayName.replace(".epub", ""); displayName.replace(".EPUB", "");
                }

                // Highlight currently selected item
                if (i == currentSelection) {
                    display.fillRoundRect(5, y_pos - 24, 380, 32, 4, GxEPD_BLACK);
                    drawShapedText(displayName.c_str(), 15, y_pos - 2, GxEPD_WHITE);
                } else {
                    drawShapedText(displayName.c_str(), 15, y_pos - 2, GxEPD_BLACK);
                }
            }
            
            // Draw Scrollbar if list exceeds screen space
            if (fileList.size() > visibleItems) {
                int barHeight = max(20, (230 * visibleItems) / (int)fileList.size());
                int barY = 65 + ((currentSelection * (230 - barHeight)) / (fileList.size() - 1));
                display.drawFastVLine(395, 60, 240, GxEPD_BLACK); 
                display.fillRoundRect(392, barY, 6, barHeight, 3, GxEPD_BLACK); 
            }
        } while (display.nextPage());
    }

    // Emergency/Diagnostic Full Screen Interrupter (e.g., Factory Reset UI)
    inline void drawFullscreenMessage(String title, String msg) {
        display.setFullWindow();
        display.firstPage();
        do {
            display.fillScreen(GxEPD_WHITE);
            drawTopBar();
            setFontSize(24);
            drawShapedText(title.c_str(), 10, 100);
            setFontSize(16);
            drawShapedText(msg.c_str(), 10, 150);
        } while (display.nextPage());
    }
}
#endif
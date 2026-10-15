#ifndef PAGINATION_ENGINE_H
#define PAGINATION_ENGINE_H

#include <Arduino.h>
#include <SD.h>
#include <vector>
#include "text_engine.h"
#include "ui_engine.h" // Gives us access to TOP_BAR_H, BOT_BAR_Y, and UIManager

// ---------------------------------------------------------
// PARAGRAPH CACHE ARRAY
// ---------------------------------------------------------
// Stores the exact byte offset for every '\n' in the text file.
extern std::vector<uint32_t> paraCache;

// Reads a pre-built .idx file from the SD card into PSRAM
inline bool loadParagraphCache(String cachePath) {
    File cacheFile = SD.open(cachePath.c_str(), FILE_READ);
    if (!cacheFile) return false;
    
    paraCache.clear();
    while (cacheFile.available()) {
        uint32_t offset;
        if (cacheFile.read((uint8_t*)&offset, sizeof(offset)) == sizeof(offset)) {
            paraCache.push_back(offset);
        }
        yield(); // WATCHDOG: Prevent crash on large .idx loads
    }
    cacheFile.close();
    return !paraCache.empty();
}

// Scans a raw text file and writes an array of '\n' offsets to the SD card
inline void buildParagraphCache(String filePath, String cachePath) {
    // Subtle UI: Uses the bottom bar instead of a full screen flash
    display.setPartialWindow(0, BOT_BAR_Y, 400, 24);
    display.firstPage();
    do {
        display.fillRect(0, BOT_BAR_Y, 400, 24, GxEPD_BLACK);
        setFontSize(18);
        drawShapedText("Building Paragraph Cache...", 10, 294, GxEPD_WHITE);
    } while (display.nextPage());

    paraCache.clear();
    paraCache.push_back(0); // Offset 0 is always the start of the book
    
    File file = SD.open(filePath.c_str());
    if (!file) return;

    int loops = 0; 
    while (file.available()) {
        char c = file.read();
        if (c == '\n' && file.available()) {
            paraCache.push_back(file.position());
        }
        // WATCHDOG: Prevent RTOS CPU Starvation during heavy file reads
        if (loops++ % 10000 == 0) yield(); 
    }
    file.close();

    File cacheFile = SD.open(cachePath.c_str(), FILE_WRITE);
    if (cacheFile) {
        for (uint32_t offset : paraCache) {
            cacheFile.write((uint8_t*)&offset, sizeof(offset));
            yield(); 
        }
        cacheFile.close();
    }
}

// ---------------------------------------------------------
// O(1) REVERSE SCROLL SIMULATOR
// ---------------------------------------------------------
// When scrolling UP, this engine skips back 4 paragraphs using the cache, 
// then simulates rendering FORWARD invisibly to find exactly where the 
// preceding page is mathematically supposed to start wrapping.
inline uint32_t calculatePreviousPageOffset(String filePath, uint32_t currentOffset, int activeFontSize) {
    if (currentOffset == 0 || paraCache.empty()) return 0;

    // Find our current location in the paragraph index
    int paraIndex = 0;
    for (size_t i = 0; i < paraCache.size(); i++) {
        if (paraCache[i] >= currentOffset) {
            paraIndex = i;
            break; // Stop immediately to prevent seeking past our target
        }
        paraIndex = i;
    }

    // Step back 4 paragraphs (Safe bounding: prevents dropping below offset 0)
    int startPara = max(0, paraIndex - 4);
    uint32_t simOffset = paraCache[startPara];
    
    uint32_t lastPageStart = simOffset;
    uint32_t prevPageStart = simOffset;

    File file = SD.open(filePath.c_str());
    if (!file) return 0;
    file.seek(simOffset);

    // Initialize layout simulation variables
    int max_width = 390; 
    int line_height = activeFontSize + (activeFontSize / 3);
    int x_pos = 10;
    int y_pos = TOP_BAR_H + activeFontSize + 6; 
    bool isNewLine = true;
    int loops = 0; 

    while (file.available()) {
        if (loops++ % 5000 == 0) yield(); // WATCHDOG SAFETY

        // Halt simulation once we hit the target offset
        if (file.position() >= currentOffset) {
            file.close();
            return prevPageStart; 
        }

        uint32_t tokenStart = file.position();
        String word = "";
        bool paragraphBreak = false;
        bool hasSpace = false;

        // Tokenize the next word
        while (file.available()) {
            char c = file.read();
            if (c == ' ') { hasSpace = true; break; }
            else if (c == '\n') { paragraphBreak = true; break; }
            else if (c != '\r') { word += c; }
        }

        // Markdown stripping
        word.replace("**", "");
        word.replace("__", "");

        if (word.length() == 0 && !paragraphBreak) continue;

        // Simulate Markdown Headers
        if (isNewLine && word.startsWith("#")) {
            int hashCount = 0;
            while (word.length() > hashCount && word[hashCount] == '#') hashCount++;
            if (hashCount > 0 && hashCount <= 6) {
                float scaleMap[] = {1.6, 1.4, 1.3, 1.2, 1.1, 1.1}; 
                line_height = (activeFontSize * scaleMap[hashCount - 1]) * 1.3;
                word = word.substring(hashCount); 
            }
        }

        // Simulate word wrapping math
        if (word.length() > 0) {
            int pure_word_width = getTextWidth(word.c_str());
            if (x_pos + pure_word_width > max_width && x_pos > 10) {
                x_pos = 10;
                y_pos += line_height;
            }
            
            // If the word crosses the bottom boundary, log the start of a new simulated page
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

        // Simulate paragraph breaks
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

#endif
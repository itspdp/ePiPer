#ifndef PAGINATION_ENGINE_H
#define PAGINATION_ENGINE_H

#include <Arduino.h>
#include <SD.h>
#include <vector>
#include "text_engine.h"
#include "ui_engine.h"

extern std::vector<uint32_t> paraCache;

inline bool loadParagraphCache(String cachePath) {
    File cacheFile = SD.open(cachePath.c_str(), FILE_READ);
    if (!cacheFile) return false;
    paraCache.clear();
    while (cacheFile.available()) {
        uint32_t offset;
        if (cacheFile.read((uint8_t*)&offset, sizeof(offset)) == sizeof(offset)) {
            paraCache.push_back(offset);
        }
        yield(); // Watchdog safety
    }
    cacheFile.close();
    return !paraCache.empty();
}

inline void buildParagraphCache(String filePath, String cachePath) {
    UIManager::drawProgressBar("System Cache", "Indexing paragraphs...", 10);

    paraCache.clear();
    paraCache.push_back(0);
    
    File file = SD.open(filePath.c_str());
    if (!file) return;

    size_t fileSize = file.size();
    int loops = 0; 

    while (file.available()) {
        char c = file.read();
        if (c == '\n' && file.available()) {
            paraCache.push_back(file.position());
        }
        
        if (loops++ % 5000 == 0) {
            int percent = (file.position() * 100) / fileSize;
            UIManager::drawProgressBar("System Cache", "Indexing paragraphs...", percent);
            yield(); 
        }
    }
    file.close();

    UIManager::drawProgressBar("System Cache", "Saving map to SD...", 95);
    File cacheFile = SD.open(cachePath.c_str(), FILE_WRITE);
    if (cacheFile) {
        for (uint32_t offset : paraCache) {
            cacheFile.write((uint8_t*)&offset, sizeof(offset));
            yield();
        }
        cacheFile.close();
    }
}

inline uint32_t calculatePreviousPageOffset(String filePath, uint32_t currentOffset, int activeFontSize) {
    if (currentOffset == 0 || paraCache.empty()) return 0;

    int paraIndex = 0;
    for (size_t i = 0; i < paraCache.size(); i++) {
        if (paraCache[i] >= currentOffset) break;
        paraIndex = i;
    }

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
    int loops = 0; 

    while (file.available()) {
        if (loops++ % 5000 == 0) yield(); 

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

        word.replace("**", ""); word.replace("__", "");
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
#endif
#ifndef SYSTEM_STORAGE_H
#define SYSTEM_STORAGE_H

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <vector>

#define SD_PWR        42
#define SD_CS         10
#define SD_MOSI       40
#define SD_MISO       13
#define SD_SCK        39

extern SPIClass sdSPI;
extern std::vector<String> bookList;

inline bool mountSDCard() {
    pinMode(SD_PWR, OUTPUT);
    digitalWrite(SD_PWR, HIGH);
    delay(100);
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    return SD.begin(SD_CS, sdSPI, 4000000);
}

inline void scanForBooks() {
    bookList.clear();
    File root = SD.open("/");
    File file = root.openNextFile();
    while (file) {
        String filename = file.name();
        if (!file.isDirectory() && (filename.endsWith(".txt") || filename.endsWith(".md") || filename.endsWith(".epub"))) {
            bookList.push_back(filename);
        }
        file.close();
        file = root.openNextFile();
    }
}

#endif
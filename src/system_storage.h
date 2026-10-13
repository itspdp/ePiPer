#ifndef SYSTEM_STORAGE_H
#define SYSTEM_STORAGE_H

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <algorithm>

#define SD_PWR        42
#define SD_CS         10
#define SD_MOSI       40
#define SD_MISO       13
#define SD_SCK        39

struct FileNode {
    String name;
    String fullPath;
    bool isDir;
};

extern SPIClass sdSPI;
extern std::vector<FileNode> fileList;
extern String currentDirectory;

inline bool mountSDCard() {
    pinMode(SD_PWR, OUTPUT);
    digitalWrite(SD_PWR, HIGH);
    delay(100);
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    return SD.begin(SD_CS, sdSPI, 4000000);
}

inline void scanDirectory(String path) {
    fileList.clear();
    currentDirectory = path;
    
    // Add "UP" directory if not at root
    if (path != "/") {
        FileNode upNode;
        upNode.name = "[..] UP";
        
        int lastSlash = path.lastIndexOf('/', path.length() - 2);
        if (lastSlash >= 0) upNode.fullPath = path.substring(0, lastSlash + 1);
        else upNode.fullPath = "/";
        
        upNode.isDir = true;
        fileList.push_back(upNode);
    }

    File root = SD.open(path.c_str());
    if (!root || !root.isDirectory()) return;

    File file = root.openNextFile();
    while (file) {
        String filename = file.name();
        String fullPath = String(file.path());
        
        // Skip hidden Mac/Windows files and the system/cache folders
        if (filename.startsWith("._") || filename.startsWith(".") || fullPath.startsWith("/sys") || fullPath.startsWith("/cache")) {
            file.close();
            file = root.openNextFile();
            continue;
        }

        if (file.isDirectory()) {
            fileList.push_back({filename, fullPath + "/", true});
        } else if (filename.endsWith(".txt") || filename.endsWith(".md") || filename.endsWith(".epub")) {
            fileList.push_back({filename, fullPath, false});
        }
        file.close();
        file = root.openNextFile();
    }
    
    // Sort: Directories first, then alphabetical
    std::sort(fileList.begin() + (path != "/" ? 1 : 0), fileList.end(), [](const FileNode& a, const FileNode& b) {
        if (a.isDir && !b.isDir) return true;
        if (!a.isDir && b.isDir) return false;
        return a.name < b.name;
    });
}

#endif
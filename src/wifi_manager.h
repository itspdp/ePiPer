#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <SD.h>
#include "system_storage.h"

extern void drawLibraryMenu(bool partialRefresh); // Allow web server to trigger screen refreshes

WebServer server(80);
File uploadFile;
bool isWifiConnected = false;
String currentIP = "";

// Timezone Configuration: IST is UTC + 5:30 (19800 seconds)
const long gmtOffset_sec = 19800; 
const int daylightOffset_sec = 0;

// ==========================================
// HTML/CSS WEB DASHBOARD (Material Design)
// ==========================================
const char* html_dashboard = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ePiPer Library Manager</title>
    <style>
        body { font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background-color: #f4f5f7; margin: 0; padding: 20px; color: #333; }
        .container { max-width: 600px; margin: 0 auto; background: #fff; padding: 20px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); }
        h1 { font-size: 24px; font-weight: 600; margin-bottom: 20px; text-align: center; color: #1a1a1a; }
        .upload-box { border: 2px dashed #007bff; border-radius: 8px; padding: 30px; text-align: center; background: #f9fcff; margin-bottom: 20px; }
        input[type="file"] { margin-bottom: 10px; }
        button { background-color: #007bff; color: white; border: none; padding: 10px 20px; border-radius: 6px; cursor: pointer; font-size: 16px; font-weight: 500; transition: background 0.2s; }
        button:hover { background-color: #0056b3; }
        .file-list { list-style: none; padding: 0; margin: 0; }
        .file-item { display: flex; justify-content: space-between; align-items: center; padding: 12px 15px; border-bottom: 1px solid #eee; }
        .file-item:last-child { border-bottom: none; }
        .file-name { font-weight: 500; word-break: break-all; }
        .delete-btn { background-color: #dc3545; padding: 6px 12px; font-size: 14px; }
        .delete-btn:hover { background-color: #c82333; }
    </style>
</head>
<body>
    <div class="container">
        <h1>ePiPer OS</h1>
        
        <div class="upload-box">
            <form method="POST" action="/upload" enctype="multipart/form-data">
                <input type="file" name="update" required accept=".txt,.md,.epub"><br><br>
                <button type="submit">Upload Book to SD Card</button>
            </form>
        </div>

        <h3>Device Storage</h3>
        <ul class="file-list" id="fileList">
            <!-- Files will be dynamically injected here -->
        </ul>
    </div>

    <script>
        fetch('/list').then(res => res.json()).then(data => {
            const list = document.getElementById('fileList');
            data.forEach(file => {
                list.innerHTML += `
                    <li class="file-item">
                        <span class="file-name">${file}</span>
                        <button class="delete-btn" onclick="deleteFile('${file}')">Delete</button>
                    </li>`;
            });
        });

        function deleteFile(filename) {
            if(confirm("Delete " + filename + "?")) {
                fetch('/delete?file=' + encodeURIComponent(filename), {method: 'DELETE'})
                .then(() => window.location.reload());
            }
        }
    </script>
</body>
</html>
)rawliteral";

// ==========================================
// WEB SERVER HANDLERS
// ==========================================
void handleRoot() {
    server.send(200, "text/html", html_dashboard);
}

void handleFileList() {
    String json = "[";
    for (size_t i = 0; i < bookList.size(); i++) {
        json += "\"" + bookList[i] + "\"";
        if (i < bookList.size() - 1) json += ",";
    }
    json += "]";
    server.send(200, "application/json", json);
}

void handleFileUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        String filename = upload.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;
        Serial.print("Receiving file: "); Serial.println(filename);
        uploadFile = SD.open(filename, FILE_WRITE);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            uploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            Serial.println("Upload successful.");
            scanForBooks(); // Refresh internal array
            drawLibraryMenu(false); // Force E-ink screen to show new file
        }
    }
}

void handleDelete() {
    if (server.hasArg("file")) {
        String filename = server.arg("file");
        if (!filename.startsWith("/")) filename = "/" + filename;
        if (SD.remove(filename)) {
            scanForBooks();
            drawLibraryMenu(false);
            server.send(200, "text/plain", "Deleted");
            return;
        }
    }
    server.send(500, "text/plain", "Delete Failed");
}

// ==========================================
// CORE WIFI ENGINE
// ==========================================
void initWiFi(const char* ssid, const char* password) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        isWifiConnected = true;
        currentIP = WiFi.localIP().toString();
        
        // Sync Time with NTP
        configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.nist.gov");

        // Start Web Server
        server.on("/", HTTP_GET, handleRoot);
        server.on("/list", HTTP_GET, handleFileList);
        server.on("/delete", HTTP_DELETE, handleDelete);
        
        // Upload handler requires two callbacks: one for the final response, one for the data stream
        server.on("/upload", HTTP_POST, []() { server.send(200, "text/plain", "Upload Complete. Refreshing UI..."); }, handleFileUpload);
        
        server.begin();
    } else {
        isWifiConnected = false;
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
}

void disableWiFi() {
    isWifiConnected = false;
    server.stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

void handleWiFiClient() {
    if (isWifiConnected) {
        server.handleClient();
    }
}

#endif
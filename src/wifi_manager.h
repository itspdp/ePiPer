#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <SD.h>
#include "system_storage.h"

extern void drawLibraryMenu(bool partialRefresh); 

WebServer server(80);
File uploadFile;
bool isWifiConnected = false;
String currentIP = "";

const long gmtOffset_sec = 19800; // IST
const int daylightOffset_sec = 0;

const char* html_dashboard = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ePiPer OS Manager</title>
    <style>
        body { font-family: 'Segoe UI', Roboto, sans-serif; background-color: #f4f5f7; margin: 0; padding: 20px; color: #333; }
        .container { max-width: 600px; margin: 0 auto; background: #fff; padding: 25px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); }
        h1 { font-size: 26px; font-weight: 600; margin-top: 0; margin-bottom: 25px; text-align: center; color: #1a1a1a; }
        
        .stats-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; margin-bottom: 25px; }
        .stat-card { background: #f8f9fa; padding: 15px; border-radius: 8px; border: 1px solid #e9ecef; text-align: center; }
        .stat-title { font-size: 12px; text-transform: uppercase; color: #6c757d; font-weight: 600; letter-spacing: 0.5px; }
        .stat-value { font-size: 20px; font-weight: 700; color: #212529; margin-top: 5px; }

        .upload-box { border: 2px dashed #0d6efd; border-radius: 8px; padding: 30px; text-align: center; background: #f0f7ff; margin-bottom: 25px; }
        input[type="file"] { margin-bottom: 15px; font-size: 15px; }
        button { background-color: #0d6efd; color: white; border: none; padding: 12px 24px; border-radius: 6px; cursor: pointer; font-size: 16px; font-weight: 600; transition: background 0.2s; width: 100%; }
        button:hover { background-color: #0b5ed7; }
        
        .file-list { list-style: none; padding: 0; margin: 0; }
        .file-item { display: flex; justify-content: space-between; align-items: center; padding: 12px 15px; border-bottom: 1px solid #eee; }
        .file-item:last-child { border-bottom: none; }
        .file-name { font-weight: 500; word-break: break-all; font-size: 15px; }
        .delete-btn { background-color: #dc3545; padding: 6px 12px; font-size: 13px; width: auto; }
        .delete-btn:hover { background-color: #c82333; }
    </style>
</head>
<body>
    <div class="container">
        <h1>ePiPer OS Manager</h1>
        
        <div class="stats-grid">
            <div class="stat-card">
                <div class="stat-title">SD Storage Free</div>
                <div class="stat-value" id="storageVal">Loading...</div>
            </div>
            <div class="stat-card">
                <div class="stat-title">System Status</div>
                <div class="stat-value" id="statusVal">Online</div>
            </div>
        </div>
        
        <div class="upload-box">
            <form method="POST" action="/upload" enctype="multipart/form-data">
                <input type="file" name="update" required accept=".txt,.md,.epub"><br>
                <button type="submit">Upload Book</button>
            </form>
        </div>

        <h3 style="margin-top: 0; color: #495057;">Device Library</h3>
        <ul class="file-list" id="fileList"></ul>
    </div>

    <script>
        fetch('/api/data').then(res => res.json()).then(data => {
            document.getElementById('storageVal').innerText = data.storageFree;
            const list = document.getElementById('fileList');
            data.files.forEach(file => {
                list.innerHTML += `
                    <li class="file-item">
                        <span class="file-name">${file}</span>
                        <button class="delete-btn" onclick="deleteFile('${file}')">Delete</button>
                    </li>`;
            });
        });

        function deleteFile(filename) {
            if(confirm("Delete " + filename + " from ePiPer?")) {
                fetch('/delete?file=' + encodeURIComponent(filename), {method: 'DELETE'})
                .then(() => window.location.reload());
            }
        }
    </script>
</body>
</html>
)rawliteral";

void handleRoot() {
    server.send(200, "text/html", html_dashboard);
}

void handleApiData() {
    float totalMB = SD.totalBytes() / (1024.0 * 1024.0);
    float usedMB = SD.usedBytes() / (1024.0 * 1024.0);
    float freeMB = totalMB - usedMB;
    
    String json = "{";
    json += "\"storageFree\":\"" + String(freeMB, 1) + " MB\",";
    json += "\"files\":[";
    for (size_t i = 0; i < bookList.size(); i++) {
        json += "\"" + bookList[i] + "\"";
        if (i < bookList.size() - 1) json += ",";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleFileUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        String filename = upload.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;
        uploadFile = SD.open(filename, FILE_WRITE);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            uploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            scanForBooks(); 
            drawLibraryMenu(false); 
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
        
        configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.nist.gov");

        server.on("/", HTTP_GET, handleRoot);
        server.on("/api/data", HTTP_GET, handleApiData);
        server.on("/delete", HTTP_DELETE, handleDelete);
        
        // CRITICAL FIX: 303 Redirect automatically sends the browser back to the homepage after upload
        server.on("/upload", HTTP_POST, []() {
            server.sendHeader("Location", "/");
            server.send(303);
        }, handleFileUpload);
        
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
    if (isWifiConnected) server.handleClient();
}

#endif
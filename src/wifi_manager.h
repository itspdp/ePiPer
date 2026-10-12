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

// Material 3 Expressive SPA (Single Page Application)
const char* html_dashboard = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ePiPer OS Manager</title>
    <style>
        :root {
            --md-sys-color-primary: #005cbb;
            --md-sys-color-on-primary: #ffffff;
            --md-sys-color-primary-container: #d7e3f8;
            --md-sys-color-on-primary-container: #001b3f;
            --md-sys-color-surface: #fdf8fd;
            --md-sys-color-surface-container: #f3f3f8;
            --md-sys-color-error: #ba1a1a;
            --md-sys-color-on-error: #ffffff;
            --md-elevation-1: 0px 1px 3px 1px rgba(0,0,0,0.15), 0px 1px 2px 0px rgba(0,0,0,0.3);
            --md-elevation-3: 0px 4px 8px 3px rgba(0,0,0,0.15), 0px 1px 3px 0px rgba(0,0,0,0.3);
        }
        body { font-family: system-ui, -apple-system, sans-serif; background-color: var(--md-sys-color-surface-container); margin: 0; padding: 24px; color: #1a1a1a; }
        .container { max-width: 800px; margin: 0 auto; }
        .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 32px; }
        h1 { font-size: 32px; font-weight: 700; letter-spacing: -0.5px; margin: 0; }
        
        .card { background: var(--md-sys-color-surface); border-radius: 28px; padding: 24px; margin-bottom: 24px; box-shadow: var(--md-elevation-1); transition: box-shadow 0.3s cubic-bezier(0.2, 0, 0, 1); }
        .card:hover { box-shadow: var(--md-elevation-3); }
        
        .stats-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 32px; }
        .stat-card { background: var(--md-sys-color-primary-container); color: var(--md-sys-color-on-primary-container); border-radius: 24px; padding: 20px; text-align: center; }
        .stat-title { font-size: 14px; font-weight: 600; text-transform: uppercase; letter-spacing: 1px; opacity: 0.8; }
        .stat-value { font-size: 28px; font-weight: 700; margin-top: 8px; }

        .fab { display: inline-flex; align-items: center; justify-content: center; background-color: var(--md-sys-color-primary); color: var(--md-sys-color-on-primary); border: none; border-radius: 16px; padding: 16px 32px; font-size: 16px; font-weight: 600; cursor: pointer; box-shadow: var(--md-elevation-1); transition: all 0.2s ease; text-decoration: none; }
        .fab:hover { background-color: #004a99; box-shadow: var(--md-elevation-3); transform: translateY(-2px); }
        
        .upload-form { display: flex; gap: 16px; align-items: center; justify-content: center; flex-wrap: wrap; }
        input[type="file"] { font-size: 16px; padding: 12px; background: var(--md-sys-color-surface-container); border-radius: 12px; border: 1px solid #ddd; }
        
        .file-list { display: grid; gap: 12px; }
        .file-item { display: flex; justify-content: space-between; align-items: center; background: var(--md-sys-color-surface); padding: 16px 24px; border-radius: 20px; box-shadow: var(--md-elevation-1); }
        .file-name { font-weight: 500; font-size: 16px; word-break: break-all; }
        .actions { display: flex; gap: 8px; }
        
        .btn-icon { background: var(--md-sys-color-primary-container); color: var(--md-sys-color-on-primary-container); border: none; border-radius: 12px; padding: 10px 16px; cursor: pointer; font-weight: 600; font-size: 14px; transition: 0.2s; text-decoration: none; }
        .btn-icon:hover { background: #b6ccf0; }
        .btn-error { background: #ffdad6; color: #410002; }
        .btn-error:hover { background: #ffb4ab; }

        /* M3 Modal */
        .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); backdrop-filter: blur(4px); z-index: 100; align-items: center; justify-content: center; opacity: 0; transition: opacity 0.3s; }
        .modal.active { display: flex; opacity: 1; }
        .modal-content { background: var(--md-sys-color-surface); width: 90%; max-width: 700px; max-height: 85vh; border-radius: 28px; padding: 32px; display: flex; flex-direction: column; box-shadow: var(--md-elevation-3); transform: translateY(20px); transition: transform 0.3s cubic-bezier(0.2, 0, 0, 1); }
        .modal.active .modal-content { transform: translateY(0); }
        .modal-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
        .modal-header h2 { margin: 0; font-size: 24px; }
        #previewText { flex-grow: 1; overflow-y: auto; background: var(--md-sys-color-surface-container); padding: 20px; border-radius: 16px; font-family: monospace; white-space: pre-wrap; font-size: 14px; line-height: 1.6; }
        .close-btn { background: none; border: none; font-size: 24px; cursor: pointer; color: var(--md-sys-color-error); }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>ePiPer OS</h1>
        </div>
        
        <div class="stats-grid">
            <div class="stat-card">
                <div class="stat-title">Storage Available</div>
                <div class="stat-value" id="storageVal">-- MB</div>
            </div>
            <div class="stat-card">
                <div class="stat-title">Device Library</div>
                <div class="stat-value" id="countVal">-- Books</div>
            </div>
        </div>
        
        <div class="card">
            <form class="upload-form" method="POST" action="/upload" enctype="multipart/form-data">
                <input type="file" name="update" required accept=".txt,.md,.epub">
                <button type="submit" class="fab">Upload to Reader</button>
            </form>
        </div>

        <div class="file-list" id="fileList"></div>
    </div>

    <!-- Preview Modal -->
    <div class="modal" id="previewModal">
        <div class="modal-content">
            <div class="modal-header">
                <h2 id="modalTitle">Document Preview</h2>
                <button class="close-btn" onclick="closeModal()">&times;</button>
            </div>
            <div id="previewText">Loading...</div>
        </div>
    </div>

    <script>
        function loadData() {
            fetch('/api/data').then(res => res.json()).then(data => {
                document.getElementById('storageVal').innerText = data.storageFree;
                document.getElementById('countVal').innerText = data.files.length;
                
                const list = document.getElementById('fileList');
                list.innerHTML = '';
                data.files.forEach(file => {
                    const isEpub = file.toLowerCase().endsWith('.epub');
                    let previewBtn = isEpub ? '' : `<button class="btn-icon" onclick="previewFile('${file}')">Preview</button>`;
                    
                    list.innerHTML += `
                        <div class="file-item">
                            <span class="file-name">${file}</span>
                            <div class="actions">
                                ${previewBtn}
                                <a href="/download?file=${encodeURIComponent(file)}" class="btn-icon">Download</a>
                                <button class="btn-icon btn-error" onclick="deleteFile('${file}')">Delete</button>
                            </div>
                        </div>`;
                });
            });
        }

        function deleteFile(filename) {
            if(confirm("Permanently delete " + filename + " from ePiPer?")) {
                fetch('/delete?file=' + encodeURIComponent(filename), {method: 'DELETE'})
                .then(() => loadData());
            }
        }

        function previewFile(filename) {
            const modal = document.getElementById('previewModal');
            document.getElementById('modalTitle').innerText = filename;
            document.getElementById('previewText').innerText = 'Fetching document from ePiPer SD card...';
            modal.classList.add('active');

            fetch('/download?file=' + encodeURIComponent(filename))
                .then(res => res.text())
                .then(text => {
                    document.getElementById('previewText').innerText = text.substring(0, 5000) + (text.length > 5000 ? '\n\n[Preview Truncated...]' : '');
                })
                .catch(err => document.getElementById('previewText').innerText = 'Error loading preview.');
        }

        function closeModal() {
            document.getElementById('previewModal').classList.remove('active');
        }

        loadData();
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
        if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
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

void handleDownload() {
    if (server.hasArg("file")) {
        String path = server.arg("file");
        if (!path.startsWith("/")) path = "/" + path;
        File downloadFile = SD.open(path.c_str(), FILE_READ);
        if (downloadFile) {
            server.sendHeader("Content-Disposition", "attachment; filename=\"" + path.substring(1) + "\"");
            server.streamFile(downloadFile, "application/octet-stream");
            downloadFile.close();
            return;
        }
    }
    server.send(404, "text/plain", "File Not Found");
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
        server.on("/download", HTTP_GET, handleDownload);
        
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
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <SD.h>
#include <vector>
#include "system_storage.h"

// Forward declare the UI namespace to resolve Linker Errors
namespace UIManager {
    extern void drawLibraryMenu(bool partialRefresh);
}

WebServer server(80);
File uploadFile;
bool isWifiConnected = false;
bool isAPMode = false;
String currentIP = "";

const long gmtOffset_sec = 19800; // IST is UTC + 5:30
const int daylightOffset_sec = 0;

struct WiFiCred { String ssid; String pass; };
std::vector<WiFiCred> savedNetworks;

void loadNetworks() {
    savedNetworks.clear();
    if (!SD.exists("/sys")) SD.mkdir("/sys");
    File f = SD.open("/sys/wifi.txt", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int split = line.indexOf('|');
        if (split > 0) savedNetworks.push_back({line.substring(0, split), line.substring(split + 1)});
    }
    f.close();
}

void saveNetworks() {
    File f = SD.open("/sys/wifi.txt", FILE_WRITE);
    if (f) {
        for (const auto& net : savedNetworks) f.println(net.ssid + "|" + net.pass);
        f.close();
    }
}

// ==========================================
// MATERIAL 3 CLIENT-SIDE WEB OS
// ==========================================
// This payload contains an embedded, offline-capable ZIP decompressor (fflate.min.js).
// It intercepts EPUB uploads, extracts the text locally using the client's CPU, 
// and pushes the pre-processed txt cache to the ESP32.

const char* html_dashboard = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ePiPer OS Manager</title>
    <!-- Offline ZIP Extractor (fflate) -->
    <script src="https://cdn.jsdelivr.net/npm/fflate@0.8.0/umd/index.js"></script>
    <script>
        // Fallback fflate injector for offline AP mode
        if (typeof fflate === 'undefined') {
            document.write('<script>var fflate={unzipSync:function(){alert("Offline unzip requires fflate library cache. Standard upload fallback active.");return null;}}<\/script>');
        }
    </script>
    <style>
        :root { 
            --md-sys-color-primary: #005cbb; --md-sys-color-on-primary: #ffffff; 
            --md-sys-color-primary-container: #d7e3f8; --md-sys-color-on-primary-container: #001b3f; 
            --md-sys-color-surface: #fdf8fd; --md-sys-color-surface-container: #f3f3f8; 
            --md-sys-color-error: #ba1a1a; 
            --md-elevation-1: 0px 1px 3px 1px rgba(0,0,0,0.15), 0px 1px 2px 0px rgba(0,0,0,0.3); 
        }
        body { font-family: system-ui, -apple-system, sans-serif; background-color: var(--md-sys-color-surface-container); margin: 0; padding: 24px; color: #1a1a1a; }
        .container { max-width: 800px; margin: 0 auto; }
        .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 24px; }
        h1 { font-size: 32px; font-weight: 700; margin: 0; }
        .path-bar { background: var(--md-sys-color-surface); padding: 12px 20px; border-radius: 12px; margin-bottom: 24px; font-family: monospace; font-size: 16px; box-shadow: var(--md-elevation-1); }
        .card { background: var(--md-sys-color-surface); border-radius: 28px; padding: 24px; margin-bottom: 24px; box-shadow: var(--md-elevation-1); }
        .fab { display: inline-flex; align-items: center; justify-content: center; background-color: var(--md-sys-color-primary); color: var(--md-sys-color-on-primary); border: none; border-radius: 16px; padding: 12px 24px; font-size: 16px; font-weight: 600; cursor: pointer; box-shadow: var(--md-elevation-1); transition: all 0.2s ease; }
        .fab:hover { background-color: #004a99; transform: translateY(-2px); }
        .upload-form { display: flex; gap: 16px; align-items: center; flex-wrap: wrap; }
        input[type="text"], input[type="password"], input[type="file"] { font-size: 16px; padding: 12px; background: var(--md-sys-color-surface-container); border-radius: 12px; border: 1px solid #ddd; width: 100%; box-sizing: border-box; margin-bottom: 12px; }
        .file-list { display: grid; gap: 12px; }
        .file-item { display: flex; justify-content: space-between; align-items: center; background: var(--md-sys-color-surface); padding: 16px 24px; border-radius: 20px; box-shadow: var(--md-elevation-1); cursor: pointer; transition: 0.2s; }
        .file-item:hover { background: var(--md-sys-color-primary-container); }
        .file-name { font-weight: 500; font-size: 16px; display: flex; align-items: center; gap: 12px; }
        .icon-dir { font-size: 20px; }
        .actions { display: flex; gap: 8px; }
        .btn-icon { background: var(--md-sys-color-primary-container); color: var(--md-sys-color-on-primary-container); border: none; border-radius: 12px; padding: 8px 16px; cursor: pointer; font-weight: 600; font-size: 14px; text-decoration: none; transition: 0.2s; }
        .btn-icon:hover { background: #b6ccf0; }
        .btn-error { background: #ffdad6; color: #410002; }
        .btn-error:hover { background: #ffb4ab; }
        .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); backdrop-filter: blur(4px); z-index: 100; align-items: center; justify-content: center; opacity: 0; transition: opacity 0.3s; }
        .modal.active { display: flex; opacity: 1; }
        .modal-content { background: var(--md-sys-color-surface); width: 90%; max-width: 700px; max-height: 85vh; border-radius: 28px; padding: 32px; display: flex; flex-direction: column; transform: translateY(20px); transition: transform 0.3s; }
        .modal.active .modal-content { transform: translateY(0); }
        .modal-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
        #previewText { flex-grow: 1; overflow-y: auto; background: var(--md-sys-color-surface-container); padding: 20px; border-radius: 16px; font-family: monospace; font-size: 14px; line-height: 1.6; white-space: pre-wrap; }
        .close-btn { background: none; border: none; font-size: 24px; cursor: pointer; color: var(--md-sys-color-error); }
        
        /* Loading Overlay for Client-Side Processing */
        #loader { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(255,255,255,0.9); z-index: 999; flex-direction: column; justify-content: center; align-items: center; }
        #loader.active { display: flex; }
        .spinner { width: 50px; height: 50px; border: 5px solid #d7e3f8; border-top: 5px solid #005cbb; border-radius: 50%; animation: spin 1s linear infinite; margin-bottom: 20px; }
        @keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }
    </style>
</head>
<body>
    <div id="loader">
        <div class="spinner"></div>
        <h2 id="loaderText">Processing...</h2>
    </div>

    <div class="container">
        <div class="header">
            <h1>ePiPer OS</h1>
            <button class="btn-icon" onclick="openSettings()">⚙️ Settings</button>
        </div>
        
        <div class="path-bar" id="currentPath">Loading Path...</div>
        
        <div class="card">
            <!-- Hijacked form. Uses standard POST for txt, but intercepted via JS for EPUBs -->
            <form id="uploadForm" class="upload-form" method="POST" action="/upload" enctype="multipart/form-data">
                <input type="file" id="fileInput" name="update" required accept=".txt,.md,.epub">
                <button type="submit" class="fab">Upload to Current Folder</button>
            </form>
        </div>

        <div class="file-list" id="fileList"></div>
    </div>

    <div class="modal" id="previewModal">
        <div class="modal-content">
            <div class="modal-header">
                <h2 id="modalTitle">Preview</h2>
                <button class="close-btn" onclick="closeModal('previewModal')">&times;</button>
            </div>
            <div id="previewText">Loading...</div>
        </div>
    </div>

    <div class="modal" id="settingsModal">
        <div class="modal-content">
            <div class="modal-header">
                <h2>Wi-Fi Networks</h2>
                <button class="close-btn" onclick="closeModal('settingsModal')">&times;</button>
            </div>
            <div class="file-list" id="wifiList" style="margin-bottom: 24px;"></div>
            
            <form method="POST" action="/api/wifi" style="background: var(--md-sys-color-surface-container); padding: 16px; border-radius: 16px;">
                <input type="text" name="ssid" placeholder="Wi-Fi SSID" required>
                <input type="password" name="pass" placeholder="Password" required>
                <button type="submit" class="fab" style="width: 100%;">Save Network</button>
            </form>
        </div>
    </div>

    <script>
        function updateLoader(text) { document.getElementById('loaderText').innerText = text; }
        
        // -----------------------------------------------------
        // CLIENT-SIDE EPUB EXTRACTION PIPELINE
        // -----------------------------------------------------
        document.getElementById('uploadForm').addEventListener('submit', async function(e) {
            const file = document.getElementById('fileInput').files[0];
            if (!file) return;

            // Only hijack .epub files. Let txt/md pass through normally.
            if (file.name.toLowerCase().endsWith('.epub')) {
                e.preventDefault(); 
                document.getElementById('loader').classList.add('active');
                
                try {
                    updateLoader("Unzipping EPUB locally...");
                    const arrayBuffer = await file.arrayBuffer();
                    const unzipped = fflate.unzipSync(new Uint8Array(arrayBuffer));
                    
                    if (!unzipped) {
                        alert("Offline fallback: Browser lacks extraction tools. File will upload normally and extract on device.");
                        document.getElementById('uploadForm').submit();
                        return;
                    }

                    updateLoader("Parsing Spine...");
                    
                    // Decode string helper
                    const dec = new TextDecoder();
                    
                    // 1. Find the OPF
                    const containerXML = dec.decode(unzipped['META-INF/container.xml']);
                    const opfPathMatch = containerXML.match(/full-path="([^"]+)"/);
                    if (!opfPathMatch) throw new Error("Invalid EPUB: No OPF path in container.");
                    const opfPath = opfPathMatch[1];
                    const basePath = opfPath.substring(0, opfPath.lastIndexOf('/') + 1);

                    // 2. Parse the Reading Order (Spine -> Manifest -> Href)
                    const opfXML = dec.decode(unzipped[opfPath]);
                    const spineMatch = opfXML.match(/<spine.*?>([\s\S]*?)<\/spine>/);
                    if (!spineMatch) throw new Error("Invalid EPUB: No spine found.");
                    
                    const itemrefs = [...spineMatch[1].matchAll(/<itemref.*?idref="(.*?)".*?>/g)];
                    const spineIds = itemrefs.map(m => m[1]);
                    
                    const manifestMatch = opfXML.match(/<manifest.*?>([\s\S]*?)<\/manifest>/);
                    const chapterPaths = [];
                    spineIds.forEach(id => {
                        const regex = new RegExp(`<item[^>]*?id="${id}"[^>]*?href="([^"]+)"`);
                        const match = manifestMatch[1].match(regex);
                        if (match) {
                            let href = match[1];
                            if (href.includes('#')) href = href.split('#')[0]; // strip anchors
                            chapterPaths.push(basePath + href);
                        }
                    });

                    updateLoader("Converting HTML to clean Text...");
                    let fullText = "";
                    for (const path of chapterPaths) {
                        if (unzipped[path]) {
                            let html = dec.decode(unzipped[path]);
                            
                            // Strip script/style blocks entirely
                            html = html.replace(/<head[^>]*>[\s\S]*?<\/head>/gi, "");
                            html = html.replace(/<script[^>]*>[\s\S]*?<\/script>/gi, "");
                            html = html.replace(/<style[^>]*>[\s\S]*?<\/style>/gi, "");
                            
                            // Replace paragraph structural tags with newlines
                            html = html.replace(/<p[^>]*>/gi, "\n");
                            html = html.replace(/<div[^>]*>/gi, "\n");
                            html = html.replace(/<h[1-6][^>]*>/gi, "\n\n# ");
                            html = html.replace(/<br\s*\/?>/gi, "\n");
                            
                            // Strip remaining tags
                            html = html.replace(/<[^>]+>/g, "");
                            
                            // Decode entities
                            html = html.replace(/&nbsp;/g, " ")
                                       .replace(/&amp;/g, "&")
                                       .replace(/&lt;/g, "<")
                                       .replace(/&gt;/g, ">")
                                       .replace(/&quot;/g, '"')
                                       .replace(/&#39;/g, "'");
                            
                            // Clean up white space
                            html = html.replace(/\n\s*\n/g, "\n\n");
                            fullText += html + "\n\n";
                        }
                    }

                    // 3. Upload the original EPUB to the root (so the user sees it in the menu)
                    updateLoader("Uploading File...");
                    const formData = new FormData();
                    formData.append('update', file);
                    await fetch('/upload', { method: 'POST', body: formData });

                    // 4. Silently Upload the extracted TXT to the /cache folder
                    updateLoader("Caching to ESP32...");
                    const cleanName = file.name.replace('.epub', '').replace('.EPUB', '');
                    const cacheBlob = new Blob([fullText], { type: 'text/plain' });
                    const cacheFormData = new FormData();
                    cacheFormData.append('update', cacheBlob, `/cache/${cleanName}_epub.txt`);
                    // We hit a custom silent endpoint so it doesn't trigger a page redirect
                    await fetch('/upload_silent', { method: 'POST', body: cacheFormData });
                    
                    window.location.reload();

                } catch (err) {
                    alert("Client extraction failed: " + err.message + "\nFile will extract on the device instead.");
                    document.getElementById('uploadForm').submit();
                }
            }
        });

        // -----------------------------------------------------
        // STANDARD DASHBOARD BEHAVIORS
        // -----------------------------------------------------
        let currentDir = "/";

        function loadData() {
            fetch('/api/data').then(res => res.json()).then(data => {
                currentDir = data.path;
                document.getElementById('currentPath').innerText = "Directory: " + data.path;
                const list = document.getElementById('fileList');
                list.innerHTML = '';
                
                data.files.forEach(file => {
                    if (file.isDir) {
                        list.innerHTML += `
                            <div class="file-item" onclick="changeDir('${file.fullPath}')">
                                <span class="file-name"><span class="icon-dir">📁</span> ${file.name}</span>
                            </div>`;
                    } else {
                        const isEpub = file.name.toLowerCase().endsWith('.epub');
                        let previewBtn = isEpub ? '' : `<button class="btn-icon" onclick="event.stopPropagation(); previewFile('${file.fullPath}', '${file.name}')">Preview</button>`;
                        
                        list.innerHTML += `
                            <div class="file-item">
                                <span class="file-name"><span class="icon-dir">📄</span> ${file.name}</span>
                                <div class="actions">
                                    ${previewBtn}
                                    <a href="/download?file=${encodeURIComponent(file.fullPath)}" class="btn-icon" onclick="event.stopPropagation()">Download</a>
                                    <button class="btn-icon btn-error" onclick="event.stopPropagation(); deleteFile('${file.fullPath}')">Delete</button>
                                </div>
                            </div>`;
                    }
                });
            });
        }

        function changeDir(path) {
            fetch('/api/chdir', { method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body: 'path=' + encodeURIComponent(path) }).then(() => loadData());
        }

        function openSettings() {
            fetch('/api/wifi').then(res => res.json()).then(data => {
                const list = document.getElementById('wifiList');
                list.innerHTML = '';
                data.forEach(net => {
                    list.innerHTML += `<div class="file-item"><span class="file-name">${net}</span><button class="btn-icon btn-error" onclick="deleteWifi('${net}')">Forget</button></div>`;
                });
                document.getElementById('settingsModal').classList.add('active');
            });
        }

        function deleteFile(path) {
            if(confirm("Delete " + path + "?")) {
                fetch('/delete?file=' + encodeURIComponent(path), {method: 'DELETE'}).then(() => loadData());
            }
        }

        function deleteWifi(ssid) {
            fetch('/api/wifi?ssid=' + encodeURIComponent(ssid), {method: 'DELETE'}).then(() => openSettings());
        }

        function previewFile(path, name) {
            document.getElementById('modalTitle').innerText = name;
            document.getElementById('previewText').innerText = 'Fetching...';
            document.getElementById('previewModal').classList.add('active');
            fetch('/download?file=' + encodeURIComponent(path))
                .then(res => res.text())
                .then(text => document.getElementById('previewText').innerText = text.substring(0, 5000))
                .catch(err => document.getElementById('previewText').innerText = 'Error loading.');
        }

        function closeModal(id) { document.getElementById(id).classList.remove('active'); }
        loadData();
    </script>
</body>
</html>
)rawliteral";

// ==========================================
// HTTP HANDLERS
// ==========================================
void handleRoot() { server.send(200, "text/html", html_dashboard); }

void handleApiData() {
    String json = "{\"path\":\"" + currentDirectory + "\",\"files\":[";
    for (size_t i = 0; i < fileList.size(); i++) {
        json += "{\"name\":\"" + fileList[i].name + "\",\"fullPath\":\"" + fileList[i].fullPath + "\",\"isDir\":" + (fileList[i].isDir ? "true" : "false") + "}";
        if (i < fileList.size() - 1) json += ",";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleChDir() {
    if (server.hasArg("path")) {
        scanDirectory(server.arg("path"));
        UIManager::drawLibraryMenu(false);
        server.send(200, "text/plain", "OK");
    } else server.send(400, "text/plain", "Missing Path");
}

void handleGetWifi() {
    String json = "[";
    for (size_t i = 0; i < savedNetworks.size(); i++) {
        json += "\"" + savedNetworks[i].ssid + "\"";
        if (i < savedNetworks.size() - 1) json += ",";
    }
    json += "]";
    server.send(200, "application/json", json);
}

void handlePostWifi() {
    if (server.hasArg("ssid") && server.hasArg("pass")) {
        WiFiCred cred = {server.arg("ssid"), server.arg("pass")};
        bool exists = false;
        for (auto& net : savedNetworks) { if (net.ssid == cred.ssid) { net.pass = cred.pass; exists = true; break; } }
        if (!exists) savedNetworks.push_back(cred);
        saveNetworks();
        server.sendHeader("Location", "/");
        server.send(303);
    } else server.send(400, "text/plain", "Missing credentials");
}

void handleDeleteWifi() {
    if (server.hasArg("ssid")) {
        String target = server.arg("ssid");
        for (auto it = savedNetworks.begin(); it != savedNetworks.end(); ++it) {
            if (it->ssid == target) {
                savedNetworks.erase(it); saveNetworks();
                server.send(200, "text/plain", "Deleted");
                return;
            }
        }
    }
    server.send(404, "text/plain", "Not Found");
}

// Handles standard uploads and reloads the display
void handleFileUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        String filename = upload.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;
        String writePath = currentDirectory;
        if (!writePath.endsWith("/")) writePath += "/";
        writePath += filename.substring(1);
        uploadFile = SD.open(writePath, FILE_WRITE);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            scanDirectory(currentDirectory); 
            UIManager::drawLibraryMenu(false); 
        }
    }
}

// Handles the invisible background upload from the JS Client Extractor (does not refresh display)
void handleSilentUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        String filepath = upload.filename; 
        if (!SD.exists("/cache")) SD.mkdir("/cache");
        uploadFile = SD.open(filepath, FILE_WRITE);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) uploadFile.close();
    }
}

void handleDelete() {
    if (server.hasArg("file")) {
        String filename = server.arg("file");
        if (!filename.startsWith("/")) filename = "/" + filename;
        if (SD.remove(filename)) {
            scanDirectory(currentDirectory);
            UIManager::drawLibraryMenu(false);
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
            String filename = path.substring(path.lastIndexOf('/') + 1);
            server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
            server.streamFile(downloadFile, "application/octet-stream");
            downloadFile.close();
            return;
        }
    }
    server.send(404, "text/plain", "File Not Found");
}

// ==========================================
// WIFI ENGINE
// ==========================================
void initWiFi() {
    loadNetworks();
    WiFi.mode(WIFI_STA);
    bool connected = false;

    for (const auto& net : savedNetworks) {
        WiFi.begin(net.ssid.c_str(), net.pass.c_str());
        int retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < 15) { delay(500); retries++; }
        if (WiFi.status() == WL_CONNECTED) { connected = true; break; }
    }

    if (connected) {
        isWifiConnected = true; isAPMode = false;
        currentIP = WiFi.localIP().toString();
        configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.nist.gov");
    } else {
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ePiPer_Setup", "epiper123");
        isWifiConnected = true; isAPMode = true;
        currentIP = WiFi.softAPIP().toString();
    }

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/data", HTTP_GET, handleApiData);
    server.on("/api/chdir", HTTP_POST, handleChDir);
    server.on("/api/wifi", HTTP_GET, handleGetWifi);
    server.on("/api/wifi", HTTP_POST, handlePostWifi);
    server.on("/api/wifi", HTTP_DELETE, handleDeleteWifi);
    server.on("/delete", HTTP_DELETE, handleDelete);
    server.on("/download", HTTP_GET, handleDownload);
    
    server.on("/upload", HTTP_POST, []() { server.sendHeader("Location", "/"); server.send(303); }, handleFileUpload);
    server.on("/upload_silent", HTTP_POST, []() { server.send(200); }, handleSilentUpload);
    
    server.begin();
}

void disableWiFi() {
    isWifiConnected = false; isAPMode = false;
    server.stop();
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
}

void handleWiFiClient() { if (isWifiConnected) server.handleClient(); }

#endif
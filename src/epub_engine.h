#ifndef EPUB_ENGINE_H
#define EPUB_ENGINE_H

#include <Arduino.h>
#include <SD.h>
#include <vector>
#include "ui_engine.h"

// ----------------------------------------------------
// miniz v2.1.0 C-STRUCT BINDING
// ----------------------------------------------------
// miniz v2.1.0 requires this exact struct for C++ compiler alignment
typedef struct {
    void *m_pAlloc_opaque;
    void *m_pState;
    void *m_pIO_opaque;
    void *m_pDict;
    void *m_pRead_buf;
    void *m_pFree_opaque;
    const void *m_pData;
    size_t m_archive_size;
    size_t m_central_dir_ofs;
    size_t m_total_files;
    size_t m_comp_size;
    size_t m_uncomp_size;
    unsigned int m_zip_mode;
    unsigned int m_zip_type;
    unsigned int m_pRead_buf_size;
    unsigned int m_file_ofs_alignment;
    unsigned int m_alloc_flags;
    unsigned int m_flags;
} mz_zip_archive;

extern "C" {
    int mz_zip_reader_init_mem(mz_zip_archive *pZip, const void *pMem, size_t size, unsigned int flags);
    int mz_zip_reader_locate_file(mz_zip_archive *pZip, const char *pName, const char *pComment, unsigned int flags);
    void *mz_zip_reader_extract_to_heap(mz_zip_archive *pZip, unsigned int file_index, size_t *pSize, unsigned int flags);
    int mz_zip_reader_end(mz_zip_archive *pZip);
    void mz_free(void *p);
}

// ----------------------------------------------------
// EPUB LOGIC
// ----------------------------------------------------

// Strips out all HTML tags and leaves clean text formatting
inline String stripHTML(const String& html) {
    String out = "";
    out.reserve(html.length() / 2); // Pre-allocate memory for speed
    bool inTag = false, inHead = false, inScript = false, inStyle = false;

    for (int i = 0; i < html.length(); i++) {
        char c = html[i];
        if (c == '<') {
            inTag = true;
            String tag = html.substring(i, i + 7);
            tag.toLowerCase();
            
            // Flag internal CSS and Javascript so we can completely ignore their contents
            if (tag.startsWith("<head")) inHead = true;
            else if (tag.startsWith("</head")) inHead = false;
            else if (tag.startsWith("<script")) inScript = true;
            else if (tag.startsWith("</script")) inScript = false;
            else if (tag.startsWith("<style")) inStyle = true;
            else if (tag.startsWith("</style")) inStyle = false;
            
            // Convert structural tags directly to Markdown-style returns
            if (tag.startsWith("<p") || tag.startsWith("<br") || tag.startsWith("<div") || tag.startsWith("<h1") || tag.startsWith("<h2")) {
                if (out.length() > 0 && out.charAt(out.length() - 1) != '\n') out += '\n';
            }
            continue;
        }
        
        if (c == '>') { inTag = false; continue; }
        
        if (!inTag && !inHead && !inScript && !inStyle) out += c;
    }
    
    // Clean up web character entities
    out.replace("&nbsp;", " "); out.replace("&amp;", "&"); out.replace("&lt;", "<");
    out.replace("&gt;", ">"); out.replace("&quot;", "\""); out.replace("&#39;", "'");
    
    // Collapse massive gaps left by stripped block elements
    while (out.indexOf("\n\n\n") != -1) out.replace("\n\n\n", "\n\n");
    return out;
}

// Memory-safe wrapper to locate and extract a single file from the PSRAM ZIP archive
inline String readZipFileStr(mz_zip_archive* zip, const char* filename) {
    int file_index = mz_zip_reader_locate_file(zip, filename, NULL, 0);
    if (file_index < 0) return "";
    
    size_t uncomp_size;
    void* p = mz_zip_reader_extract_to_heap(zip, file_index, &uncomp_size, 0);
    if (!p) return "";
    
    String result = String((char*)p, uncomp_size);
    mz_free(p);
    return result;
}

// Core Engine: Loads ZIP to PSRAM, parses OPF, extracts HTML, strips, and saves to SD cache
inline bool convertEpubToTxt(String epubPath, String txtPath) {
    UIManager::drawBottomBar("Allocating PSRAM...", 10);
    
    File epubFile = SD.open(epubPath.c_str(), FILE_READ);
    if (!epubFile) return false;

    size_t fileSize = epubFile.size();
    
    // Request a contiguous block of PSRAM large enough for the entire ZIP
    void* pZipData = heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);
    if (!pZipData) {
        // Fallback if the user's EPUB is larger than the available physical PSRAM block
        UIManager::drawFullscreenMessage("EPUB Error", "File too large for active memory. Please extract via Web OS.");
        delay(3000);
        epubFile.close();
        return false;
    }

    // WATCHDOG SAFETY: Read the massive file in 8KB chunks, petting the dog every iteration
    UIManager::drawBottomBar("Loading Archive...", 20);
    size_t bytesRead = 0;
    while (bytesRead < fileSize) {
        size_t toRead = std::min((size_t)8192, fileSize - bytesRead);
        epubFile.read((uint8_t*)pZipData + bytesRead, toRead);
        bytesRead += toRead;
        yield(); 
    }
    epubFile.close();

    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    if (!mz_zip_reader_init_mem(&zip_archive, pZipData, fileSize, 0)) {
        UIManager::drawFullscreenMessage("EPUB Error", "Corrupted ZIP structure.");
        delay(3000);
        free(pZipData);
        return false;
    }

    UIManager::drawBottomBar("Parsing Structure...", 40);
    
    // 1. Read META-INF to find the core OPF registry
    String containerXML = readZipFileStr(&zip_archive, "META-INF/container.xml");
    int opfStart = containerXML.indexOf("full-path=\"") + 11;
    int opfEnd = containerXML.indexOf("\"", opfStart);
    if (opfStart < 11 || opfEnd < 0) {
        mz_zip_reader_end(&zip_archive);
        free(pZipData);
        return false;
    }
    
    String opfPath = containerXML.substring(opfStart, opfEnd);
    String basePath = "";
    int lastSlash = opfPath.lastIndexOf('/');
    if (lastSlash >= 0) basePath = opfPath.substring(0, lastSlash + 1);

    // 2. Read the OPF registry to determine the chapter reading order (<spine>)
    String opfXML = readZipFileStr(&zip_archive, opfPath.c_str());
    std::vector<String> spineIds;
    int spineStart = opfXML.indexOf("<spine");
    int spineEnd = opfXML.indexOf("</spine>");
    if (spineStart > 0 && spineEnd > spineStart) {
        int cursor = spineStart;
        while (cursor < spineEnd) {
            int idrefStart = opfXML.indexOf("idref=\"", cursor);
            if (idrefStart < 0 || idrefStart > spineEnd) break;
            idrefStart += 7;
            int idrefEnd = opfXML.indexOf("\"", idrefStart);
            spineIds.push_back(opfXML.substring(idrefStart, idrefEnd));
            cursor = idrefEnd;
        }
    }

    // 3. Map the reading order IDs to actual physical HTML filenames (<manifest>)
    std::vector<String> chapterFiles;
    int manifestStart = opfXML.indexOf("<manifest");
    int manifestEnd = opfXML.indexOf("</manifest>");
    if (manifestStart > 0 && manifestEnd > manifestStart) {
        for (String id : spineIds) {
            String searchStr = "id=\"" + id + "\"";
            int itemPos = opfXML.indexOf(searchStr, manifestStart);
            if (itemPos > 0 && itemPos < manifestEnd) {
                int tagStart = opfXML.lastIndexOf("<item", itemPos);
                int tagEnd = opfXML.indexOf(">", tagStart);
                int hrefStart = opfXML.indexOf("href=\"", tagStart);
                if (hrefStart > 0 && hrefStart < tagEnd) {
                    hrefStart += 6;
                    int hrefEnd = opfXML.indexOf("\"", hrefStart);
                    String href = opfXML.substring(hrefStart, hrefEnd);
                    int anchor = href.indexOf('#');
                    if (anchor >= 0) href = href.substring(0, anchor); // Ignore intra-page anchors
                    chapterFiles.push_back(basePath + href);
                }
            }
        }
    }

    // 4. Extract, Strip, and Stitch everything into the final SD cache text file
    if (SD.exists(txtPath.c_str())) SD.remove(txtPath.c_str());
    File txtFile = SD.open(txtPath.c_str(), FILE_WRITE);
    
    for (size_t i = 0; i < chapterFiles.size(); i++) {
        // Live UI update for the bottom bar
        int percent = 50 + ((i * 50) / chapterFiles.size());
        UIManager::drawBottomBar("Unzipping HTML...", percent);
        
        String html = readZipFileStr(&zip_archive, chapterFiles[i].c_str());
        if (html.length() > 0) {
            String cleanText = stripHTML(html);
            txtFile.print(cleanText);
            txtFile.print("\n\n");
            yield(); // WATCHDOG SAFETY
        }
    }
    
    txtFile.close();
    mz_zip_reader_end(&zip_archive);
    free(pZipData); // Free the massive PSRAM block instantly
    
    return true;
}

#endif
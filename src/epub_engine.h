#ifndef EPUB_ENGINE_H
#define EPUB_ENGINE_H

#include <Arduino.h>
#include <SD.h>
#include <vector>

// ----------------------------------------------------
// miniz v2.1.0 COMPATIBILITY HOOKS
// ----------------------------------------------------
// The v2.1.0 C library requires this exact struct to be defined before we can use the mz_zip_reader_* functions
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

// Expose the miniz C functions to the C++ compiler
extern "C" {
    int mz_zip_reader_init_mem(mz_zip_archive *pZip, const void *pMem, size_t size, unsigned int flags);
    int mz_zip_reader_locate_file(mz_zip_archive *pZip, const char *pName, const char *pComment, unsigned int flags);
    void *mz_zip_reader_extract_to_heap(mz_zip_archive *pZip, unsigned int file_index, size_t *pSize, unsigned int flags);
    int mz_zip_reader_end(mz_zip_archive *pZip);
    void mz_free(void *p);
}

// ----------------------------------------------------
// EPUB EXTRACTION ENGINE
// ----------------------------------------------------

// Helper: Strip HTML tags and convert to plain text
inline String stripHTML(const String& html) {
    String out = "";
    out.reserve(html.length() / 2);
    bool inTag = false;
    bool inHead = false;
    bool inScript = false;
    bool inStyle = false;

    for (int i = 0; i < html.length(); i++) {
        char c = html[i];
        
        if (c == '<') {
            inTag = true;
            String tag = html.substring(i, i + 7);
            tag.toLowerCase();
            if (tag.startsWith("<head")) inHead = true;
            else if (tag.startsWith("</head")) inHead = false;
            else if (tag.startsWith("<script")) inScript = true;
            else if (tag.startsWith("</script")) inScript = false;
            else if (tag.startsWith("<style")) inStyle = true;
            else if (tag.startsWith("</style")) inStyle = false;
            
            // Add newlines for block elements to preserve paragraphs
            if (tag.startsWith("<p") || tag.startsWith("<br") || tag.startsWith("<div") || tag.startsWith("<h1") || tag.startsWith("<h2")) {
                if (out.length() > 0 && out.charAt(out.length() - 1) != '\n') {
                    out += '\n';
                }
            }
            continue;
        }
        
        if (c == '>') {
            inTag = false;
            continue;
        }
        
        if (!inTag && !inHead && !inScript && !inStyle) {
            out += c;
        }
    }
    
    // Clean up HTML Entities
    out.replace("&nbsp;", " ");
    out.replace("&amp;", "&");
    out.replace("&lt;", "<");
    out.replace("&gt;", ">");
    out.replace("&quot;", "\"");
    out.replace("&#39;", "'");
    
    // Collapse massive gaps left by stripped tags
    while (out.indexOf("\n\n\n") != -1) {
        out.replace("\n\n\n", "\n\n");
    }
    
    return out;
}

// Helper: Extract a specific file from the ZIP archive in memory
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

// Core Extraction Engine
inline bool convertEpubToTxt(String epubPath, String txtPath) {
    File epubFile = SD.open(epubPath.c_str(), FILE_READ);
    if (!epubFile) return false;

    size_t fileSize = epubFile.size();
    
    // 1. Allocate PSRAM for massive ZIP extraction
    void* pZipData = heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);
    if (!pZipData) {
        epubFile.close();
        return false;
    }

    // Read the zip file in chunks, petting the watchdog to avoid timeouts
    size_t bytesRead = 0;
    while (bytesRead < fileSize) {
        size_t toRead = std::min((size_t)8192, fileSize - bytesRead);
        epubFile.read((uint8_t*)pZipData + bytesRead, toRead);
        bytesRead += toRead;
        yield(); // WATCHDOG SAFETY
    }
    epubFile.close();

    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    if (!mz_zip_reader_init_mem(&zip_archive, pZipData, fileSize, 0)) {
        free(pZipData);
        return false;
    }

    // 2. Parse META-INF to locate the OPF registry file
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

    // 3. Parse OPF XML to resolve Chapter Reading Order
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
                    if (anchor >= 0) href = href.substring(0, anchor);
                    chapterFiles.push_back(basePath + href);
                }
            }
        }
    }

    // 4. Stitch, Strip, and Save the final TXT cache
    if (SD.exists(txtPath.c_str())) SD.remove(txtPath.c_str());
    File txtFile = SD.open(txtPath.c_str(), FILE_WRITE);
    
    for (String chapterName : chapterFiles) {
        String html = readZipFileStr(&zip_archive, chapterName.c_str());
        if (html.length() > 0) {
            String cleanText = stripHTML(html);
            txtFile.print(cleanText);
            txtFile.print("\n\n");
            yield(); // WATCHDOG SAFETY
        }
    }
    
    txtFile.close();
    mz_zip_reader_end(&zip_archive);
    free(pZipData);

    return true;
}

#endif
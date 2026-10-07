#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <vector>

// Raw engine headers
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>

#include "splash.h"

// ==========================================
// 1. HARDWARE CONFIGURATION
// ==========================================
#define EPD_PWR       7
#define EPD_CS        45
#define EPD_DC        46
#define EPD_RES       47
#define EPD_BUSY      48

#define SPI_MOSI      11
#define SPI_MISO      -1
#define SPI_SCK       12

#define SD_PWR        42
#define SD_CS         10
#define SD_MOSI       40
#define SD_MISO       13
#define SD_SCK        39

// Physical Inputs
#define ROT_A         6
#define ROT_B         4
#define ROT_PUSH      5
#define BTN_MENU      2
#define BTN_EXIT      1

GxEPD2_BW<GxEPD2_420_SE0420NQ04, GxEPD2_420_SE0420NQ04::HEIGHT> display(
    GxEPD2_420_SE0420NQ04(EPD_CS, EPD_DC, EPD_RES, EPD_BUSY)
);

SPIClass sdSPI(FSPI);

// ==========================================
// 2. GLOBALS
// ==========================================
// Text Engine
uint8_t* font_buffer = nullptr;
FT_Library ft_library;
FT_Face ft_face;
hb_font_t *hb_font;

// GUI State
std::vector<String> bookList;
int currentSelection = 0;
bool inReadingMode = false;

// Input State
int lastRotA = HIGH;
unsigned long lastButtonPress = 0;

// ==========================================
// 3. HARDWARE INIT & ENGINE
// ==========================================
void showError(const char* msg) {
    Serial.println(msg);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setTextSize(2);
        display.setCursor(20, 50);
        display.print("SYSTEM ERROR:");
        display.setCursor(20, 100);
        display.print(msg);
    } while (display.nextPage());
}

void showBootScreen() {
    display.setFullWindow();
    display.setRotation(0); 
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.drawBitmap((400 - LOGO_WIDTH) / 2, (300 - LOGO_HEIGHT) / 2, epipar_pi_logo, LOGO_WIDTH, LOGO_HEIGHT, GxEPD_BLACK);
    } while (display.nextPage());
    Serial.println("Boot screen drawn.");
}

bool mountSDCard() {
    pinMode(SD_PWR, OUTPUT);
    digitalWrite(SD_PWR, HIGH);
    delay(100);
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sdSPI, 4000000)) {
        showError("SD Mount Failed");
        return false;
    }
    return true;
}

bool initRawEngine() {
    if (FT_Init_FreeType(&ft_library)) {
        showError("FreeType Init Failed");
        return false;
    }

    File root = SD.open("/");
    File file = root.openNextFile();
    String fontFilename = "";
    
    // Dynamically locate the first TrueType font
    while (file) {
        String filename = String(file.name());
        if (!file.isDirectory() && (filename.endsWith(".ttf") || filename.endsWith(".TTF"))) {
            fontFilename = "/" + filename;
            file.close();
            break;
        }
        file.close();
        file = root.openNextFile();
    }
    
    if (fontFilename == "") {
        showError("No .ttf file found");
        return false;
    }

    File fontFile = SD.open(fontFilename.c_str(), FILE_READ);
    size_t fileSize = fontFile.size();
    
    font_buffer = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);
    if (!font_buffer) {
        showError("PSRAM Allocation Failed");
        fontFile.close();
        return false;
    }
    
    fontFile.read(font_buffer, fileSize);
    fontFile.close();

    if (FT_New_Memory_Face(ft_library, font_buffer, fileSize, 0, &ft_face)) {
        showError("Face Binding Failed");
        return false;
    }
    FT_Set_Pixel_Sizes(ft_face, 0, 48); 

    hb_font = hb_ft_font_create(ft_face, NULL);
    return true;
}

// ==========================================
// 4. GUI & RENDER LAYERS
// ==========================================
void drawShapedText(const char* text, int start_x, int start_y) {
    hb_buffer_t *hb_buffer = hb_buffer_create();
    hb_buffer_add_utf8(hb_buffer, text, -1, 0, -1);
    
    hb_buffer_set_direction(hb_buffer, HB_DIRECTION_LTR);
    hb_buffer_set_script(hb_buffer, HB_SCRIPT_DEVANAGARI);
    hb_buffer_set_language(hb_buffer, hb_language_from_string("hi", -1));
    
    hb_shape(hb_font, hb_buffer, NULL, 0);
    
    unsigned int glyph_count;
    hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(hb_buffer, &glyph_count);
    hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(hb_buffer, &glyph_count);
    
    int current_x = start_x;
    int current_y = start_y;
    
    for (unsigned int i = 0; i < glyph_count; i++) {
        hb_codepoint_t glyphid = glyph_info[i].codepoint;
        FT_Load_Glyph(ft_face, glyphid, FT_LOAD_DEFAULT);
        FT_Render_Glyph(ft_face->glyph, FT_RENDER_MODE_NORMAL);
        
        int x_pos = current_x + (glyph_pos[i].x_offset >> 6) + ft_face->glyph->bitmap_left;
        int y_pos = current_y + (glyph_pos[i].y_offset >> 6) - ft_face->glyph->bitmap_top;
        
        FT_Bitmap* bitmap = &ft_face->glyph->bitmap;
        for (unsigned int row = 0; row < bitmap->rows; ++row) {
            for (unsigned int col = 0; col < bitmap->width; ++col) {
                if (bitmap->buffer[row * bitmap->pitch + col]) {
                    display.drawPixel(x_pos + col, y_pos + row, GxEPD_BLACK);
                }
            }
        }
        current_x += (glyph_pos[i].x_advance >> 6);
        current_y += (glyph_pos[i].y_advance >> 6);
    }
    hb_buffer_destroy(hb_buffer);
}

void scanForBooks() {
    bookList.clear();
    File root = SD.open("/");
    File file = root.openNextFile();
    while (file) {
        String filename = file.name();
        if (!file.isDirectory() && (filename.endsWith(".txt") || filename.endsWith(".TXT"))) {
            bookList.push_back(filename);
        }
        file.close();
        file = root.openNextFile();
    }
}

void drawLibraryMenu(bool partialRefresh = false) {
    display.setRotation(0);
    
    // Use partial window to make rotary scrolling much faster
    if (partialRefresh) {
        display.setPartialWindow(0, 0, 400, 300);
    } else {
        display.setFullWindow();
    }
    
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.fillRect(0, 0, 400, 40, GxEPD_BLACK);
        display.setTextColor(GxEPD_WHITE);
        display.setTextSize(2); 
        display.setCursor(10, 12);
        display.print("ePiPer Library");
        
        if (bookList.empty()) {
            display.setTextColor(GxEPD_BLACK);
            display.setCursor(10, 60);
            display.print("No .txt files found on SD.");
            continue;
        }

        display.setTextSize(2);
        
        // Scroll window logic
        int startIndex = 0;
        if (currentSelection > 6) startIndex = currentSelection - 6; 

        for (int i = startIndex; i < (int)bookList.size() && i < startIndex + 7; i++) {
            int y_pos = 60 + ((i - startIndex) * 35);
            
            if (i == currentSelection) {
                display.fillRoundRect(5, y_pos - 5, 390, 30, 4, GxEPD_BLACK);
                display.setTextColor(GxEPD_WHITE);
            } else {
                display.setTextColor(GxEPD_BLACK);
            }
            
            display.setCursor(15, y_pos);
            display.print(bookList[i].c_str());
        }
    } while (display.nextPage());
}

void openBook() {
    inReadingMode = true;
    display.setFullWindow(); // Force full refresh to clear menu ghosting
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setTextSize(2);
        display.setCursor(10, 20);
        display.print("Reading: ");
        display.print(bookList[currentSelection].c_str());
        
        // Proof of Engine Capability
        drawShapedText("क्ष त्र ज्ञ श्र", 20, 100);
        drawShapedText("क कि की कु कू", 20, 180);
    } while (display.nextPage());
}

// ==========================================
// 5. LIFECYCLE
// ==========================================
void setup() {
    Serial.begin(115200);
    
    // Setup Inputs
    pinMode(ROT_A, INPUT_PULLUP);
    pinMode(ROT_B, INPUT_PULLUP);
    pinMode(ROT_PUSH, INPUT_PULLUP);
    pinMode(BTN_MENU, INPUT_PULLUP);
    pinMode(BTN_EXIT, INPUT_PULLUP);

    // Init E-Paper
    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, HIGH); 
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);
    display.init(115200, true, 2, false);
    
    showBootScreen();
    
    if (mountSDCard() && initRawEngine()) {
        scanForBooks();
        drawLibraryMenu(false); // Initial full refresh
    }
}

void loop() {
    // 1. Rotary Scroll Polling
    int a = digitalRead(ROT_A);
    int b = digitalRead(ROT_B);
    
    if (a == LOW && lastRotA == HIGH) {
        if (!inReadingMode && !bookList.empty()) {
            if (b == HIGH) {
                currentSelection++;
                if (currentSelection >= (int)bookList.size()) currentSelection = 0;
            } else {
                currentSelection--;
                if (currentSelection < 0) currentSelection = bookList.size() - 1;
            }
            drawLibraryMenu(true); // Fast partial refresh for scrolling
        }
    }
    lastRotA = a;
    
    // 2. Push to Confirm
    if (digitalRead(ROT_PUSH) == LOW && (millis() - lastButtonPress > 300)) {
        lastButtonPress = millis();
        if (!inReadingMode && !bookList.empty()) {
            openBook();
        }
    }
    
    // 3. Exit Back to Menu
    if (digitalRead(BTN_EXIT) == LOW && (millis() - lastButtonPress > 300)) {
        lastButtonPress = millis();
        if (inReadingMode) {
            inReadingMode = false;
            drawLibraryMenu(false); // Full refresh to wipe book ghosting
        }
    }

    delay(2); // Tight debounce 
}
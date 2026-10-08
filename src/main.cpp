#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <vector>

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

// Physical Inputs (Rocker Switch & Buttons)
#define BTN_UP        6  // Rocker Push Up
#define BTN_DOWN      4  // Rocker Push Down
#define BTN_SELECT    5  // Rocker Center Push
#define BTN_MENU      2
#define BTN_EXIT      1

GxEPD2_BW<GxEPD2_420_SE0420NQ04, GxEPD2_420_SE0420NQ04::HEIGHT> display(
    GxEPD2_420_SE0420NQ04(EPD_CS, EPD_DC, EPD_RES, EPD_BUSY)
);

SPIClass sdSPI(FSPI);

// ==========================================
// 2. GLOBALS
// ==========================================
uint8_t* font_buffer = nullptr;
FT_Library ft_library;
FT_Face ft_face;
hb_font_t *hb_font;

std::vector<String> bookList;
int currentSelection = 0;
bool inReadingMode = false;
unsigned long lastButtonPress = 0;

// ==========================================
// 3. TEXT SHAPING ENGINE
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
}

bool mountSDCard() {
    pinMode(SD_PWR, OUTPUT);
    digitalWrite(SD_PWR, HIGH);
    delay(100);
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    return SD.begin(SD_CS, sdSPI, 4000000);
}

bool initRawEngine() {
    if (FT_Init_FreeType(&ft_library)) return false;

    File root = SD.open("/");
    File file = root.openNextFile();
    String fontFilename = "";
    
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
    
    if (fontFilename == "") return false;

    File fontFile = SD.open(fontFilename.c_str(), FILE_READ);
    size_t fileSize = fontFile.size();
    font_buffer = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);
    
    fontFile.read(font_buffer, fileSize);
    fontFile.close();

    if (FT_New_Memory_Face(ft_library, font_buffer, fileSize, 0, &ft_face)) return false;
    
    hb_font = hb_ft_font_create(ft_face, NULL);
    return true;
}

// CRITICAL FIX: Update HarfBuzz metrics when FreeType font size changes
void setFontSize(int size) {
    FT_Set_Pixel_Sizes(ft_face, 0, size);
    hb_ft_font_changed(hb_font); 
}

void drawShapedText(const char* text, int start_x, int start_y, uint16_t color = GxEPD_BLACK) {
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
                    display.drawPixel(x_pos + col, y_pos + row, color);
                }
            }
        }
        current_x += (glyph_pos[i].x_advance >> 6);
        current_y += (glyph_pos[i].y_advance >> 6);
    }
    hb_buffer_destroy(hb_buffer);
}

// ==========================================
// 4. GUI & FILE EXPLORER
// ==========================================
void scanForBooks() {
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

void drawLibraryMenu(bool partialRefresh = false) {
    display.setRotation(0);
    if (partialRefresh) display.setPartialWindow(0, 0, 400, 300);
    else display.setFullWindow();
    
    setFontSize(24); // Syncs FreeType and HarfBuzz to 24px
    
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        
        // Header
        display.fillRect(0, 0, 400, 40, GxEPD_BLACK);
        drawShapedText("ePiPer Library", 10, 28, GxEPD_WHITE);
        
        if (bookList.empty()) {
            drawShapedText("No books found on SD.", 10, 80);
            continue;
        }

        int visibleItems = 6;
        int startIndex = 0;
        if (currentSelection >= visibleItems) startIndex = currentSelection - visibleItems + 1; 

        // Draw Book List
        for (int i = startIndex; i < (int)bookList.size() && i < startIndex + visibleItems; i++) {
            int y_pos = 75 + ((i - startIndex) * 40); 
            
            if (i == currentSelection) {
                display.fillRoundRect(5, y_pos - 24, 380, 34, 4, GxEPD_BLACK);
                drawShapedText(bookList[i].c_str(), 15, y_pos, GxEPD_WHITE);
            } else {
                drawShapedText(bookList[i].c_str(), 15, y_pos, GxEPD_BLACK);
            }
        }
        
        // Draw Dynamic Scrollbar
        if (bookList.size() > visibleItems) {
            int totalListHeight = 260; // 300px screen - 40px header
            int barHeight = max(20, (totalListHeight * visibleItems) / (int)bookList.size());
            int barY = 40 + ((currentSelection * (totalListHeight - barHeight)) / (bookList.size() - 1));
            
            display.drawFastVLine(395, 40, 260, GxEPD_BLACK); // Track line
            display.fillRoundRect(392, barY, 6, barHeight, 3, GxEPD_BLACK); // Indicator thumb
        }

    } while (display.nextPage());
}

void openBook() {
    inReadingMode = true;
    display.setFullWindow(); 
    
    setFontSize(26); // Set a comfortable reading size
    
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        
        File file = SD.open(bookList[currentSelection].c_str());
        if (!file) {
            drawShapedText("Error reading file.", 10, 50);
        } else {
            int y_pos = 35;
            // Read lines until the screen is full
            while (file.available() && y_pos < 290) {
                String line = file.readStringUntil('\n');
                line.trim(); // Remove trailing \r characters
                if (line.length() > 0) {
                    drawShapedText(line.c_str(), 10, y_pos);
                    y_pos += 35; // Advance line height
                }
            }
            file.close();
        }
    } while (display.nextPage());
}

// ==========================================
// 5. LIFECYCLE
// ==========================================
void setup() {
    Serial.begin(115200);
    
    // Treat Rocker Switch like standard buttons
    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_SELECT, INPUT_PULLUP);
    pinMode(BTN_MENU, INPUT_PULLUP);
    pinMode(BTN_EXIT, INPUT_PULLUP);

    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, HIGH); 
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);
    display.init(115200, true, 2, false);
    
    showBootScreen();
    
    if (mountSDCard() && initRawEngine()) {
        scanForBooks();
        drawLibraryMenu(false); 
    }
}

void loop() {
    unsigned long currentMillis = millis();
    
    if (currentMillis - lastButtonPress > 250) { // Debounce delay
    
        // 1. Scroll Up (Rocker Up)
        if (digitalRead(BTN_UP) == LOW) {
            lastButtonPress = currentMillis;
            if (!inReadingMode && !bookList.empty()) {
                currentSelection--;
                if (currentSelection < 0) currentSelection = bookList.size() - 1;
                drawLibraryMenu(true);
            }
        }
        
        // 2. Scroll Down (Rocker Down OR Menu Button Fallback)
        if (digitalRead(BTN_DOWN) == LOW || digitalRead(BTN_MENU) == LOW) {
            lastButtonPress = currentMillis;
            if (!inReadingMode && !bookList.empty()) {
                currentSelection++;
                if (currentSelection >= (int)bookList.size()) currentSelection = 0;
                drawLibraryMenu(true);
            }
        }
        
        // 3. Select / Open File (Center Push)
        if (digitalRead(BTN_SELECT) == LOW) {
            lastButtonPress = currentMillis;
            if (!inReadingMode && !bookList.empty()) {
                openBook();
            }
        }
        
        // 4. Exit / Back Button
        if (digitalRead(BTN_EXIT) == LOW) {
            lastButtonPress = currentMillis;
            if (inReadingMode) {
                inReadingMode = false;
                drawLibraryMenu(false); 
            }
        }
    }

    delay(10); 
}
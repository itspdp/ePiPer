#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>

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

GxEPD2_BW<GxEPD2_420_SE0420NQ04, GxEPD2_420_SE0420NQ04::HEIGHT> display(
    GxEPD2_420_SE0420NQ04(EPD_CS, EPD_DC, EPD_RES, EPD_BUSY)
);

SPIClass sdSPI(FSPI);

// ==========================================
// 2. TEXT ENGINE GLOBALS
// ==========================================
uint8_t* font_buffer = nullptr;
FT_Library ft_library;
FT_Face ft_face;
hb_font_t *hb_font;

// ==========================================
// 3. ENGINE LOGIC & SHAPING
// ==========================================
void showError(const char* msg) {
    Serial.println(msg);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setTextSize(2);
        display.setCursor(20, 50);
        display.print("ENGINE ERROR:");
        display.setCursor(20, 100);
        display.print(msg);
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
    // 1. Init FreeType Core
    if (FT_Init_FreeType(&ft_library)) {
        showError("FreeType Core Init Failed");
        return false;
    }

    // 2. Load Font into PSRAM
    File fontFile = SD.open("/notodev.ttf", FILE_READ);
    if (!fontFile) {
        showError("Missing /notodev.ttf");
        return false;
    }
    
    size_t fileSize = fontFile.size();
    font_buffer = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);
    if (!font_buffer) {
        showError("PSRAM Allocation Failed");
        fontFile.close();
        return false;
    }
    
    fontFile.read(font_buffer, fileSize);
    fontFile.close();

    // 3. Bind FreeType to Font Buffer
    if (FT_New_Memory_Face(ft_library, font_buffer, fileSize, 0, &ft_face)) {
        showError("FreeType Face Binding Failed");
        return false;
    }
    FT_Set_Pixel_Sizes(ft_face, 0, 48); // Set font size

    // 4. Bind HarfBuzz to FreeType
    hb_font = hb_ft_font_create(ft_face, NULL);
    
    Serial.println("SUCCESS: Raw HarfBuzz Engine Linked");
    return true;
}

void drawShapedText(const char* text, int start_x, int start_y) {
    // 1. Create a HarfBuzz buffer for the incoming text
    hb_buffer_t *hb_buffer = hb_buffer_create();
    hb_buffer_add_utf8(hb_buffer, text, -1, 0, -1);
    
    // 2. Apply Hindi / Devanagari specific layout rules
    hb_buffer_set_direction(hb_buffer, HB_DIRECTION_LTR);
    hb_buffer_set_script(hb_buffer, HB_SCRIPT_DEVANAGARI);
    hb_buffer_set_language(hb_buffer, hb_language_from_string("hi", -1));
    
    // 3. SHAPE THE TEXT (Matra reordering & Conjunct formation)
    hb_shape(hb_font, hb_buffer, NULL, 0);
    
    // 4. Extract shaped glyph IDs and calculated X/Y offsets
    unsigned int glyph_count;
    hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(hb_buffer, &glyph_count);
    hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(hb_buffer, &glyph_count);
    
    int current_x = start_x;
    int current_y = start_y;
    
    // 5. Draw the absolute glyphs
    for (unsigned int i = 0; i < glyph_count; i++) {
        hb_codepoint_t glyphid = glyph_info[i].codepoint;
        
        FT_Load_Glyph(ft_face, glyphid, FT_LOAD_DEFAULT);
        FT_Render_Glyph(ft_face->glyph, FT_RENDER_MODE_NORMAL);
        
        // Apply HarfBuzz calculated offsets (divided by 64 for sub-pixel scaling)
        int x_pos = current_x + (glyph_pos[i].x_offset >> 6) + ft_face->glyph->bitmap_left;
        int y_pos = current_y + (glyph_pos[i].y_offset >> 6) - ft_face->glyph->bitmap_top;
        
        // Push pixels to e-paper buffer
        FT_Bitmap* bitmap = &ft_face->glyph->bitmap;
        for (unsigned int row = 0; row < bitmap->rows; ++row) {
            for (unsigned int col = 0; col < bitmap->width; ++col) {
                if (bitmap->buffer[row * bitmap->pitch + col]) {
                    display.drawPixel(x_pos + col, y_pos + row, GxEPD_BLACK);
                }
            }
        }
        
        // Advance cursor
        current_x += (glyph_pos[i].x_advance >> 6);
        current_y += (glyph_pos[i].y_advance >> 6);
    }
    
    hb_buffer_destroy(hb_buffer);
}

// ==========================================
// 4. MAIN LIFECYCLE
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, HIGH); 
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);
    display.init(115200, true, 2, false);
    
    if (mountSDCard() && initRawEngine()) {
        display.setRotation(0);
        display.firstPage();
        do {
            display.fillScreen(GxEPD_WHITE);
            drawShapedText("ePiPer Reader", 20, 100);
            drawShapedText("क्ष त्र ज्ञ श्र", 20, 180);
            drawShapedText("क कि की कु कू", 20, 260);
        } while (display.nextPage());
    }
}

void loop() {
    delay(1000);
}
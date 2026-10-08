#ifndef TEXT_ENGINE_H
#define TEXT_ENGINE_H

#include <Arduino.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>

extern GxEPD2_BW<GxEPD2_420_SE0420NQ04, GxEPD2_420_SE0420NQ04::HEIGHT> display;

extern uint8_t* font_buffer;
extern FT_Library ft_library;
extern FT_Face ft_face;
extern hb_font_t *hb_font;

inline void setFontSize(int size) {
    FT_Set_Pixel_Sizes(ft_face, 0, size);
    hb_ft_font_changed(hb_font); 
}

inline bool initRawEngine() {
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

// NEW: Calculates exact pixel width of a shaped word for word-wrapping
inline int getTextWidth(const char* text) {
    hb_buffer_t *hb_buffer = hb_buffer_create();
    hb_buffer_add_utf8(hb_buffer, text, -1, 0, -1);
    hb_buffer_set_direction(hb_buffer, HB_DIRECTION_LTR);
    hb_buffer_set_script(hb_buffer, HB_SCRIPT_DEVANAGARI);
    hb_buffer_set_language(hb_buffer, hb_language_from_string("hi", -1));
    hb_shape(hb_font, hb_buffer, NULL, 0);
    
    unsigned int glyph_count;
    hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(hb_buffer, &glyph_count);
    
    int width = 0;
    for (unsigned int i = 0; i < glyph_count; i++) {
        width += (glyph_pos[i].x_advance >> 6);
    }
    hb_buffer_destroy(hb_buffer);
    return width;
}

inline void drawShapedText(const char* text, int start_x, int start_y, uint16_t color = GxEPD_BLACK) {
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
                // The "> 128" threshold kills the smudgy gray pixels!
                if (bitmap->buffer[row * bitmap->pitch + col] > 128) { 
                    display.drawPixel(x_pos + col, y_pos + row, color);
                }
            }
        }
        current_x += (glyph_pos[i].x_advance >> 6);
        current_y += (glyph_pos[i].y_advance >> 6);
    }
    hb_buffer_destroy(hb_buffer);
}

#endif
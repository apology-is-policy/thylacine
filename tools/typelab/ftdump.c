// ftdump -- dump FreeType glyph coverage masks (PGM) + metrics for typelab.
//
//   ftdump <ttf> <px> <mode> <outdir> <prefix> <utf8 text>
//
// mode: nohint      -- outlines scaled, no hinting at all (the fontdue-class control)
//       light       -- the autohinter's LIGHT target (vertical-only snapping)
//       light-dark  -- LIGHT + the autohinter's stem darkening property
//       normal      -- the TrueType bytecode interpreter (v40), NORMAL target
//
// Writes <outdir>/<prefix>.idx:
//   face <ascender> <descender> <height>          (px, from the scaled size metrics)
//   g <cp> <w> <h> <left> <top> <advance> <linear_advance> <file>
// and one 8-bit PGM per glyph.
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char *utf8_next(const unsigned char *p, unsigned *cp) {
    unsigned c = *p;
    if (c < 0x80) { *cp = c; return p + 1; }
    if ((c & 0xE0) == 0xC0) { *cp = ((c & 0x1F) << 6) | (p[1] & 0x3F); return p + 2; }
    if ((c & 0xF0) == 0xE0) { *cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); return p + 3; }
    *cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    return p + 4;
}

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: ftdump <ttf> <px> <nohint|light|light-dark|normal> <outdir> <prefix> <text>\n");
        return 2;
    }
    const char *ttf = argv[1];
    double px = atof(argv[2]);
    const char *mode = argv[3];
    const char *outdir = argv[4];
    const char *prefix = argv[5];
    const unsigned char *text = (const unsigned char *)argv[6];
    /* Optional: shift the (hinted) outline right by N thirds of a pixel before
       rendering -- subpixel positioning on top of vertical-only hinting. */
    int thirds = argc > 7 ? atoi(argv[7]) : 0;

    FT_Library lib;
    if (FT_Init_FreeType(&lib)) { fprintf(stderr, "FT_Init_FreeType failed\n"); return 1; }
    if (strcmp(mode, "light-dark") == 0) {
        FT_Bool no_dark = 0;
        if (FT_Property_Set(lib, "autofitter", "no-stem-darkening", &no_dark))
            fprintf(stderr, "warning: stem-darkening property not accepted\n");
    }
    FT_Face face;
    if (FT_New_Face(lib, ttf, 0, &face)) { fprintf(stderr, "FT_New_Face failed: %s\n", ttf); return 1; }
    if (FT_Set_Char_Size(face, 0, (FT_F26Dot6)(px * 64.0 + 0.5), 72, 72)) { fprintf(stderr, "FT_Set_Char_Size failed\n"); return 1; }

    FT_Int32 flags = FT_LOAD_NO_BITMAP;
    if (strcmp(mode, "nohint") == 0) flags |= FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT;
    else if (strcmp(mode, "light") == 0 || strcmp(mode, "light-dark") == 0) flags |= FT_LOAD_TARGET_LIGHT | FT_LOAD_FORCE_AUTOHINT;
    else if (strcmp(mode, "normal") == 0) flags |= FT_LOAD_TARGET_NORMAL;
    else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }

    char path[4096];
    snprintf(path, sizeof path, "%s/%s.idx", outdir, prefix);
    FILE *idx = fopen(path, "w");
    if (!idx) { perror(path); return 1; }
    fprintf(idx, "face %.4f %.4f %.4f\n", face->size->metrics.ascender / 64.0,
            face->size->metrics.descender / 64.0, face->size->metrics.height / 64.0);

    const unsigned char *p = text;
    while (*p) {
        unsigned cp;
        p = utf8_next(p, &cp);
        if (FT_Load_Char(face, cp, flags)) { fprintf(stderr, "load U+%04X failed\n", cp); continue; }
        if (thirds) FT_Outline_Translate(&face->glyph->outline, (FT_Pos)((thirds * 64 + 1) / 3), 0);
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) { fprintf(stderr, "render U+%04X failed\n", cp); continue; }
        FT_Bitmap *bm = &face->glyph->bitmap;
        char fname[256];
        snprintf(fname, sizeof fname, "%s_%04X.pgm", prefix, cp);
        snprintf(path, sizeof path, "%s/%s", outdir, fname);
        FILE *f = fopen(path, "wb");
        if (!f) { perror(path); return 1; }
        fprintf(f, "P5\n%u %u\n255\n", bm->width, bm->rows);
        for (unsigned r = 0; r < bm->rows; r++)
            fwrite(bm->buffer + r * bm->pitch, 1, bm->width, f);
        fclose(f);
        fprintf(idx, "g %u %u %u %d %d %.4f %.4f %s\n", cp, bm->width, bm->rows,
                face->glyph->bitmap_left, face->glyph->bitmap_top,
                face->glyph->advance.x / 64.0, face->glyph->linearHoriAdvance / 65536.0, fname);
    }
    fclose(idx);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return 0;
}

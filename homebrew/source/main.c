#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <gccore.h>

#include "current_ansi_bin.h"
#include "current_sjis_bin.h"
#include "proposed_ansi_bin.h"
#include "proposed_sjis_bin.h"

// Reads bytes from the IPL ROM over EXI, provided by libogc
extern void __SYS_ReadROM(void *buf, u32 len, u32 offset);

// Size of a glyph cell and of a font sheet in pixels
#define FONT_CELL_SIZE  24
#define FONT_SHEET_SIZE 512

// Location and size of each font in the IPL ROM
#define ROM_SJIS_OFFSET 0x1AFF00
#define ROM_SJIS_SIZE   0x4D000
#define ROM_ANSI_OFFSET 0x1FCF00
#define ROM_ANSI_SIZE   0x3000

// Number of font sets, as IPL, proposed and current
#define FONT_SET_COUNT 3

// A decoded font
struct font {
  // Advance width of each glyph
  const uint8_t *widths;

  // Glyph sheets at 2 bits per pixel in 8x8 tiles
  const uint8_t *sheets;

  // Number of cell columns on a sheet
  int columns;
};

// メモリーカードを確認してください。
static const uint16_t japanese_card[] = {356,  357, 365, 27,  302, 27,  332, 290, 1123,
                                         2964, 231, 246, 223, 240, 229, 212, 2};

// 電源を切って、取扱説明書を読んでください。
static const uint16_t japanese_power[] = {2806, 1574, 290,  2357, 243, 246, 1,   1995, 818, 2365, 3459,
                                          2098, 290,  2904, 291,  247, 223, 240, 229,  212, 2};

// セーブしますか？　はい　いいえ
static const uint16_t japanese_save[] = {318, 27, 345, 231, 270, 233, 219, 8, 0, 255, 212, 0, 212, 212, 216};

// スロットAのメモリーカードにセーブします。
static const uint16_t mixed_slot[] = {316, 368, 326, 331, 557, 254, 356, 357, 365, 27, 302,
                                      27,  332, 251, 318, 27,  345, 231, 270, 233, 2};

// STARTボタンを押してください。
static const uint16_t mixed_start[] = {575, 576, 557, 574, 576, 351, 322, 374, 290,
                                       984, 231, 246, 223, 240, 229, 212, 2};

// ﾊﾝｶｸ ｶﾀｶﾅ ﾃﾞｨｽｸ 123 abc
static const uint16_t mixed_half[] = {660, 679, 640, 642, 524, 640, 650, 640, 655, 524, 653, 680,
                                      626, 647, 642, 524, 541, 542, 543, 524, 589, 590, 591};

// ＧａｍｅＣｕｂｅ　１２３４５
static const uint16_t mixed_full[] = {163, 183, 195, 187, 159, 203, 184, 187, 0, 148, 149, 150, 151, 152};

// ＷＷＷＷＷ　ＭＭＭＭＭ　ｉｉｉｉｉ
static const uint16_t extreme_full[] = {179, 179, 179, 179, 179, 0,   169, 169, 169,
                                        169, 169, 0,   191, 191, 191, 191, 191};

// A sample line, as Windows-1252 text or as Shift JIS glyph indices
struct line {
  // Windows-1252 text, or NULL for a Shift JIS line
  const char *text;

  // Shift JIS glyph indices
  const uint16_t *glyphs;

  // Number of Shift JIS glyphs
  int count;
};

// Fill a line from Windows-1252 text
#define TEXT(string) {string, NULL, 0}

// Fill a line from a Shift JIS glyph array
#define GLYPHS(array) {NULL, array, sizeof(array) / sizeof(array[0])}

// Most lines on a page
#define MAX_LINES 4

// A page of sample lines under a title
struct page {
  // Title shown at the top of the page
  const char *title;

  // Lines on the page, ending at the first empty line
  struct line lines[MAX_LINES];
};

// Pages in display order
static const struct page pages[] = {
  {"Latin text",
   {TEXT("The quick brown fox jumps over"), TEXT("The Memory Card in Slot A"),
    TEXT("Press A to continue, B to exit.")}},
  {"Accented Latin text",
   {TEXT("Gr\xF6\xDF"
         "e, \xE9l\xE8ve, se\xF1or"),
    TEXT("\xC0g\xE9 \xC9quipe, \xC5ngstr\xF6m"),
    TEXT("\xC7"
         "a d\xE9j\xE0 vu, na\xEFve caf\xE9")}},
  {"Japanese text", {GLYPHS(japanese_card), GLYPHS(japanese_power), GLYPHS(japanese_save)}},
  {"Mixed Japanese and Latin text", {GLYPHS(mixed_slot), GLYPHS(mixed_start), GLYPHS(mixed_half), GLYPHS(mixed_full)}},
  {"Very wide and very narrow characters",
   {TEXT("WWWWWWWWWW MMMMMMMMMM"), TEXT("iiiiiiiiii llllllllll ||||||||||"), TEXT("@@@@@ %%%%% ##### &&&&&"),
    GLYPHS(extreme_full)}},
};

// Video mode and the framebuffer every glyph is drawn into
static GXRModeObj *mode;
static u32 *framebuffer;

// Decompress a Yay0 font and find its width table and glyph sheets, leaving the font empty if there is none
static void font_load(struct font *font, const uint8_t *packed)
{
  // A missing or unreadable font has no Yay0 header
  if (memcmp(packed, "Yay0", 4) != 0)
    return;

  // The console is big endian and every header field is aligned, so fields are read in place
  const u32 *header = (const u32 *)packed;
  uint32_t size     = header[1];
  uint8_t *data     = malloc(size);
  uint32_t link     = header[2];
  uint32_t chunk    = header[3];
  uint32_t masks    = 16;
  uint32_t out      = 0;
  uint32_t mask     = 0;

  // Decode Yay0, where each mask bit picks a literal byte or a back reference
  for (int bits = 0; out < size; bits--, mask <<= 1) {
    if (bits == 0) {
      mask = *(const u32 *)(packed + masks);
      masks += 4;
      bits = 32;
    }

    if (mask & 0x80000000) {
      data[out++] = packed[chunk++];
    } else {
      uint32_t code  = *(const u16 *)(packed + link);
      uint32_t count = code >> 12 ? (code >> 12) + 2 : packed[chunk++] + 0x12u;
      link += 2;
      for (; count; count--, out++)
        data[out] = data[out - (code & 0xFFF) - 1];
    }
  }

  // Pick the width table, sheet image and sheet layout out of the font header
  font->widths  = data + *(u16 *)(data + 0x22);
  font->sheets  = data + *(u32 *)(data + 0x24);
  font->columns = *(u16 *)(data + 0x1A);
}

// Draw one glyph with its top left corner at (x, y), and return its advance width
static int draw_glyph(const struct font *font, int glyph, int x, int y)
{
  // Find the sheet holding the glyph and the glyph's cell on that sheet
  int per_sheet        = font->columns * font->columns;
  const uint8_t *sheet = font->sheets + glyph / per_sheet * FONT_SHEET_SIZE * FONT_SHEET_SIZE / 4;
  int cell_x           = glyph % per_sheet % font->columns * FONT_CELL_SIZE;
  int cell_y           = glyph % per_sheet / font->columns * FONT_CELL_SIZE;

  for (int py = 0; py < FONT_CELL_SIZE; py++) {
    for (int px = 0; px < FONT_CELL_SIZE; px++) {
      // Sheets are stored as 8x8 tiles of 2 bit pixels, 16 bytes per tile
      int sheet_x = cell_x + px;
      int sheet_y = cell_y + py;
      int tile    = sheet_y / 8 * (FONT_SHEET_SIZE / 8) + sheet_x / 8;
      int level   = sheet[tile * 16 + sheet_y % 8 * 2 + sheet_x % 8 / 4] >> (6 - 2 * (sheet_x % 4)) & 3;
      if (level == 0)
        continue;

      // The framebuffer packs two pixels per word as Y1 Cb Y2 Cr, so set only the luma of this pixel
      u32 *word = &framebuffer[(y + py) * mode->fbWidth / 2 + (x + px) / 2];
      int shift = (x + px) % 2 ? 8 : 24;
      *word     = (*word & ~(0xFF << shift)) | (235 - level * 73) << shift;
    }
  }

  return font->widths[glyph];
}

// Draw a line of Windows-1252 text or Shift JIS glyphs starting at (x, y)
static void draw_line(const struct font *font, const struct line *line, int x, int y)
{
  // Windows-1252 glyphs start at code 0x20
  for (const char *c = line->text; c != NULL && *c != 0; c++)
    x += draw_glyph(font, (uint8_t)*c - 0x20, x, y);

  for (int i = 0; i < line->count; i++)
    x += draw_glyph(font, line->glyphs[i], x, y);
}

int main(void)
{
  // Set up a single framebuffer in the console's preferred video mode
  VIDEO_Init();
  mode        = VIDEO_GetPreferredMode(NULL);
  framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
  VIDEO_Configure(mode);
  VIDEO_SetNextFramebuffer(framebuffer);
  VIDEO_SetBlack(false);
  VIDEO_Flush();

  // Initialize the controllers
  PAD_Init();

  // Read the IPL fonts from the console, so none of their data ships with this program
  uint8_t *rom_sjis = memalign(32, ROM_SJIS_SIZE);
  uint8_t *rom_ansi = memalign(32, ROM_ANSI_SIZE);
  __SYS_ReadROM(rom_sjis, ROM_SJIS_SIZE, ROM_SJIS_OFFSET);
  __SYS_ReadROM(rom_ansi, ROM_ANSI_SIZE, ROM_ANSI_OFFSET);

  // Load the IPL, proposed and current font sets, as Windows-1252 then Shift JIS
  struct font fonts[FONT_SET_COUNT][2] = {0};
  font_load(&fonts[0][0], rom_ansi);
  font_load(&fonts[0][1], rom_sjis);
  font_load(&fonts[1][0], proposed_ansi_bin);
  font_load(&fonts[1][1], proposed_sjis_bin);
  font_load(&fonts[2][0], current_ansi_bin);
  font_load(&fonts[2][1], current_sjis_bin);
  const char *names[FONT_SET_COUNT] = {"IPL", "New", "Cur"};

  // Draw labels, titles and the page counter with the proposed Windows-1252 font
  const struct font *ui_font = &fonts[1][0];
  int page_count             = sizeof(pages) / sizeof(pages[0]);
  int page                   = 0;

  while (1) {
    // Draw the page title and counter
    VIDEO_ClearFrameBuffer(mode, framebuffer, COLOR_WHITE);
    draw_line(ui_font, &(struct line)TEXT(pages[page].title), 32, 24);
    char counter[] = {'1' + page, '/', '0' + page_count, 0};
    draw_line(ui_font, &(struct line)TEXT(counter), 560, 24);

    // Draw each line once per font set, one row under the other, with the label at the left
    int y = 72;
    for (int n = 0; n < MAX_LINES; n++) {
      const struct line *line = &pages[page].lines[n];
      if (line->text == NULL && line->glyphs == NULL)
        break;

      for (int set = 0; set < FONT_SET_COUNT; set++, y += FONT_CELL_SIZE + 2) {
        draw_line(ui_font, &(struct line)TEXT(names[set]), 32, y);

        // A Windows-1252 line uses the first font of the set and a Shift JIS line the second
        const struct font *font = &fonts[set][line->text != NULL ? 0 : 1];
        if (font->widths != NULL)
          draw_line(font, line, 96, y);
      }

      y += 10;
    }

    // Wait for the D-pad to change the page, or START to exit
    u16 down = 0;
    while (!(down & (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_START))) {
      VIDEO_WaitVSync();
      PAD_ScanPads();
      down = PAD_ButtonsDown(0);
    }

    if (down & PAD_BUTTON_START)
      exit(0);

    page = (page + (down & PAD_BUTTON_RIGHT ? 1 : page_count - 1)) % page_count;
  }

  return 0;
}

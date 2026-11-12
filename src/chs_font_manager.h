#ifndef FALLOUT_CHS_FONT_MANAGER_H_
#define FALLOUT_CHS_FONT_MANAGER_H_

// Chinese (GBK) TrueType font support.
//
// Ported from yurikaka/fallout1-ce-chs (which itself is based on
// sonilyan/fallout2-ce's truetype-support branch) and adapted to this fork's
// FontManager / text_font abstraction:
//
//   * FreeType + libiconv + zlib are replaced by the stb_truetype header that
//     is already vendored in third_party/stb, plus a static GBK table. The
//     Android NDK has no libiconv, and FetchContent would need network access
//     at configure time inside Gradle.
//   * The font manager installs a CJK-aware wordWrapProc (see word_wrap.cc),
//     because the original one walks single bytes and happily splits a GBK
//     double-byte character in half.
//   * Game text is assumed to be GBK (the encoding used by the Chinese
//     Fallout translations). UTF-8 is also accepted via font.ini.
//
// Activation is purely data-driven, exactly like the source fork: if
// `fonts/chs/font.ini` and the TTF it points at are present in the game's
// VFS, fonts 100..110 become TrueType Chinese fonts. If either is missing the
// stock AAF bitmap font manager is registered instead and the game behaves
// exactly as before.

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace fallout {

// Returns 0 when the Chinese font manager took over fonts 100..110, -1 when
// the game should keep using the stock AAF fonts.
int chsFontsInit();
void chsFontsExit();
bool chsFontsActive();

// --- Encoding -------------------------------------------------------------

// Number of bytes making up the next character: 1 for ASCII / the 0x95 knob,
// 2 for a GBK double-byte character (2..4 for UTF-8 when configured).
int chsCharLength(const char* text);

// Unicode codepoint of the next character. 0xFFFD when the bytes cannot be
// mapped. `length`, when provided, receives the number of bytes consumed.
char32_t chsDecodeChar(const char* text, int* length = nullptr);

// Whole-string conversions used by the mobile UI (which works in UTF-32).
std::u32string chsDecodeString(const char* text);
std::string chsEncodeString(const std::u32string& text);

// --- Layout ---------------------------------------------------------------

// Build one line that fits `maxWidth` pixels into `out`, never splitting a
// multi-byte character. Measurement uses the currently selected font, so
// callers must fontSetCurrent() first. Returns the number of input bytes
// consumed (the offset where the next line starts).
size_t chsBuildLine(const char* text, int maxWidth, char* out, size_t outSize);

// CJK-aware replacement for wordWrap(), installed into gChsFontManager.
int chsWordWrap(const char* string, int width, short* breakpoints, short* breakpointsLengthPtr);

// Kinsoku helpers, exported for renderers that do their own line breaking
// (the mobile UI wraps text on std::u32string before measuring it).
//
// `chsIsNoStartChar` is true for closing punctuation that may not begin a
// line (，。！？：；」』）, `chsIsNoEndChar` for opening brackets that may not
// end one (（「『《【).
bool chsIsNoStartChar(char32_t codepoint);
bool chsIsNoEndChar(char32_t codepoint);

// --- Font data sharing ----------------------------------------------------

// Raw TTF/TTC bytes of the primary Chinese font, for the bottom-screen UI
// renderer. Returns nullptr when Chinese fonts are not active. The buffer
// stays alive until chsFontsExit().
const unsigned char* chsFontData(int* size);

} // namespace fallout

#endif // FALLOUT_CHS_FONT_MANAGER_H_

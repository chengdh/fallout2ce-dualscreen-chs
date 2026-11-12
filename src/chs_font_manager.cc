#include "chs_font_manager.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "chs_gbk_table.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include "color.h"
#include "config.h"
#include "db.h"
#include "debug.h"
#include "platform_compat.h"
#include "text_font.h"
#include "word_wrap.h"

namespace fallout {

namespace {

    // Same font ids the stock AAF manager uses. Registering ours first makes
    // fontSetCurrent(100..110) resolve to the Chinese glyphs.
    constexpr int kChsFontBase = 100;
    constexpr int kChsFontCount = 16;
    constexpr int kChsFontMax = 110;

    constexpr char kChsConfigPath[] = "fonts\\chs\\font.ini";
    constexpr char kChsFontDir[] = "fonts\\chs\\";

    // 0x95 is the original engine's "conversation knob" glyph. It is inserted
    // in front of dialogue options and display-monitor messages. GBK also uses
    // 0x95 as a (very rare) lead byte, so only treat it as the knob when the
    // following byte cannot be a plausible GBK trail.
    constexpr unsigned char kChsKnob = 0x95;
    constexpr char32_t kReplacementChar = 0xFFFD;

    // The 5x7 knob bitmap from fallout1-ce-chs, kept for pixel parity with the
    // original game's bullet marker.
    constexpr int kKnobWidth = 5;
    constexpr int kKnobHeight = 7;
    const unsigned char kKnobDump[kKnobWidth * kKnobHeight] = {
        0x34, 0x82, 0xB6, 0x82, 0x34,
        0x82, 0xB6, 0xB6, 0xB6, 0x82,
        0xB6, 0xB6, 0xB6, 0xB6, 0xB6,
        0x82, 0xB6, 0xB6, 0xB6, 0x82,
        0x34, 0x82, 0xB6, 0x82, 0x34,
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
    };

    enum class ChsEncoding {
        Gbk,
        Utf8,
    };

    struct ChsGlyph {
        int width = 0;
        int height = 0;
        int xOffset = 0;
        int yOffset = 0;
        int advance = 0;
        std::vector<unsigned char> pixels;
    };

    struct ChsFontDescriptor {
        bool loaded = false;
        int maxHeight = 0;
        int maxWidth = 0;
        int letterSpacing = 0;
        int wordSpacing = 0;
        int lineSpacing = 0;
        int heightOffset = 0;
        int warpMode = 0;
        int baseline = 0;
        ChsEncoding encoding = ChsEncoding::Gbk;
        stbtt_fontinfo fontInfo = {};
        std::vector<unsigned char> fileBuffer;
        std::unordered_map<char32_t, ChsGlyph> glyphs;
    };

    ChsFontDescriptor gChsFonts[kChsFontCount];
    ChsFontDescriptor* gChsCurrentFont = nullptr;
    int gChsCurrentFontIndex = -1;
    bool gChsActive = false;
    ChsEncoding gChsEncoding = ChsEncoding::Gbk;

    FontManager gChsFontManager;

    // --- Forward declarations --------------------------------------------

    static void chsSetCurrentImpl(int font);
    static void chsDrawImpl(unsigned char* buf, const char* string, int length, int pitch, ColorWithFlags color);
    static int chsGetLineHeightImpl();
    static int chsGetStringWidthImpl(const char* string);
    static int chsGetCharacterWidthImpl(int ch);
    static int chsGetMonospacedStringWidthImpl(const char* string);
    static int chsGetLetterSpacingImpl();
    static int chsGetBufferSizeImpl(const char* string);
    static int chsGetMonospacedCharacterWidthImpl();

    // --- Encoding ---------------------------------------------------------

    bool isGbkLead(unsigned char ch)
    {
        return ch >= kChsGbkLeadMin && ch <= kChsGbkLeadMax;
    }

    bool isGbkTrail(unsigned char ch)
    {
        return ch >= kChsGbkTrailMin && ch <= kChsGbkTrailMax && ch != 0x7F;
    }

    int decodeNext(const char* text, char32_t* codepoint)
    {
        if (text == nullptr || *text == '\0') {
            *codepoint = 0;
            return 0;
        }

        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(text);
        unsigned char b0 = bytes[0];

        if (b0 == kChsKnob) {
            unsigned char b1 = bytes[1];
            if (b1 == '\0' || b1 < 0x80 || isGbkLead(b1)) {
                *codepoint = kChsKnob;
                return 1;
            }
        }

        if (gChsEncoding == ChsEncoding::Utf8 && b0 >= 0xC0) {
            int length = b0 >= 0xF0 ? 4 : (b0 >= 0xE0 ? 3 : 2);
            char32_t cp = b0 & (length == 4 ? 0x07 : (length == 3 ? 0x0F : 0x1F));
            for (int index = 1; index < length; index++) {
                if ((bytes[index] & 0xC0) != 0x80) {
                    *codepoint = kReplacementChar;
                    return 1;
                }
                cp = (cp << 6) | (bytes[index] & 0x3F);
            }
            *codepoint = cp;
            return length;
        }

        if (gChsEncoding == ChsEncoding::Gbk && isGbkLead(b0) && bytes[1] != '\0' && isGbkTrail(bytes[1])) {
            char32_t cp = chsGbkToUnicode(b0, bytes[1]);
            *codepoint = cp != 0 ? cp : kReplacementChar;
            return 2;
        }

        // A lone high byte that isn't a valid pair: don't eat the next byte.
        *codepoint = b0 < 0x80 ? static_cast<char32_t>(b0) : kReplacementChar;
        return 1;
    }

    // --- Glyphs -----------------------------------------------------------

    ChsGlyph makeKnobGlyph()
    {
        ChsGlyph glyph;
        glyph.width = kKnobWidth;
        glyph.height = kKnobHeight;
        glyph.xOffset = 0;
        glyph.yOffset = gChsCurrentFont != nullptr ? -(gChsCurrentFont->maxHeight / 2) : -4;
        glyph.advance = kKnobWidth + 2;
        glyph.pixels.assign(kKnobDump, kKnobDump + sizeof(kKnobDump));
        return glyph;
    }

    const ChsGlyph* getGlyph(char32_t codepoint)
    {
        if (gChsCurrentFont == nullptr) {
            return nullptr;
        }

        auto found = gChsCurrentFont->glyphs.find(codepoint);
        if (found != gChsCurrentFont->glyphs.end()) {
            return &(found->second);
        }

        ChsGlyph glyph;
        if (codepoint == kChsKnob) {
            glyph = makeKnobGlyph();
        } else {
            float scale = stbtt_ScaleForPixelHeight(&(gChsCurrentFont->fontInfo), static_cast<float>(gChsCurrentFont->maxHeight));
            int x0 = 0;
            int y0 = 0;
            int x1 = 0;
            int y1 = 0;
            stbtt_GetCodepointBitmapBox(&(gChsCurrentFont->fontInfo), static_cast<int>(codepoint), scale, scale, &x0, &y0, &x1, &y1);

            glyph.width = std::max(0, x1 - x0);
            glyph.height = std::max(0, y1 - y0);
            glyph.xOffset = x0;
            glyph.yOffset = y0;

            if (glyph.width > 0 && glyph.height > 0) {
                glyph.pixels.assign(static_cast<size_t>(glyph.width) * glyph.height, 0);
                stbtt_MakeCodepointBitmap(&(gChsCurrentFont->fontInfo), glyph.pixels.data(), glyph.width, glyph.height, glyph.width, scale, scale, static_cast<int>(codepoint));
            }

            int advance = 0;
            int leftSideBearing = 0;
            stbtt_GetCodepointHMetrics(&(gChsCurrentFont->fontInfo), static_cast<int>(codepoint), &advance, &leftSideBearing);
            glyph.advance = static_cast<int>(advance * scale + 0.5f);
            if (glyph.advance <= 0) {
                glyph.advance = glyph.width;
            }
        }

        auto inserted = gChsCurrentFont->glyphs.emplace(codepoint, std::move(glyph));
        return &(inserted.first->second);
    }

    int characterAdvance(char32_t codepoint)
    {
        if (gChsCurrentFont == nullptr) {
            return 0;
        }

        if (codepoint == U' ') {
            return gChsCurrentFont->wordSpacing + gChsCurrentFont->letterSpacing;
        }
        if (codepoint == U'\n' || codepoint == U'\r') {
            return 0;
        }
        const ChsGlyph* glyph = getGlyph(codepoint);
        int width = glyph != nullptr ? glyph->advance : gChsCurrentFont->wordSpacing;
        return width + gChsCurrentFont->letterSpacing;
    }

    // --- Loading ----------------------------------------------------------

    ChsEncoding parseEncoding(const char* name)
    {
        if (name != nullptr && (compat_stricmp(name, "utf-8") == 0 || compat_stricmp(name, "utf8") == 0)) {
            return ChsEncoding::Utf8;
        }
        // GBK, GB18030, CP936 and anything unknown all use the GBK table.
        return ChsEncoding::Gbk;
    }

    bool loadFontData(const char* fileName, std::vector<unsigned char>* out)
    {
        char path[COMPAT_MAX_PATH];

        if (fileName[0] == '/' || fileName[0] == '\\' || (fileName[0] != '\0' && fileName[1] == ':')) {
            snprintf(path, sizeof(path), "%s", fileName);
        } else {
            snprintf(path, sizeof(path), "%s%s", kChsFontDir, fileName);
        }

        // fileOpen() goes through the VFS (master.dat / critter.dat / data dir
        // / ce.dat), so a font dropped next to the game files just works.
        File* stream = fileOpen(path, "rb");
        if (stream == nullptr) {
            debugPrint("CHS: cannot open font %s\n", path);
            return false;
        }

        int size = fileGetSize(stream);
        if (size <= 0) {
            fileClose(stream);
            return false;
        }

        out->resize(static_cast<size_t>(size));
        size_t read = fileRead(out->data(), 1, static_cast<size_t>(size), stream);
        fileClose(stream);

        if (static_cast<int>(read) != size) {
            out->clear();
            return false;
        }
        return true;
    }

    bool loadFont(int index, Config* config)
    {
        ChsFontDescriptor* desc = &(gChsFonts[index]);
        desc->loaded = false;

        char section[32];
        snprintf(section, sizeof(section), "font%d", index);

        if (!configGetInt(config, section, "maxHeight", &(desc->maxHeight)) || desc->maxHeight <= 0) {
            return false;
        }
        // Only maxHeight and fileName are mandatory; the rest mirror the
        // fallout1-ce-chs font.ini keys but have sane defaults.
        configGetInt(config, section, "lineSpacing", &(desc->lineSpacing), 2);
        configGetInt(config, section, "wordSpacing", &(desc->wordSpacing), desc->maxHeight / 2);
        configGetInt(config, section, "letterSpacing", &(desc->letterSpacing), 0);
        configGetInt(config, section, "heightOffset", &(desc->heightOffset), 0);
        configGetInt(config, section, "warpMode", &(desc->warpMode), 0);
        configGetInt(config, section, "maxWidth", &(desc->maxWidth), desc->maxHeight);
        if (desc->maxWidth <= 0) {
            desc->maxWidth = desc->maxHeight;
        }
        if (desc->wordSpacing <= 0) {
            desc->wordSpacing = desc->maxHeight / 2;
        }

        char* encodingName = nullptr;
        configGetString(config, section, "encoding", &encodingName, "GBK");
        desc->encoding = parseEncoding(encodingName);

        char* fileName = nullptr;
        if (!configGetString(config, section, "fileName", &fileName) || fileName == nullptr || fileName[0] == '\0') {
            return false;
        }

        if (!loadFontData(fileName, &(desc->fileBuffer)) || desc->fileBuffer.empty()) {
            return false;
        }

        if (!stbtt_InitFont(&(desc->fontInfo), desc->fileBuffer.data(), stbtt_GetFontOffsetForIndex(desc->fileBuffer.data(), 0))) {
            debugPrint("CHS: font%d is not a usable TrueType font\n", index);
            desc->fileBuffer.clear();
            return false;
        }

        int ascent = 0;
        int descent = 0;
        int lineGap = 0;
        stbtt_GetFontVMetrics(&(desc->fontInfo), &ascent, &descent, &lineGap);
        float scale = stbtt_ScaleForPixelHeight(&(desc->fontInfo), static_cast<float>(desc->maxHeight));
        desc->baseline = static_cast<int>(ascent * scale + 0.5f);

        desc->loaded = true;
        return true;
    }

    void releaseFonts()
    {
        for (int index = 0; index < kChsFontCount; index++) {
            gChsFonts[index].glyphs.clear();
            gChsFonts[index].fileBuffer.clear();
            gChsFonts[index].loaded = false;
            gChsFonts[index].fontInfo = {};
        }
        gChsCurrentFont = nullptr;
        gChsCurrentFontIndex = -1;
    }

    // --- FontManager callbacks -------------------------------------------

    void chsSetCurrentImpl(int font)
    {
        int index = font - kChsFontBase;
        if (index < 0 || index >= kChsFontCount) {
            return;
        }

        if (!gChsFonts[index].loaded) {
            // The engine asks for font numbers that a font pack may not
            // define. Pick the closest defined size instead of silently
            // keeping the previous one.
            int nearest = -1;
            int bestDistance = kChsFontCount;
            for (int candidate = 0; candidate < kChsFontCount; candidate++) {
                if (!gChsFonts[candidate].loaded) {
                    continue;
                }
                int distance = candidate > index ? candidate - index : index - candidate;
                if (distance < bestDistance) {
                    bestDistance = distance;
                    nearest = candidate;
                }
            }
            if (nearest == -1) {
                return;
            }
            index = nearest;
        }

        gChsCurrentFontIndex = index;
        gChsCurrentFont = &(gChsFonts[index]);
        gChsEncoding = gChsCurrentFont->encoding;
    }

    int chsGetStringWidthImpl(const char* string)
    {
        if (string == nullptr || gChsCurrentFont == nullptr) {
            return 0;
        }

        int width = 0;
        const char* p = string;
        while (*p != '\0') {
            char32_t cp;
            int length = decodeNext(p, &cp);
            if (length <= 0) {
                break;
            }
            width += characterAdvance(cp);
            p += length;
        }
        return width;
    }

    int chsGetLineHeightImpl()
    {
        if (gChsCurrentFont == nullptr) {
            return 0;
        }
        return gChsCurrentFont->lineSpacing + gChsCurrentFont->maxHeight + gChsCurrentFont->heightOffset;
    }

    int chsGetCharacterWidthImpl(int ch)
    {
        if (gChsCurrentFont == nullptr) {
            return 0;
        }

        // Callers pass a single byte here. For a GBK lead byte the full
        // character is two bytes wide, so report the full-width spacing.
        unsigned char byte = static_cast<unsigned char>(ch);
        if (byte == ' ') {
            return gChsCurrentFont->wordSpacing;
        }
        return gChsCurrentFont->wordSpacing + gChsCurrentFont->letterSpacing;
    }

    int chsGetMonospacedStringWidthImpl(const char* string)
    {
        return chsGetStringWidthImpl(string);
    }

    int chsGetLetterSpacingImpl()
    {
        return gChsCurrentFont != nullptr ? gChsCurrentFont->letterSpacing : 0;
    }

    int chsGetBufferSizeImpl(const char* string)
    {
        return chsGetStringWidthImpl(string) * chsGetLineHeightImpl();
    }

    int chsGetMonospacedCharacterWidthImpl()
    {
        if (gChsCurrentFont == nullptr) {
            return 0;
        }
        return gChsCurrentFont->lineSpacing + gChsCurrentFont->maxHeight;
    }

    void chsDrawImpl(unsigned char* buf, const char* string, int length, int pitch, ColorWithFlags color)
    {
        if (buf == nullptr || string == nullptr || length <= 0 || pitch <= 0 || gChsCurrentFont == nullptr) {
            return;
        }

        // The original font managers render the shadow by recursing with the
        // shadowed flag cleared, then continue with the main glyphs.
        if ((color & DRAW_TEXT_FLAG_SHADOWED) != 0) {
            int flags = static_cast<int>(color) & ~0xFF & ~static_cast<int>(DRAW_TEXT_FLAG_SHADOWED);
            ColorWithFlags shadowColor = COLOR_BLACK | static_cast<DrawTextFlags>(flags);
            chsDrawImpl(buf + pitch + 1, string, length, pitch, shadowColor);
        }

        Color paletteIndex = static_cast<Color>(static_cast<int>(color) & 0xFF);
        Color* palette = _getColorBlendTable(paletteIndex);
        if (palette == nullptr) {
            return;
        }

        int lineHeight = chsGetLineHeightImpl();
        int x = 0;
        const char* p = string;
        while (*p != '\0') {
            char32_t cp;
            int charLength = decodeNext(p, &cp);
            if (charLength <= 0) {
                break;
            }
            p += charLength;

            if (cp == U'\n' || cp == U'\r') {
                continue;
            }

            int advance = characterAdvance(cp);
            if (x + advance > length) {
                break;
            }

            if (cp != U' ') {
                const ChsGlyph* glyph = getGlyph(cp);
                if (glyph != nullptr && !glyph->pixels.empty()) {
                    int glyphX = x + glyph->xOffset;
                    int glyphY = gChsCurrentFont->baseline + glyph->yOffset;
                    for (int row = 0; row < glyph->height; row++) {
                        int y = glyphY + row;
                        if (y < 0 || y >= lineHeight) {
                            continue;
                        }
                        for (int column = 0; column < glyph->width; column++) {
                            int px = glyphX + column;
                            if (px < 0 || px >= length) {
                                continue;
                            }
                            unsigned char alpha = glyph->pixels[static_cast<size_t>(row) * glyph->width + column];
                            if (alpha == 0) {
                                continue;
                            }
                            unsigned char* dest = buf + static_cast<size_t>(y) * pitch + px;
                            *dest = palette[((alpha / 26) << 8) + *dest];
                        }
                    }
                }
            }

            x += advance;
        }

        if ((color & DRAW_TEXT_FLAG_UNDERLINED) != 0) {
            int underlineY = std::min(lineHeight - 1, gChsCurrentFont->maxHeight - 1);
            unsigned char* underlinePtr = buf + static_cast<size_t>(underlineY) * pitch;
            for (int index = 0; index < x; index++) {
                *underlinePtr++ = static_cast<unsigned char>(static_cast<int>(color) & 0xFF);
            }
        }

        _freeColorBlendTable(paletteIndex);
    }

    bool isBreakOpportunity(char32_t cp, int charLength)
    {
        if (cp == U' ' || cp == U'-' || cp == U'\t') {
            return true;
        }
        // CJK ideographs, kana, hangul and full-width punctuation can be
        // broken after any character. Latin runs are kept together.
        if (charLength > 1 || (cp >= 0x2E80 && cp <= 0xFEFF) || (cp >= 0xFF00 && cp <= 0xFFEF)) {
            return true;
        }
        return false;
    }

} // namespace

// --- Public API -----------------------------------------------------------

int chsFontsInit()
{
    Config config;
    if (!configInit(&config)) {
        return -1;
    }

    if (!configRead(&config, kChsConfigPath, true)) {
        configFree(&config);
        return -1; // No Chinese font pack installed - stock behaviour.
    }

    int firstFont = -1;
    for (int index = 0; index < kChsFontCount; index++) {
        if (loadFont(index, &config) && firstFont == -1) {
            firstFont = index;
        }
    }
    configFree(&config);

    if (firstFont == -1) {
        debugPrint("CHS: font.ini found but no font could be loaded\n");
        releaseFonts();
        return -1;
    }

    gChsCurrentFontIndex = firstFont;
    gChsCurrentFont = &(gChsFonts[firstFont]);
    gChsEncoding = gChsCurrentFont->encoding;

    gChsFontManager = {
        kChsFontBase,
        kChsFontMax,
        chsSetCurrentImpl,
        chsDrawImpl,
        chsGetLineHeightImpl,
        chsGetStringWidthImpl,
        chsGetCharacterWidthImpl,
        chsGetMonospacedStringWidthImpl,
        chsGetLetterSpacingImpl,
        chsGetBufferSizeImpl,
        chsGetMonospacedCharacterWidthImpl,
        chsWordWrap,
    };

    if (fontManagerAdd(&gChsFontManager) != 0) {
        debugPrint("CHS: could not register font manager\n");
        releaseFonts();
        return -1;
    }

    gChsActive = true;
    debugPrint("CHS: Chinese TrueType fonts enabled (encoding %s)\n",
        gChsEncoding == ChsEncoding::Utf8 ? "UTF-8" : "GBK");
    return 0;
}

void chsFontsExit()
{
    if (!gChsActive) {
        return;
    }
    gChsActive = false;
    releaseFonts();
}

bool chsFontsActive()
{
    return gChsActive;
}

int chsCharLength(const char* text)
{
    char32_t cp;
    return decodeNext(text, &cp);
}

char32_t chsDecodeChar(const char* text, int* length)
{
    char32_t cp = 0;
    int consumed = decodeNext(text, &cp);
    if (length != nullptr) {
        *length = consumed;
    }
    return consumed == 0 ? 0 : cp;
}

std::u32string chsDecodeString(const char* text)
{
    std::u32string result;
    if (text == nullptr) {
        return result;
    }

    const char* p = text;
    while (*p != '\0') {
        char32_t cp;
        int consumed = decodeNext(p, &cp);
        if (consumed <= 0) {
            break;
        }
        if (cp == kChsKnob) {
            cp = 0x2022; // bullet
        }
        result.push_back(cp);
        p += consumed;
    }
    return result;
}

std::string chsEncodeString(const std::u32string& text)
{
    std::string result;
    for (char32_t cp : text) {
        if (gChsEncoding == ChsEncoding::Utf8) {
            if (cp < 0x80) {
                result.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            continue;
        }

        if (cp < 0x80) {
            result.push_back(static_cast<char>(cp));
            continue;
        }
        uint16_t gbk = chsUnicodeToGbk(cp);
        if (gbk != 0) {
            result.push_back(static_cast<char>(gbk >> 8));
            result.push_back(static_cast<char>(gbk & 0xFF));
        }
    }
    return result;
}

size_t chsBuildLine(const char* text, int maxWidth, char* out, size_t outSize)
{
    if (text == nullptr || out == nullptr || outSize == 0) {
        return 0;
    }

    out[0] = '\0';
    size_t used = 0;
    const char* p = text;

    while (*p != '\0') {
        char32_t cp;
        int charLength = decodeNext(p, &cp);
        if (charLength <= 0) {
            break;
        }
        if (used + static_cast<size_t>(charLength) + 1 > outSize) {
            break;
        }

        memcpy(out + used, p, static_cast<size_t>(charLength));
        used += static_cast<size_t>(charLength);
        out[used] = '\0';

        if (maxWidth > 0 && fontGetStringWidth(out) > maxWidth) {
            used -= static_cast<size_t>(charLength);
            out[used] = '\0';
            if (used == 0) {
                // A single character wider than the available space: keep it
                // anyway so the caller always makes progress.
                memcpy(out, p, static_cast<size_t>(charLength));
                used = static_cast<size_t>(charLength);
                out[used] = '\0';
                p += charLength;
            }
            break;
        }
        p += charLength;
    }

    return static_cast<size_t>(p - text);
}

int chsWordWrap(const char* string, int width, short* breakpoints, short* breakpointsLengthPtr)
{
    breakpoints[0] = 0;
    *breakpointsLengthPtr = 1;
    for (int index = 1; index < WORD_WRAP_MAX_COUNT; index++) {
        breakpoints[index] = -1;
    }

    if (string == nullptr || width <= 0) {
        breakpoints[1] = 0;
        *breakpointsLengthPtr = 2;
        return 0;
    }

    int length = static_cast<int>(strlen(string));
    if (chsGetStringWidthImpl(string) <= width) {
        breakpoints[1] = static_cast<short>(length);
        *breakpointsLengthPtr = 2;
        return 0;
    }

    int accum = 0;
    int lastBreak = -1;
    int position = 0;

    while (position < length) {
        char32_t cp;
        int charLength = decodeNext(string + position, &cp);
        if (charLength <= 0) {
            break;
        }

        int advance = characterAdvance(cp);
        int previousPosition = position;

        if (accum + advance > width) {
            if (*breakpointsLengthPtr == WORD_WRAP_MAX_COUNT) {
                return -1;
            }

            int breakAt = lastBreak > breakpoints[*breakpointsLengthPtr - 1] ? lastBreak : previousPosition;
            if (breakAt <= breakpoints[*breakpointsLengthPtr - 1]) {
                // Nothing fits on this line: consume one whole character.
                breakAt = previousPosition + charLength;
            }

            breakpoints[*breakpointsLengthPtr] = static_cast<short>(breakAt);
            *breakpointsLengthPtr += 1;
            position = breakAt;
            accum = 0;
            lastBreak = -1;
            continue;
        }

        accum += advance;
        position += charLength;

        if (isBreakOpportunity(cp, charLength) && !(gChsCurrentFont != nullptr && gChsCurrentFont->warpMode == 1 && cp != U' ')) {
            lastBreak = position;
        }
    }

    if (*breakpointsLengthPtr == WORD_WRAP_MAX_COUNT) {
        return -1;
    }

    breakpoints[*breakpointsLengthPtr] = static_cast<short>(length);
    *breakpointsLengthPtr += 1;
    return 0;
}

const unsigned char* chsFontData(int* size)
{
    if (!gChsActive || gChsCurrentFont == nullptr || gChsCurrentFont->fileBuffer.empty()) {
        if (size != nullptr) {
            *size = 0;
        }
        return nullptr;
    }
    if (size != nullptr) {
        *size = static_cast<int>(gChsCurrentFont->fileBuffer.size());
    }
    return gChsCurrentFont->fileBuffer.data();
}

} // namespace fallout

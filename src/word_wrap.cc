#include "word_wrap.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "text_font.h"

namespace fallout {

// CE/CHS: the engine calls wordWrap() from a dozen places (dialogue options,
// Pip-Boy, save/load, floating text, ...). Route it through the active font
// manager so CJK text is wrapped on character boundaries instead of bytes.
int wordWrap(const char* string, int width, short* breakpoints, short* breakpointsLengthPtr)
{
    if (fontWordWrap != nullptr) {
        return fontWordWrap(string, width, breakpoints, breakpointsLengthPtr);
    }
    return legacyWordWrap(string, width, breakpoints, breakpointsLengthPtr);
}

// 0x4BC6F0 word_wrap
int legacyWordWrap(const char* string, int width, short* breakpoints, short* breakpointsLengthPtr)
{
    breakpoints[0] = 0;
    *breakpointsLengthPtr = 1;

    for (int index = 1; index < WORD_WRAP_MAX_COUNT; index++) {
        breakpoints[index] = -1;
    }

    if (fontGetMonospacedCharacterWidth() > width) {
        return -1;
    }

    if (fontGetStringWidth(string) < width) {
        breakpoints[*breakpointsLengthPtr] = (short)strlen(string);
        *breakpointsLengthPtr += 1;
        return 0;
    }

    int gap = fontGetLetterSpacing();

    int accum = 0;
    const char* prevSpaceOrHyphen = nullptr;
    const char* pch = string;
    while (*pch != '\0') {
        accum += gap + fontGetCharacterWidth(*pch & 0xFF);
        if (accum <= width) {
            // NOTE: quests.txt #807 uses extended ascii.
            if (isspace(*pch & 0xFF) || *pch == '-') {
                prevSpaceOrHyphen = pch;
            }
        } else {
            if (*breakpointsLengthPtr == WORD_WRAP_MAX_COUNT) {
                return -1;
            }

            if (prevSpaceOrHyphen != nullptr) {
                // Word wrap.
                breakpoints[*breakpointsLengthPtr] = prevSpaceOrHyphen - string + 1;
                *breakpointsLengthPtr += 1;

                pch = prevSpaceOrHyphen;
            } else {
                // Character wrap.
                breakpoints[*breakpointsLengthPtr] = pch - string;
                *breakpointsLengthPtr += 1;

                pch--;
            }

            prevSpaceOrHyphen = nullptr;
            accum = 0;
        }
        pch++;
    }

    if (*breakpointsLengthPtr == WORD_WRAP_MAX_COUNT) {
        return -1;
    }

    breakpoints[*breakpointsLengthPtr] = pch - string + 1;
    *breakpointsLengthPtr += 1;

    return 0;
}

} // namespace fallout

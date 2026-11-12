#ifndef WORD_WRAP_H
#define WORD_WRAP_H

namespace fallout {

#define WORD_WRAP_MAX_COUNT (64)

// Dispatches to the active font manager's wordWrapProc when one is installed
// (Chinese fonts), otherwise to the original byte-based algorithm.
int wordWrap(const char* string, int width, short* breakpoints, short* breakpointsLengthPtr);

// The original single-byte implementation, kept for non-CJK fonts.
int legacyWordWrap(const char* string, int width, short* breakpoints, short* breakpointsLengthPtr);

} // namespace fallout

#endif /* WORD_WRAP_H */

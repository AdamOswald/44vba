/* Copyright  (C) 2010-2016 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (utf.h).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef _LIBRETRO_ENCODINGS_UTF_H
#define _LIBRETRO_ENCODINGS_UTF_H

#include <retro_common_api.h>

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>

RETRO_BEGIN_DECLS

enum CodePage {
  CODEPAGE_LOCAL = 0,   /* CP_ACP */
  CODEPAGE_UTF8 = 65001 /* CP_UTF8 */
};

size_t utf8_conv_utf32(uint32_t *out, size_t out_chars, const char *in,
                       size_t in_size);

bool utf16_conv_utf8(uint8_t *out, size_t *out_chars, const uint16_t *in,
                     size_t in_size);

size_t utf8len(const char *string);

size_t utf8cpy(char *d, size_t d_len, const char *s, size_t chars);

const char *utf8skip(const char *str, size_t chars);

uint32_t utf8_walk(const char **string);

bool utf16_to_char_string(const uint16_t *in, char *s, size_t len);

char *utf8_to_local_string_alloc(const char *str);

char *local_to_utf8_string_alloc(const char *str);

wchar_t *utf8_to_utf16_string_alloc(const char *str);

char *utf16_to_utf8_string_alloc(const wchar_t *str);

RETRO_END_DECLS

#endif

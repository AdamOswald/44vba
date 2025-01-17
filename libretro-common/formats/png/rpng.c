/* Copyright  (C) 2010-2016 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (rpng.c).
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GEKKO
#include <malloc.h>
#endif

#include <boolean.h>
#include <file/archive_file.h>
#include <formats/image.h>
#include <formats/rpng.h>

#include "rpng_internal.h"

enum png_ihdr_color_type {
  PNG_IHDR_COLOR_GRAY = 0,
  PNG_IHDR_COLOR_RGB = 2,
  PNG_IHDR_COLOR_PLT = 3,
  PNG_IHDR_COLOR_GRAY_ALPHA = 4,
  PNG_IHDR_COLOR_RGBA = 6
};

enum png_line_filter {
  PNG_FILTER_NONE = 0,
  PNG_FILTER_SUB,
  PNG_FILTER_UP,
  PNG_FILTER_AVERAGE,
  PNG_FILTER_PAETH
};

enum png_chunk_type {
  PNG_CHUNK_NOOP = 0,
  PNG_CHUNK_ERROR,
  PNG_CHUNK_IHDR,
  PNG_CHUNK_IDAT,
  PNG_CHUNK_PLTE,
  PNG_CHUNK_tRNS,
  PNG_CHUNK_IEND
};

struct adam7_pass {
  unsigned x;
  unsigned y;
  unsigned stride_x;
  unsigned stride_y;
};

struct idat_buffer {
  uint8_t *data;
  size_t size;
};

struct png_chunk {
  uint32_t size;
  char type[4];
  uint8_t *data;
};

struct rpng_process {
  bool inflate_initialized;
  bool adam7_pass_initialized;
  bool pass_initialized;
  uint32_t *data;
  uint32_t *palette;
  struct png_ihdr ihdr;
  uint8_t *prev_scanline;
  uint8_t *decoded_scanline;
  uint8_t *inflate_buf;
  size_t restore_buf_size;
  size_t adam7_restore_buf_size;
  size_t data_restore_buf_size;
  size_t inflate_buf_size;
  unsigned bpp;
  unsigned pitch;
  unsigned h;
  struct {
    unsigned width;
    unsigned height;
    size_t size;
    unsigned pos;
  } pass;
  void *stream;
  const struct file_archive_file_backend *stream_backend;
};

struct rpng {
  struct rpng_process *process;
  bool has_ihdr;
  bool has_idat;
  bool has_iend;
  bool has_plte;
  bool has_trns;
  struct idat_buffer idat_buf;
  struct png_ihdr ihdr;
  uint8_t *buff_data;
  uint32_t palette[256];
};

static INLINE uint32_t dword_be(const uint8_t *buf) {
  return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | (buf[3] << 0);
}

static enum png_chunk_type png_chunk_type(const struct png_chunk *chunk) {
  unsigned i;
  struct {
    const char *id;
    enum png_chunk_type type;
  } static const chunk_map[] = {
      {"IHDR", PNG_CHUNK_IHDR}, {"IDAT", PNG_CHUNK_IDAT},
      {"IEND", PNG_CHUNK_IEND}, {"PLTE", PNG_CHUNK_PLTE},
      {"tRNS", PNG_CHUNK_tRNS},
  };

  for (i = 0; i < ARRAY_SIZE(chunk_map); i++) {
    if (memcmp(chunk->type, chunk_map[i].id, 4) == 0)
      return chunk_map[i].type;
  }

  return PNG_CHUNK_NOOP;
}

static bool png_process_ihdr(struct png_ihdr *ihdr) {
  unsigned i;
  bool ret = true;

  switch (ihdr->color_type) {
  case PNG_IHDR_COLOR_RGB:
  case PNG_IHDR_COLOR_GRAY_ALPHA:
  case PNG_IHDR_COLOR_RGBA:
    if (ihdr->depth != 8 && ihdr->depth != 16)
      GOTO_END_ERROR();
    break;
  case PNG_IHDR_COLOR_GRAY: {
    static const unsigned valid_bpp[] = {1, 2, 4, 8, 16};
    bool correct_bpp = false;

    for (i = 0; i < ARRAY_SIZE(valid_bpp); i++) {
      if (valid_bpp[i] == ihdr->depth) {
        correct_bpp = true;
        break;
      }
    }

    if (!correct_bpp)
      GOTO_END_ERROR();
  } break;
  case PNG_IHDR_COLOR_PLT: {
    static const unsigned valid_bpp[] = {1, 2, 4, 8};
    bool correct_bpp = false;

    for (i = 0; i < ARRAY_SIZE(valid_bpp); i++) {
      if (valid_bpp[i] == ihdr->depth) {
        correct_bpp = true;
        break;
      }
    }

    if (!correct_bpp)
      GOTO_END_ERROR();
  } break;
  default:
    GOTO_END_ERROR();
  }

#ifdef RPNG_TEST
  fprintf(stderr,
          "IHDR: (%u x %u), bpc = %u, palette = %s, color = %s, alpha = %s, "
          "adam7 = %s.\n",
          ihdr->width, ihdr->height, ihdr->depth,
          (ihdr->color_type == PNG_IHDR_COLOR_PLT) ? "yes" : "no",
          (ihdr->color_type & PNG_IHDR_COLOR_RGB) ? "yes" : "no",
          (ihdr->color_type & PNG_IHDR_COLOR_GRAY_ALPHA) ? "yes" : "no",
          ihdr->interlace == 1 ? "yes" : "no");
#endif

  if (ihdr->compression != 0)
    GOTO_END_ERROR();

end:
  return ret;
}

static void png_reverse_filter_copy_line_rgb(uint32_t *data,
                                             const uint8_t *decoded,
                                             unsigned width, unsigned bpp) {
  unsigned i;

  bpp /= 8;

  for (i = 0; i < width; i++) {
    uint32_t r, g, b;

    r = *decoded;
    decoded += bpp;
    g = *decoded;
    decoded += bpp;
    b = *decoded;
    decoded += bpp;
    data[i] = (0xffu << 24) | (r << 16) | (g << 8) | (b << 0);
  }
}

static void png_reverse_filter_copy_line_rgba(uint32_t *data,
                                              const uint8_t *decoded,
                                              unsigned width, unsigned bpp) {
  unsigned i;

  bpp /= 8;

  for (i = 0; i < width; i++) {
    uint32_t r, g, b, a;
    r = *decoded;
    decoded += bpp;
    g = *decoded;
    decoded += bpp;
    b = *decoded;
    decoded += bpp;
    a = *decoded;
    decoded += bpp;
    data[i] = (a << 24) | (r << 16) | (g << 8) | (b << 0);
  }
}

static void png_reverse_filter_copy_line_bw(uint32_t *data,
                                            const uint8_t *decoded,
                                            unsigned width, unsigned depth) {
  unsigned i, bit;
  static const unsigned mul_table[] = {0, 0xff, 0x55, 0, 0x11, 0, 0, 0, 0x01};
  unsigned mul, mask;

  if (depth == 16) {
    for (i = 0; i < width; i++) {
      uint32_t val = decoded[i << 1];
      data[i] = (val * 0x010101) | (0xffu << 24);
    }
    return;
  }

  mul = mul_table[depth];
  mask = (1 << depth) - 1;
  bit = 0;

  for (i = 0; i < width; i++, bit += depth) {
    unsigned byte = bit >> 3;
    unsigned val = decoded[byte] >> (8 - depth - (bit & 7));

    val &= mask;
    val *= mul;
    data[i] = (val * 0x010101) | (0xffu << 24);
  }
}

static void png_reverse_filter_copy_line_gray_alpha(uint32_t *data,
                                                    const uint8_t *decoded,
                                                    unsigned width,
                                                    unsigned bpp) {
  unsigned i;

  bpp /= 8;

  for (i = 0; i < width; i++) {
    uint32_t gray, alpha;

    gray = *decoded;
    decoded += bpp;
    alpha = *decoded;
    decoded += bpp;

    data[i] = (gray * 0x010101) | (alpha << 24);
  }
}

static void png_reverse_filter_copy_line_plt(uint32_t *data,
                                             const uint8_t *decoded,
                                             unsigned width, unsigned depth,
                                             const uint32_t *palette) {
  switch (depth) {
  case 1: {
    unsigned w = width / 8;
    unsigned i;

    for (i = 0; i < w; i++, decoded++) {
      *data++ = palette[(*decoded >> 7) & 1];
      *data++ = palette[(*decoded >> 6) & 1];
      *data++ = palette[(*decoded >> 5) & 1];
      *data++ = palette[(*decoded >> 4) & 1];
      *data++ = palette[(*decoded >> 3) & 1];
      *data++ = palette[(*decoded >> 2) & 1];
      *data++ = palette[(*decoded >> 1) & 1];
      *data++ = palette[*decoded & 1];
    }

    switch (width & 7) {
    case 7:
      data[6] = palette[(*decoded >> 1) & 1];
    case 6:
      data[5] = palette[(*decoded >> 2) & 1];
    case 5:
      data[4] = palette[(*decoded >> 3) & 1];
    case 4:
      data[3] = palette[(*decoded >> 4) & 1];
    case 3:
      data[2] = palette[(*decoded >> 5) & 1];
    case 2:
      data[1] = palette[(*decoded >> 6) & 1];
    case 1:
      data[0] = palette[(*decoded >> 7) & 1];
      break;
    }
  } break;

  case 2: {
    unsigned w = width / 4;
    unsigned i;

    for (i = 0; i < w; i++, decoded++) {
      *data++ = palette[(*decoded >> 6) & 3];
      *data++ = palette[(*decoded >> 4) & 3];
      *data++ = palette[(*decoded >> 2) & 3];
      *data++ = palette[*decoded & 3];
    }

    switch (width & 3) {
    case 3:
      data[2] = palette[(*decoded >> 2) & 3];
    case 2:
      data[1] = palette[(*decoded >> 4) & 3];
    case 1:
      data[0] = palette[(*decoded >> 6) & 3];
      break;
    }
  } break;

  case 4: {
    unsigned w = width / 2;
    unsigned i;

    for (i = 0; i < w; i++, decoded++) {
      *data++ = palette[*decoded >> 4];
      *data++ = palette[*decoded & 0x0f];
    }

    if (width & 1) {
      *data = palette[*decoded >> 4];
    }
  } break;

  case 8: {
    unsigned i;

    for (i = 0; i < width; i++, decoded++, data++) {
      *data = palette[*decoded];
    }
  } break;
  }
}

static void png_pass_geom(const struct png_ihdr *ihdr, unsigned width,
                          unsigned height, unsigned *bpp_out,
                          unsigned *pitch_out, size_t *pass_size) {
  unsigned bpp;
  unsigned pitch;

  switch (ihdr->color_type) {
  case PNG_IHDR_COLOR_GRAY:
    bpp = (ihdr->depth + 7) / 8;
    pitch = (ihdr->width * ihdr->depth + 7) / 8;
    break;
  case PNG_IHDR_COLOR_RGB:
    bpp = (ihdr->depth * 3 + 7) / 8;
    pitch = (ihdr->width * ihdr->depth * 3 + 7) / 8;
    break;
  case PNG_IHDR_COLOR_PLT:
    bpp = (ihdr->depth + 7) / 8;
    pitch = (ihdr->width * ihdr->depth + 7) / 8;
    break;
  case PNG_IHDR_COLOR_GRAY_ALPHA:
    bpp = (ihdr->depth * 2 + 7) / 8;
    pitch = (ihdr->width * ihdr->depth * 2 + 7) / 8;
    break;
  case PNG_IHDR_COLOR_RGBA:
    bpp = (ihdr->depth * 4 + 7) / 8;
    pitch = (ihdr->width * ihdr->depth * 4 + 7) / 8;
    break;
  default:
    bpp = 0;
    pitch = 0;
    break;
  }

  if (pass_size)
    *pass_size = (pitch + 1) * ihdr->height;
  if (bpp_out)
    *bpp_out = bpp;
  if (pitch_out)
    *pitch_out = pitch;
}

static void png_reverse_filter_adam7_deinterlace_pass(
    uint32_t *data, const struct png_ihdr *ihdr, const uint32_t *input,
    unsigned pass_width, unsigned pass_height, const struct adam7_pass *pass) {
  unsigned x, y;

  data += pass->y * ihdr->width + pass->x;

  for (y = 0; y < pass_height;
       y++, data += ihdr->width * pass->stride_y, input += pass_width) {
    uint32_t *out = data;

    for (x = 0; x < pass_width; x++, out += pass->stride_x)
      *out = input[x];
  }
}

static void png_reverse_filter_deinit(struct rpng_process *pngp) {
  if (pngp->decoded_scanline)
    free(pngp->decoded_scanline);
  pngp->decoded_scanline = NULL;
  if (pngp->prev_scanline)
    free(pngp->prev_scanline);
  pngp->prev_scanline = NULL;

  pngp->pass_initialized = false;
  pngp->h = 0;
}

static const struct adam7_pass passes[] = {
    {0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
    {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2},
};

static int png_reverse_filter_init(const struct png_ihdr *ihdr,
                                   struct rpng_process *pngp) {
  size_t pass_size;

  if (!pngp->adam7_pass_initialized && ihdr->interlace) {
    if (ihdr->width <= passes[pngp->pass.pos].x ||
        ihdr->height <= passes[pngp->pass.pos].y) /* Empty pass */
      return 1;

    pngp->pass.width = (ihdr->width - passes[pngp->pass.pos].x +
                        passes[pngp->pass.pos].stride_x - 1) /
                       passes[pngp->pass.pos].stride_x;
    pngp->pass.height = (ihdr->height - passes[pngp->pass.pos].y +
                         passes[pngp->pass.pos].stride_y - 1) /
                        passes[pngp->pass.pos].stride_y;

    pngp->data = (uint32_t *)malloc(pngp->pass.width * pngp->pass.height *
                                    sizeof(uint32_t));

    if (!pngp->data)
      return -1;

    pngp->ihdr = *ihdr;
    pngp->ihdr.width = pngp->pass.width;
    pngp->ihdr.height = pngp->pass.height;

    png_pass_geom(&pngp->ihdr, pngp->pass.width, pngp->pass.height, NULL, NULL,
                  &pngp->pass.size);

    if (pngp->pass.size >
        pngp->stream_backend->stream_get_total_out(pngp->stream)) {
      free(pngp->data);
      return -1;
    }

    pngp->adam7_pass_initialized = true;

    return 0;
  }

  if (pngp->pass_initialized)
    return 0;

  png_pass_geom(ihdr, ihdr->width, ihdr->height, &pngp->bpp, &pngp->pitch,
                &pass_size);

  if (pngp->stream_backend->stream_get_total_out(pngp->stream) < pass_size)
    return -1;

  pngp->restore_buf_size = 0;
  pngp->data_restore_buf_size = 0;
  pngp->prev_scanline = (uint8_t *)calloc(1, pngp->pitch);
  pngp->decoded_scanline = (uint8_t *)calloc(1, pngp->pitch);

  if (!pngp->prev_scanline || !pngp->decoded_scanline)
    goto error;

  pngp->h = 0;
  pngp->pass_initialized = true;

  return 0;

error:
  png_reverse_filter_deinit(pngp);
  return -1;
}

static int png_reverse_filter_copy_line(uint32_t *data,
                                        const struct png_ihdr *ihdr,
                                        struct rpng_process *pngp,
                                        unsigned filter) {
  unsigned i;

  switch (filter) {
  case PNG_FILTER_NONE:
    memcpy(pngp->decoded_scanline, pngp->inflate_buf, pngp->pitch);
    break;
  case PNG_FILTER_SUB:
    for (i = 0; i < pngp->bpp; i++)
      pngp->decoded_scanline[i] = pngp->inflate_buf[i];
    for (i = pngp->bpp; i < pngp->pitch; i++)
      pngp->decoded_scanline[i] =
          pngp->decoded_scanline[i - pngp->bpp] + pngp->inflate_buf[i];
    break;
  case PNG_FILTER_UP:
    for (i = 0; i < pngp->pitch; i++)
      pngp->decoded_scanline[i] = pngp->prev_scanline[i] + pngp->inflate_buf[i];
    break;
  case PNG_FILTER_AVERAGE:
    for (i = 0; i < pngp->bpp; i++) {
      uint8_t avg = pngp->prev_scanline[i] >> 1;
      pngp->decoded_scanline[i] = avg + pngp->inflate_buf[i];
    }
    for (i = pngp->bpp; i < pngp->pitch; i++) {
      uint8_t avg =
          (pngp->decoded_scanline[i - pngp->bpp] + pngp->prev_scanline[i]) >> 1;
      pngp->decoded_scanline[i] = avg + pngp->inflate_buf[i];
    }
    break;
  case PNG_FILTER_PAETH:
    for (i = 0; i < pngp->bpp; i++)
      pngp->decoded_scanline[i] =
          paeth(0, pngp->prev_scanline[i], 0) + pngp->inflate_buf[i];
    for (i = pngp->bpp; i < pngp->pitch; i++)
      pngp->decoded_scanline[i] =
          paeth(pngp->decoded_scanline[i - pngp->bpp], pngp->prev_scanline[i],
                pngp->prev_scanline[i - pngp->bpp]) +
          pngp->inflate_buf[i];
    break;

  default:
    return IMAGE_PROCESS_ERROR_END;
  }

  switch (ihdr->color_type) {
  case PNG_IHDR_COLOR_GRAY:
    png_reverse_filter_copy_line_bw(data, pngp->decoded_scanline, ihdr->width,
                                    ihdr->depth);
    break;
  case PNG_IHDR_COLOR_RGB:
    png_reverse_filter_copy_line_rgb(data, pngp->decoded_scanline, ihdr->width,
                                     ihdr->depth);
    break;
  case PNG_IHDR_COLOR_PLT:
    png_reverse_filter_copy_line_plt(data, pngp->decoded_scanline, ihdr->width,
                                     ihdr->depth, pngp->palette);
    break;
  case PNG_IHDR_COLOR_GRAY_ALPHA:
    png_reverse_filter_copy_line_gray_alpha(data, pngp->decoded_scanline,
                                            ihdr->width, ihdr->depth);
    break;
  case PNG_IHDR_COLOR_RGBA:
    png_reverse_filter_copy_line_rgba(data, pngp->decoded_scanline, ihdr->width,
                                      ihdr->depth);
    break;
  }

  memcpy(pngp->prev_scanline, pngp->decoded_scanline, pngp->pitch);

  return IMAGE_PROCESS_NEXT;
}

static int png_reverse_filter_regular_iterate(uint32_t **data,
                                              const struct png_ihdr *ihdr,
                                              struct rpng_process *pngp) {
  int ret = IMAGE_PROCESS_END;

  if (pngp->h < ihdr->height) {
    unsigned filter = *pngp->inflate_buf++;
    pngp->restore_buf_size += 1;
    ret = png_reverse_filter_copy_line(*data, ihdr, pngp, filter);
  }

  if (ret == IMAGE_PROCESS_END || ret == IMAGE_PROCESS_ERROR_END)
    goto end;

  pngp->h++;
  pngp->inflate_buf += pngp->pitch;
  pngp->restore_buf_size += pngp->pitch;

  *data += ihdr->width;
  pngp->data_restore_buf_size += ihdr->width;

  return IMAGE_PROCESS_NEXT;

end:
  png_reverse_filter_deinit(pngp);

  pngp->inflate_buf -= pngp->restore_buf_size;
  *data -= pngp->data_restore_buf_size;
  pngp->data_restore_buf_size = 0;
  return ret;
}

static int png_reverse_filter_adam7_iterate(uint32_t **data_,
                                            const struct png_ihdr *ihdr,
                                            struct rpng_process *pngp) {
  int ret = 0;
  bool to_next = pngp->pass.pos < ARRAY_SIZE(passes);
  uint32_t *data = *data_;

  if (!to_next)
    return IMAGE_PROCESS_END;

  ret = png_reverse_filter_init(ihdr, pngp);

  if (ret == 1)
    return IMAGE_PROCESS_NEXT;
  if (ret == -1)
    return IMAGE_PROCESS_ERROR_END;

  if (png_reverse_filter_init(&pngp->ihdr, pngp) == -1)
    return IMAGE_PROCESS_ERROR;

  do {
    ret = png_reverse_filter_regular_iterate(&pngp->data, &pngp->ihdr, pngp);
  } while (ret == IMAGE_PROCESS_NEXT);

  if (ret == IMAGE_PROCESS_ERROR || ret == IMAGE_PROCESS_ERROR_END)
    return IMAGE_PROCESS_ERROR;

  pngp->inflate_buf += pngp->pass.size;
  pngp->adam7_restore_buf_size += pngp->pass.size;

  pngp->stream_backend->stream_decrement_total_out(pngp->stream,
                                                   pngp->pass.size);

  png_reverse_filter_adam7_deinterlace_pass(data, ihdr, pngp->data,
                                            pngp->pass.width, pngp->pass.height,
                                            &passes[pngp->pass.pos]);

  free(pngp->data);

  pngp->pass.width = 0;
  pngp->pass.height = 0;
  pngp->pass.size = 0;
  pngp->adam7_pass_initialized = false;

  return IMAGE_PROCESS_NEXT;
}

static int png_reverse_filter_adam7(uint32_t **data_,
                                    const struct png_ihdr *ihdr,
                                    struct rpng_process *pngp) {
  int ret = png_reverse_filter_adam7_iterate(data_, ihdr, pngp);

  switch (ret) {
  case IMAGE_PROCESS_ERROR_END:
  case IMAGE_PROCESS_END:
    break;
  case IMAGE_PROCESS_NEXT:
    pngp->pass.pos++;
    return 0;
  case IMAGE_PROCESS_ERROR:
    if (pngp->data)
      free(pngp->data);
    pngp->inflate_buf -= pngp->adam7_restore_buf_size;
    pngp->adam7_restore_buf_size = 0;
    return -1;
  }

  pngp->inflate_buf -= pngp->adam7_restore_buf_size;
  pngp->adam7_restore_buf_size = 0;
  return ret;
}

static int png_reverse_filter_iterate(rpng_t *rpng, uint32_t **data) {
  if (!rpng)
    return false;

  if (rpng->ihdr.interlace)
    return png_reverse_filter_adam7(data, &rpng->ihdr, rpng->process);

  return png_reverse_filter_regular_iterate(data, &rpng->ihdr, rpng->process);
}

static int rpng_load_image_argb_process_inflate_init(rpng_t *rpng,
                                                     uint32_t **data,
                                                     unsigned *width,
                                                     unsigned *height) {
  int zstatus;
  struct rpng_process *process = (struct rpng_process *)rpng->process;
  bool to_continue =
      (process->stream_backend->stream_get_avail_in(process->stream) > 0 &&
       process->stream_backend->stream_get_avail_out(process->stream) > 0);

  if (!to_continue)
    goto end;

  zstatus = process->stream_backend->stream_decompress_data_to_file_iterate(
      process->stream);

  switch (zstatus) {
  case 1:
    goto end;
  case -1:
    goto error;
  default:
    break;
  }

  return 0;

end:
  process->stream_backend->stream_free(process->stream);

  *width = rpng->ihdr.width;
  *height = rpng->ihdr.height;
#ifdef GEKKO
  /* we often use these in textures, make sure they're 32-byte aligned */
  *data = (uint32_t *)memalign(32, rpng->ihdr.width * rpng->ihdr.height *
                                       sizeof(uint32_t));
#else
  *data = (uint32_t *)malloc(rpng->ihdr.width * rpng->ihdr.height *
                             sizeof(uint32_t));
#endif
  if (!*data)
    goto false_end;

  process->adam7_restore_buf_size = 0;
  process->restore_buf_size = 0;
  process->palette = rpng->palette;

  if (rpng->ihdr.interlace != 1)
    if (png_reverse_filter_init(&rpng->ihdr, process) == -1)
      goto false_end;

  process->inflate_initialized = true;
  return 1;

error:
false_end:
  process->inflate_initialized = false;
  return -1;
}

static bool png_read_plte(uint8_t *buf, uint32_t *buffer, unsigned entries) {
  unsigned i;

  for (i = 0; i < entries; i++) {
    uint32_t r = buf[3 * i + 0];
    uint32_t g = buf[3 * i + 1];
    uint32_t b = buf[3 * i + 2];
    buffer[i] = (r << 16) | (g << 8) | (b << 0) | (0xffu << 24);
  }

  return true;
}

static bool png_read_trns(uint8_t *buf, uint32_t *palette, unsigned entries) {
  unsigned i;

  for (i = 0; i < entries; i++, buf++, palette++) {
    *palette = (*palette & 0x00ffffff) | *buf << 24;
  }

  return true;
}

bool png_realloc_idat(const struct png_chunk *chunk, struct idat_buffer *buf) {
  uint8_t *new_buffer = (uint8_t *)realloc(buf->data, buf->size + chunk->size);

  if (!new_buffer)
    return false;

  buf->data = new_buffer;
  return true;
}

static struct rpng_process *rpng_process_init(rpng_t *rpng, unsigned *width,
                                              unsigned *height) {
  uint8_t *inflate_buf = NULL;
  struct rpng_process *process =
      (struct rpng_process *)calloc(1, sizeof(*process));

  if (!process)
    return NULL;

  process->stream_backend = file_archive_get_default_file_backend();

  png_pass_geom(&rpng->ihdr, rpng->ihdr.width, rpng->ihdr.height, NULL, NULL,
                &process->inflate_buf_size);
  if (rpng->ihdr.interlace == 1) /* To be sure. */
    process->inflate_buf_size *= 2;

  process->stream = process->stream_backend->stream_new();

  if (!process->stream) {
    free(process);
    return NULL;
  }

  if (!process->stream_backend->stream_decompress_init(process->stream)) {
    free(process);
    return NULL;
  }

  inflate_buf = (uint8_t *)malloc(process->inflate_buf_size);
  if (!inflate_buf)
    goto error;

  process->inflate_buf = inflate_buf;
  process->stream_backend->stream_set(
      process->stream, rpng->idat_buf.size, process->inflate_buf_size,
      rpng->idat_buf.data, process->inflate_buf);

  return process;

error:
  if (process) {
    if (process->stream)
      process->stream_backend->stream_free(process->stream);
    free(process);
  }
  return NULL;
}

static bool read_chunk_header(uint8_t *buf, struct png_chunk *chunk) {
  unsigned i;
  uint8_t dword[4] = {0};

  for (i = 0; i < 4; i++)
    dword[i] = buf[i];

  chunk->size = dword_be(dword);

  for (i = 0; i < 4; i++)
    chunk->type[i] = buf[i + 4];

  return true;
}

static bool png_parse_ihdr(uint8_t *buf, struct png_ihdr *ihdr) {
  buf += 4 + 4;

  ihdr->width = dword_be(buf + 0);
  ihdr->height = dword_be(buf + 4);
  ihdr->depth = buf[8];
  ihdr->color_type = buf[9];
  ihdr->compression = buf[10];
  ihdr->filter = buf[11];
  ihdr->interlace = buf[12];

  if (ihdr->width == 0 || ihdr->height == 0)
    return false;

  return true;
}

bool rpng_iterate_image(rpng_t *rpng) {
  unsigned i;
  struct png_chunk chunk = {0};
  uint8_t *buf = (uint8_t *)rpng->buff_data;

  if (!read_chunk_header(buf, &chunk))
    return false;

  *buf += 8;

#if 0
    for (i = 0; i < 4; i++)
    {
        fprintf(stderr, "chunktype: %c\n", chunk.type[i]);
    }
#endif

  switch (png_chunk_type(&chunk)) {
  case PNG_CHUNK_NOOP:
  default:
    break;

  case PNG_CHUNK_ERROR:
    goto error;

  case PNG_CHUNK_IHDR:
    if (rpng->has_ihdr || rpng->has_idat || rpng->has_iend)
      goto error;

    if (chunk.size != 13)
      goto error;

    if (!png_parse_ihdr(buf, &rpng->ihdr))
      goto error;

    if (!png_process_ihdr(&rpng->ihdr))
      goto error;

    rpng->has_ihdr = true;
    break;

  case PNG_CHUNK_PLTE: {
    unsigned entries = chunk.size / 3;

    if (!rpng->has_ihdr || rpng->has_plte || rpng->has_iend || rpng->has_idat ||
        rpng->has_trns)
      goto error;

    if (chunk.size % 3)
      goto error;

    if (entries > 256)
      goto error;

    buf += 8;

    if (!png_read_plte(buf, rpng->palette, entries))
      goto error;

    rpng->has_plte = true;
  } break;

  case PNG_CHUNK_tRNS:
    if (rpng->has_idat)
      goto error;

    if (rpng->ihdr.color_type == PNG_IHDR_COLOR_PLT) {
      /* we should compare with the number of palette entries */
      if (chunk.size > 256)
        goto error;

      buf += 8;

      if (!png_read_trns(buf, rpng->palette, chunk.size))
        goto error;
    }
    /* TODO: support colorkey in grayscale and truecolor images */

    rpng->has_trns = true;
    break;

  case PNG_CHUNK_IDAT:
    if (!(rpng->has_ihdr) || rpng->has_iend ||
        (rpng->ihdr.color_type == PNG_IHDR_COLOR_PLT && !(rpng->has_plte)))
      goto error;

    if (!png_realloc_idat(&chunk, &rpng->idat_buf))
      goto error;

    buf += 8;

    for (i = 0; i < chunk.size; i++)
      rpng->idat_buf.data[i + rpng->idat_buf.size] = buf[i];

    rpng->idat_buf.size += chunk.size;

    rpng->has_idat = true;
    break;

  case PNG_CHUNK_IEND:
    if (!(rpng->has_ihdr) || !(rpng->has_idat))
      goto error;

    rpng->has_iend = true;
    goto error;
  }

  rpng->buff_data += chunk.size + 12;

  return true;

error:
  return false;
}

int rpng_process_image(rpng_t *rpng, void **_data, size_t size, unsigned *width,
                       unsigned *height) {
  uint32_t **data = (uint32_t **)_data;

  (void)size;

  if (!rpng->process) {
    struct rpng_process *process = rpng_process_init(rpng, width, height);

    if (!process)
      goto error;

    rpng->process = process;
    return IMAGE_PROCESS_NEXT;
  }

  if (!rpng->process->inflate_initialized) {
    if (rpng_load_image_argb_process_inflate_init(rpng, data, width, height) ==
        -1)
      goto error;
    return IMAGE_PROCESS_NEXT;
  }

  return png_reverse_filter_iterate(rpng, data);

error:
  if (rpng->process) {
    if (rpng->process->inflate_buf)
      free(rpng->process->inflate_buf);
    if (rpng->process->stream)
      rpng->process->stream_backend->stream_free(rpng->process->stream);
    free(rpng->process);
  }
  return IMAGE_PROCESS_ERROR;
}

void rpng_free(rpng_t *rpng) {
  if (!rpng)
    return;

  if (rpng->idat_buf.data)
    free(rpng->idat_buf.data);
  if (rpng->process) {
    if (rpng->process->inflate_buf)
      free(rpng->process->inflate_buf);
    if (rpng->process->stream) {
      if (rpng->process->stream_backend)
        rpng->process->stream_backend->stream_free(rpng->process->stream);
      free(rpng->process->stream);
    }
    free(rpng->process);
  }

  free(rpng);
}

bool rpng_start(rpng_t *rpng) {
  unsigned i;
  char header[8] = {0};

  if (!rpng)
    return false;

  for (i = 0; i < 8; i++)
    header[i] = rpng->buff_data[i];

  if (memcmp(header, png_magic, sizeof(png_magic)) != 0)
    return false;

  rpng->buff_data += 8;

  return true;
}

bool rpng_is_valid(rpng_t *rpng) {
  if (!rpng)
    return false;

  if (rpng->has_ihdr)
    return true;
  if (rpng->has_idat)
    return true;
  if (rpng->has_iend)
    return true;
  return false;
}

bool rpng_set_buf_ptr(rpng_t *rpng, void *data) {
  if (!rpng)
    return false;

  rpng->buff_data = (uint8_t *)data;

  return true;
}

rpng_t *rpng_alloc(void) {
  rpng_t *rpng = (rpng_t *)calloc(1, sizeof(rpng_t));
  if (!rpng)
    return NULL;
  return rpng;
}

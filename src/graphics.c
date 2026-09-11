/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#include "graphics.h"
#include "tigt.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(TIGT_HAVE_LIBCACA) && TIGT_HAVE_LIBCACA
#include <caca.h>
#endif

#define GRAPHICS_MAX_OUTPUT 4096u

static bool
valid_bitmap(const uint32_t *pixels, uint16_t width, uint16_t height,
             uint8_t pixel_width, uint16_t output_width, uint16_t output_height,
             bool themed)
{
    if (pixels == NULL || width == 0 || width > 640 || height == 0 || height > 200 ||
        (pixel_width != 1 && pixel_width != 2) || width % pixel_width != 0 ||
        output_width == 0 || output_width > GRAPHICS_MAX_OUTPUT ||
        output_height == 0 || output_height > GRAPHICS_MAX_OUTPUT)
        return false;
    if (!themed)
        return true;
    uint32_t colors[3];
    unsigned colors_count = 0;
    const size_t count = (size_t) width * height;
    for (size_t i = 0; i < count; i++) {
        const uint32_t rgb = pixels[i] & 0xffffffu;
        const unsigned gray = rgb & 255u;
        if (rgb != 0 && (gray <= 128 || rgb != gray * 0x010101u))
            return false;
        unsigned color = 0;
        while (color < colors_count && colors[color] != rgb)
            color++;
        if (color == colors_count) {
            if (colors_count == 3)
                return false;
            colors[colors_count++] = rgb;
        }
    }
    return true;
}

#if defined(TIGT_HAVE_LIBCACA) && TIGT_HAVE_LIBCACA

struct tigt_ascii {
    caca_canvas_t *canvas;
    caca_dither_t *dither;
    tigt_ascii_cell *cells;
    size_t capacity;
    uint32_t *source_pixels;
    size_t source_capacity;
    uint16_t width;
    uint16_t height;
};

bool
tigt_ascii_available(void)
{
    return true;
}

tigt_ascii *
tigt_ascii_create(void)
{
    tigt_ascii *ascii = calloc(1, sizeof(*ascii));
    if (ascii == NULL)
        return NULL;
    ascii->canvas = caca_create_canvas(0, 0);
    if (ascii->canvas == NULL) {
        const int error = errno;
        free(ascii);
        errno = error;
        return NULL;
    }
    caca_disable_dirty_rect(ascii->canvas);
    return ascii;
}

void
tigt_ascii_destroy(tigt_ascii *ascii)
{
    if (ascii == NULL)
        return;
    if (ascii->dither != NULL)
        caca_free_dither(ascii->dither);
    caca_free_canvas(ascii->canvas);
    free(ascii->cells);
    free(ascii->source_pixels);
    free(ascii);
}

static caca_dither_t *
create_dither(uint16_t width, uint16_t height)
{
    caca_dither_t *dither = caca_create_dither(32, width, height, width * 4,
                                             0xff0000u, 0x00ff00u, 0x0000ffu, 0);
    if (dither == NULL)
        return NULL;
    /* Deterministic conversion avoids temporal noise and error diffusion
     * inventing bright emphasis in an otherwise normal-intensity source.
     */
    if (caca_set_dither_charset(dither, "ascii") < 0 ||
        caca_set_dither_antialias(dither, "prefilter") < 0 ||
        caca_set_dither_algorithm(dither, "none") < 0) {
        const int error = errno;
        caca_free_dither(dither);
        errno = error;
        return NULL;
    }
    return dither;
}

int
tigt_ascii_render(tigt_ascii *ascii, const uint32_t *pixels,
                  uint16_t width, uint16_t height, uint8_t pixel_width,
                  uint16_t columns, uint16_t rows, bool themed,
                  const tigt_ascii_cell **cells)
{
    if (cells == NULL)
        return TIGT_ERROR_ARGUMENT;
    *cells = NULL;
    if (ascii == NULL ||
        !valid_bitmap(pixels, width, height, pixel_width, columns, rows, themed))
        return TIGT_ERROR_ARGUMENT;

    const uint16_t logical_width = width / pixel_width;
    if (themed || pixel_width != 1) {
        const size_t pixel_count = (size_t) logical_width * height;
        if (pixel_count > ascii->source_capacity) {
            uint32_t *buffer = realloc(ascii->source_pixels, pixel_count * sizeof(*buffer));
            if (buffer == NULL)
                return TIGT_ERROR_SYSTEM;
            ascii->source_pixels = buffer;
            ascii->source_capacity = pixel_count;
        }
        for (uint16_t y = 0; y < height; y++) {
            for (uint16_t x = 0; x < logical_width; x++) {
                const uint32_t rgb = pixels[(size_t) y * width + x * pixel_width] & 0xffffffu;
                ascii->source_pixels[(size_t) y * logical_width + x] =
                    !themed || rgb == 0 || rgb == 0xffffffu ? rgb : 0xaaaaaau;
            }
        }
        pixels = ascii->source_pixels;
    }
    const size_t count = (size_t) columns * rows;
    if (count > ascii->capacity) {
        tigt_ascii_cell *buffer = realloc(ascii->cells, count * sizeof(*buffer));
        if (buffer == NULL)
            return TIGT_ERROR_SYSTEM;
        ascii->cells = buffer;
        ascii->capacity = count;
    }
    if (caca_get_canvas_width(ascii->canvas) != columns ||
        caca_get_canvas_height(ascii->canvas) != rows) {
        if (caca_set_canvas_size(ascii->canvas, columns, rows) < 0)
            return TIGT_ERROR_SYSTEM;
    }
    if (ascii->dither == NULL || ascii->width != logical_width || ascii->height != height) {
        caca_dither_t *dither = create_dither(logical_width, height);
        if (dither == NULL)
            return TIGT_ERROR_SYSTEM;
        if (ascii->dither != NULL)
            caca_free_dither(ascii->dither);
        ascii->dither = dither;
        ascii->width = logical_width;
        ascii->height = height;
    }
    if (caca_set_dither_color(ascii->dither, themed ? "fullgray" : "full16") < 0 ||
        caca_dither_bitmap(ascii->canvas, 0, 0, columns, rows, ascii->dither, pixels) < 0)
        return TIGT_ERROR_SYSTEM;

    const uint32_t *chars = caca_get_canvas_chars(ascii->canvas);
    const uint32_t *attrs = caca_get_canvas_attrs(ascii->canvas);
    for (size_t i = 0; i < count; i++) {
        uint8_t foreground = caca_attr_to_ansi_fg(attrs[i]);
        uint8_t background = caca_attr_to_ansi_bg(attrs[i]);
        /* fullgray also permits dark gray (8). Its brightness is halfway
         * between 0 and 7; pick the endpoint that retains contrast with the
         * other color. Leave normal 7 and bright 15 distinctly unchanged.
         */
        if (themed) {
            if (foreground == 8)
                foreground = background == 7 ? 0 : 7;
            if (background == 8)
                background = foreground == 7 ? 0 : 7;
        }
        if (chars[i] < 32 || chars[i] > 126 || foreground > 15 || background > 15 ||
            (themed && ((foreground != 0 && foreground != 7 && foreground != 15) ||
                        (background != 0 && background != 7 && background != 15)))) {
            errno = EILSEQ;
            return TIGT_ERROR_SYSTEM;
        }
        ascii->cells[i] = (tigt_ascii_cell) {(uint8_t) chars[i], foreground, background};
    }
    *cells = ascii->cells;
    return TIGT_OK;
}

#else

bool
tigt_ascii_available(void)
{
    return false;
}

tigt_ascii *
tigt_ascii_create(void)
{
    errno = ENOSYS;
    return NULL;
}

void
tigt_ascii_destroy(tigt_ascii *ascii)
{
    (void) ascii;
}

int
tigt_ascii_render(tigt_ascii *ascii, const uint32_t *pixels,
                  uint16_t width, uint16_t height, uint8_t pixel_width,
                  uint16_t columns, uint16_t rows, bool themed,
                  const tigt_ascii_cell **cells)
{
    (void) ascii;
    (void) pixels;
    (void) width;
    (void) height;
    (void) pixel_width;
    (void) columns;
    (void) rows;
    (void) themed;
    if (cells != NULL)
        *cells = NULL;
    errno = ENOSYS;
    return TIGT_ERROR_TERMINAL;
}

#endif

/* Bounded, reusable storage for the renderer's serialized sixel calls. Keep
 * large buffers out of its thread stack and avoid per-frame heap ownership.
 */
struct color_bin {
    uint32_t count;
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    uint8_t palette;
};

struct color_box {
    unsigned first;
    unsigned count;
    uint32_t weight;
    unsigned range;
    unsigned shift;
};

static struct {
    struct color_bin histogram[32768];
    uint16_t bins[32768];
    uint16_t hash[512];
    uint32_t palette[256];
    uint8_t indices[640 * 200];
    uint8_t planes[256][GRAPHICS_MAX_OUTPUT];
} sixel;

static unsigned
color_bin_index(uint32_t rgb)
{
    return ((rgb >> 9) & 0x7c00u) | ((rgb >> 6) & 0x03e0u) | ((rgb >> 3) & 0x001fu);
}

static void
measure_box(struct color_box *box)
{
    unsigned minimum[3] = {31, 31, 31}, maximum[3] = {0, 0, 0};
    box->weight = 0;
    for (unsigned i = box->first; i < box->first + box->count; i++) {
        const unsigned bin = sixel.bins[i];
        box->weight += sixel.histogram[bin].count;
        for (unsigned channel = 0; channel < 3; channel++) {
            const unsigned value = (bin >> (channel * 5)) & 31u;
            if (value < minimum[channel])
                minimum[channel] = value;
            if (value > maximum[channel])
                maximum[channel] = value;
        }
    }
    box->range = 0;
    box->shift = 0;
    for (unsigned channel = 0; channel < 3; channel++) {
        const unsigned range = maximum[channel] - minimum[channel];
        if (range > box->range) {
            box->range = range;
            box->shift = channel * 5;
        }
    }
}

static int
compare_component(const void *left, const void *right, unsigned shift)
{
    const unsigned a = *(const uint16_t *) left, b = *(const uint16_t *) right;
    const int difference = (int) ((a >> shift) & 31u) - (int) ((b >> shift) & 31u);
    return difference != 0 ? difference : (int) a - (int) b;
}

static int
compare_red(const void *left, const void *right)
{
    return compare_component(left, right, 10);
}

static int
compare_green(const void *left, const void *right)
{
    return compare_component(left, right, 5);
}

static int
compare_blue(const void *left, const void *right)
{
    return compare_component(left, right, 0);
}

static unsigned
reduce_palette(const uint32_t *pixels, size_t count)
{
    memset(sixel.histogram, 0, sizeof(sixel.histogram));
    for (size_t i = 0; i < count; i++) {
        const uint32_t rgb = pixels[i];
        struct color_bin *bin = &sixel.histogram[color_bin_index(rgb)];
        bin->count++;
        bin->red += (rgb >> 16) & 255u;
        bin->green += (rgb >> 8) & 255u;
        bin->blue += rgb & 255u;
    }
    unsigned bins_count = 0;
    for (unsigned i = 0; i < 32768; i++) {
        if (sixel.histogram[i].count != 0)
            sixel.bins[bins_count++] = (uint16_t) i;
    }

    /* Weighted median-cut: split the most populated wide box along its
     * widest channel, then use the original 8-bit samples for centroids.
     * Five-bit histogram bins bound sorting work independently of pixel
     * count. Exact palettes bypass this reduction entirely.
     */
    struct color_box boxes[256];
    boxes[0] = (struct color_box) {.first = 0, .count = bins_count};
    measure_box(&boxes[0]);
    unsigned colors_count = 1;
    while (colors_count < 256) {
        unsigned selected = 0;
        uint64_t best_score = 0;
        for (unsigned i = 0; i < colors_count; i++) {
            const uint64_t score = (uint64_t) boxes[i].range * boxes[i].weight;
            if (boxes[i].count > 1 && score > best_score) {
                selected = i;
                best_score = score;
            }
        }
        if (best_score == 0)
            break;
        struct color_box *box = &boxes[selected];
        int (*compare)(const void *, const void *) = box->shift == 10 ? compare_red :
                                                    box->shift == 5 ? compare_green : compare_blue;
        qsort(sixel.bins + box->first, box->count, sizeof(sixel.bins[0]), compare);
        const unsigned end = box->first + box->count;
        unsigned split = box->first;
        uint32_t weight = 0;
        do {
            weight += sixel.histogram[sixel.bins[split++]].count;
        } while (split < end - 1 && weight < (box->weight + 1) / 2);
        boxes[colors_count] = (struct color_box) {.first = split, .count = end - split};
        box->count = split - box->first;
        measure_box(box);
        measure_box(&boxes[colors_count]);
        colors_count++;
    }

    for (unsigned color = 0; color < colors_count; color++) {
        const struct color_box *box = &boxes[color];
        uint32_t red = 0, green = 0, blue = 0;
        for (unsigned i = box->first; i < box->first + box->count; i++) {
            struct color_bin *bin = &sixel.histogram[sixel.bins[i]];
            red += bin->red;
            green += bin->green;
            blue += bin->blue;
            bin->palette = (uint8_t) color;
        }
        red = (red + box->weight / 2) / box->weight;
        green = (green + box->weight / 2) / box->weight;
        blue = (blue + box->weight / 2) / box->weight;
        sixel.palette[color] = (red << 16) | (green << 8) | blue;
    }
    for (size_t i = 0; i < count; i++)
        sixel.indices[i] = sixel.histogram[color_bin_index(pixels[i])].palette;
    return colors_count;
}

static unsigned
prepare_palette(const uint32_t *pixels, size_t count)
{
    memset(sixel.hash, 0, sizeof(sixel.hash));
    unsigned colors_count = 0;
    for (size_t i = 0; i < count; i++) {
        const uint32_t rgb = pixels[i] & 0xffffffu;
        unsigned slot = (rgb * UINT32_C(2654435761)) >> 23;
        while (sixel.hash[slot] != 0 && sixel.palette[sixel.hash[slot] - 1] != rgb)
            slot = (slot + 1) & 511u;
        if (sixel.hash[slot] == 0) {
            if (colors_count == 256)
                return reduce_palette(pixels, count);
            sixel.palette[colors_count++] = rgb;
            sixel.hash[slot] = (uint16_t) colors_count;
        }
        sixel.indices[i] = (uint8_t) (sixel.hash[slot] - 1);
    }
    return colors_count;
}

/* Buffer protocol bytes ourselves: a run incurs neither a formatted-I/O call
 * nor stream locking per pixel.
 */
struct sixel_writer {
    FILE *output;
    size_t used;
    bool failed;
    char buffer[4096];
};

static void
sixel_flush(struct sixel_writer *writer)
{
    if (!writer->failed && writer->used != 0 &&
        fwrite(writer->buffer, 1, writer->used, writer->output) != writer->used)
        writer->failed = true;
    writer->used = 0;
}

static void
sixel_byte(struct sixel_writer *writer, char byte)
{
    if (writer->failed)
        return;
    if (writer->used == sizeof(writer->buffer)) {
        sixel_flush(writer);
        if (writer->failed)
            return;
    }
    writer->buffer[writer->used++] = byte;
}

static void
sixel_string(struct sixel_writer *writer, const char *text)
{
    while (*text != '\0')
        sixel_byte(writer, *text++);
}

static void
sixel_number(struct sixel_writer *writer, unsigned value)
{
    char digits[10];
    unsigned count = 0;
    do {
        digits[count++] = (char) ('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0)
        sixel_byte(writer, digits[--count]);
}

static void
sixel_run(struct sixel_writer *writer, uint8_t mask, unsigned length)
{
    if (length > 3) {
        sixel_byte(writer, '!');
        sixel_number(writer, length);
        sixel_byte(writer, (char) ('?' + mask));
    } else {
        while (length-- != 0)
            sixel_byte(writer, (char) ('?' + mask));
    }
}

int
tigt_sixel_write(FILE *output, const uint32_t *pixels,
                 uint16_t width, uint16_t height, uint8_t pixel_width,
                 uint16_t output_width, uint16_t output_height)
{
    if (output == NULL ||
        !valid_bitmap(pixels, width, height, pixel_width, output_width, output_height, false))
        return TIGT_ERROR_ARGUMENT;
    if (ferror(output))
        return TIGT_ERROR_SYSTEM;

    struct sixel_writer writer = {.output = output};
    const unsigned colors_count = prepare_palette(pixels, (size_t) width * height);
    uint16_t source_x[GRAPHICS_MAX_OUTPUT];
    for (unsigned x = 0; x < output_width; x++)
        source_x[x] = (uint16_t) (((uint32_t) x * (width / pixel_width) / output_width) * pixel_width);

    /* P2=1 leaves unpainted pixels untouched; every in-bounds pixel below is
     * explicitly painted, while padding in the final band is not touched.
     */
    sixel_string(&writer, "\033P0;1;0q\"1;1;");
    sixel_number(&writer, output_width);
    sixel_byte(&writer, ';');
    sixel_number(&writer, output_height);
    for (unsigned color = 0; color < colors_count; color++) {
        const uint32_t rgb = sixel.palette[color];
        sixel_byte(&writer, '#');
        sixel_number(&writer, color);
        sixel_string(&writer, ";2;");
        sixel_number(&writer, (((rgb >> 16) & 255u) * 100u + 127u) / 255u);
        sixel_byte(&writer, ';');
        sixel_number(&writer, (((rgb >> 8) & 255u) * 100u + 127u) / 255u);
        sixel_byte(&writer, ';');
        sixel_number(&writer, ((rgb & 255u) * 100u + 127u) / 255u);
    }

    for (unsigned y = 0; y < output_height && !writer.failed; y += 6) {
        uint16_t ends[256] = {0};
        for (unsigned bit = 0; bit < 6 && y + bit < output_height; bit++) {
            const unsigned source_y = (uint32_t) (y + bit) * height / output_height;
            const uint8_t *row = sixel.indices + (size_t) source_y * width;
            for (unsigned x = 0; x < output_width; x++) {
                const unsigned color = row[source_x[x]];
                if (ends[color] == 0)
                    memset(sixel.planes[color], 0, output_width);
                sixel.planes[color][x] |= (uint8_t) (1u << bit);
                if (x + 1 > ends[color])
                    ends[color] = (uint16_t) (x + 1);
            }
        }
        bool first = true;
        for (unsigned color = 0; color < colors_count && !writer.failed; color++) {
            if (ends[color] == 0)
                continue;
            if (!first)
                sixel_byte(&writer, '$');
            first = false;
            sixel_byte(&writer, '#');
            sixel_number(&writer, color);
            const uint8_t *plane = sixel.planes[color];
            const unsigned end = ends[color];
            for (unsigned x = 0; x < end;) {
                unsigned next = x + 1;
                while (next < end && plane[next] == plane[x])
                    next++;
                sixel_run(&writer, plane[x], next - x);
                x = next;
            }
        }
        if (y + 6 < output_height)
            sixel_byte(&writer, '-');
    }
    sixel_string(&writer, "\033\\");
    sixel_flush(&writer);
    if (writer.failed || fflush(output) == EOF || ferror(output))
        return TIGT_ERROR_SYSTEM;
    return TIGT_OK;
}

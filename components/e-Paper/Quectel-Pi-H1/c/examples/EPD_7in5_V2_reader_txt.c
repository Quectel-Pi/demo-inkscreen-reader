// examples/EPD_7in5_V2_reader_txt.c
#define _DEFAULT_SOURCE  // Must be defined before including header files to enable file type constants like DT_REG
#include "EPD_7in5_V2.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include "GUI_BMPfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <lgpio.h>
#include <sys/ioctl.h>
#include "DEV_Config.h"
#include <sys/types.h>  // Add this header to define DT_REG
#include <poll.h>
#include <ctype.h>
#include <iconv.h>

// Define custom screen off and on signals
#define CUSTOM_SCREEN_OFF_BTN BTN_LEFT
#define CUSTOM_SCREEN_ON_BTN BTN_RIGHT

#define BOOK_PATH "./books"
#define MAX_BOOK_SIZE (4* 1024 * 1024) // 4MB
#define MAX_BOOKS 20
#define MAX_HISTORY 500 // Record up to 500 page history entries

// Partial refresh area definitions
#define HEADER_HEIGHT   30
#define FOOTER_HEIGHT   30  // Increase footer height to provide space for page numbers

#define CONTENT_Y_START   (HEADER_HEIGHT + 5)  // Reduce margin at top of content area
#define CONTENT_X_MARGIN  10 // Left/right margin for text content
#define FOOTER_Y_START    (EPD_7IN5_V2_HEIGHT - FOOTER_HEIGHT)

// Text layout tuning: 3pt before/after paragraph gap and 1.2x line height.
#define PARAGRAPH_SPACING_PX 2

// Chapter break: when a paragraph starts with "第X章/节/回", force a new page.
#define CHAPTER_BREAK_MARKER '\f'

// Font macros: change these in one place to control typography globally.
#define BODY_EN_FONT Font20
#define BODY_CN_FONT Font12CN


// Function declarations
void safe_truncate_filename(char* dest, const char* src, size_t dest_size);
char* process_text_content(const char* raw_text, size_t raw_size);
void calculate_page_info();
int get_current_page_index(size_t offset);
int find_eye_control_device();
void init_eye_control_device();
void enter_screen_off_mode();
void exit_screen_off_mode();

// Screen-off related definitions
static int screen_off = 0;  // Whether currently in screen-off state
// Add anti-flicker variable
static int anti_flicker_until = 0;  // Unix timestamp until which anti-flicker is active

static char current_file[2048] = {0};  // Reasonable size for file path
static UBYTE *g_frame_buffer = NULL;
static UBYTE *g_prev_frame_buffer = NULL; // Used to compare differences between frames for implementing partial refresh
static int key1_fd = -1; // event3: next page / long press: next book
static int key2_fd = -1; // event1: prev page / long press: prev book
static int eye_key_fd = -1; // New: eye_page_turner virtual device
static int first_display_done = 0;
static int book_changed = 0;  // Flag to mark whether book has changed
static int header_drawn = 0;  // New: flag to mark if Header area has been drawn
// Multi-book support
static char book_list[MAX_BOOKS][2048];  // Reasonable size for file path
static int book_count = 0;
static int current_book_index = 0;

// Global text
static char* g_full_text = NULL;
static size_t g_text_size = 0;
// New: Processed plain text content with extra line breaks removed
static char* g_processed_text = NULL;
static size_t g_processed_text_size = 0;

// Current page starting offset (in bytes)
static size_t g_current_char_offset = 0;
// Start offset of the page currently shown on screen
static size_t g_displayed_page_offset = 0;

// History stack: record starting offset of each page (for precise backward navigation)
static size_t history_stack[MAX_HISTORY];
static int history_top = -1;

// Flag to mark if book title needs redrawing
static int title_drawn = 0;

// New: Used for accurate calculation of current page number
static size_t *page_offsets = NULL;  // Store starting offset of each page
static int total_pages = 0;          // Total number of pages
static int current_page_index = 0;


const char* get_ext(const char* filename) {
    const char* dot = strrchr(filename, '.');
    return (dot && dot != filename) ? dot + 1 : "";
}

// Function: Find input device named eye_page_turner
int find_eye_control_device() {
    char name[256] = {0};
    int fd;
    int i;
    char fname[64];  // Device file name template
    
    // Iterate through /dev/input/event* devices
    for (i = 0; i < 32; i++) {
        sprintf(fname, "/dev/input/event%d", i);
        fd = open(fname, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            // Read device name
            ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
            if (strstr(name, "eye_page_turner")) {
                printf("Found eye control device: %s (%s)\n", fname, name);
                return fd;
            }
            close(fd);
        }
    }
    return -1;
}

// Initialize virtual device with retry mechanism
void init_eye_control_device() {
    int attempts = 0;
    const int max_attempts = 5; // Try 5 times with 1 second intervals
    
    printf("Waiting for eye control device...\n");
    
    while (attempts < max_attempts) {
        eye_key_fd = find_eye_control_device();
        if (eye_key_fd >= 0) {
            printf("Successfully connected to eye control device!\n");
            return;
        }
        
        printf("Attempt %d/%d: Eye control device not found, waiting...\n", attempts+1, max_attempts);
        sleep(1); // Wait for 1 second before retrying
        attempts++;
    }
    
    printf("Warning: Failed to connect to eye control device after %d attempts\n", max_attempts);
}

// Add: UTF-8 character length detection function
static inline int utf8_char_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// Add: GB2312 character length detection function
static inline int gb2312_char_len(const char* text, size_t size, size_t pos) {
    if (pos >= size) return 1;
    unsigned char c = (unsigned char)text[pos];
    // GB2312 specification: first byte ≥0x80 and second byte ≥0x40
    if (c >= 0x80 && pos + 1 < size && (unsigned char)text[pos + 1] >= 0x40) {
        return 2;
    }
    return 1;
}

// Add: File encoding detection function
static int detect_file_encoding(const char* data, size_t size) {
    // Check BOM marker
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        return 1; // UTF-8 with BOM
    }

    // Check for obvious GB2312 characteristics (double-byte characters)
    for (size_t i = 0; i < size && i < 1024; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c >= 0x80) {
            // Check if it conforms to GB2312 specification: first byte ≥0x80 and second byte ≥0x40
            if (i + 1 < size && (unsigned char)data[i+1] >= 0x40) {
                return 0; // Likely GB2312
            }
            // If it's UTF-8, it should conform to UTF-8 encoding rules
            else if ((c & 0xE0) == 0xC0 && i + 1 < size) {
                return 1; // UTF-8
            }
            else if ((c & 0xF0) == 0xE0 && i + 2 < size) {
                return 1; // UTF-8
            }
            else if ((c & 0xF8) == 0xF0 && i + 3 < size) {
                return 1; // UTF-8
            }
        }
    }

    return 1; // Default to UTF-8 (English text)
}

// Modify: Add UTF-8 character length detection helper function, keeping interface consistent
static inline int utf8_char_len_pos(const char* text, size_t size, size_t pos) {
    if (pos >= size) return 1;
    return utf8_char_len((unsigned char)text[pos]);
}

// Add: Character processor structure
typedef int (*char_length_func)(const char*, size_t, size_t);

typedef struct {
    char_length_func char_len;
    int is_gb2312;
} CharProcessor;

// Add: Global character processor
static CharProcessor char_processor = {gb2312_char_len, 1};

static inline int body_en_char_width(void) {
    return BODY_EN_FONT.Width;
}

static inline int body_cn_char_width(void) {
    return BODY_CN_FONT.Width;
}

static inline int body_en_line_step(void) {
    return (BODY_EN_FONT.Height * 12 + 9) / 10;
}

static inline int body_cn_line_step(void) {
    return (BODY_CN_FONT.Height * 13 + 9) / 10;
}

static inline int body_first_line_indent_cn(void) {
    return body_cn_char_width() * 2;
}

static inline int body_first_line_indent_en(void) {
    return body_en_char_width() * 2;
}

static int is_ascii_closing_punct(unsigned char c) {
    return (c == ',' || c == '.' || c == '!' || c == '?' ||
            c == ';' || c == ':' || c == ')' || c == ']' ||
            c == '}' || c == '>' || c == '"' || c == '\'');
}

static int is_gbk_closing_punct_at(const char* text, size_t size, size_t pos) {
    if (!text || pos + 1 >= size) return 0;
    unsigned char b0 = (unsigned char)text[pos];
    unsigned char b1 = (unsigned char)text[pos + 1];

    // Common GBK full-width closing punctuation.
    if (b0 == 0xA3 && (b1 == 0xAC || b1 == 0xAE || b1 == 0xA1 || b1 == 0xBF ||
                       b1 == 0xBA || b1 == 0xBB || b1 == 0xA9 || b1 == 0xBD ||
                       b1 == 0xB1 || b1 == 0xAF)) {
        return 1;
    }

    if (b0 == 0xA1 && (b1 == 0xA2 || b1 == 0xA3 || b1 == 0xB7 || b1 == 0xBF ||
                       b1 == 0xA1 || b1 == 0xA8 || b1 == 0xA9)) {
        return 1;
    }
    return 0;
}

static int is_gbk_opening_punct_at(const char* text, size_t size, size_t pos) {
    if (!text || pos + 1 >= size) return 0;
    unsigned char b0 = (unsigned char)text[pos];
    unsigned char b1 = (unsigned char)text[pos + 1];

    // Common GBK full-width opening punctuation.
    if (b0 == 0xA3 && (b1 == 0xA8 || b1 == 0xDB || b1 == 0xB0 || b1 == 0xAE)) {
        return 1;
    }

    if (b0 == 0xA1 && (b1 == 0xBE || b1 == 0xB6 || b1 == 0xB0 || b1 == 0xAA || b1 == 0xAE)) {
        return 1;
    }
    return 0;
}

static void normalize_cn_punct_spacing_gbk(char* text, size_t* text_size) {
    if (!text || !text_size || *text_size == 0) return;

    size_t src = 0;
    size_t dst = 0;
    size_t n = *text_size;

    while (src < n) {
        int clen = char_processor.char_len(text, n, src);
        if (clen <= 0) clen = 1;

        // Trim spaces before Chinese closing punctuation.
        if (clen == 2 && is_gbk_closing_punct_at(text, n, src)) {
            while (dst > 0 && text[dst - 1] == ' ') {
                dst--;
            }
        }

        // Copy current character bytes.
        for (int i = 0; i < clen && src + (size_t)i < n; i++) {
            text[dst++] = text[src + (size_t)i];
        }

        // Trim spaces immediately after Chinese opening punctuation.
        if (clen == 2 && is_gbk_opening_punct_at(text, n, src)) {
            size_t next = src + (size_t)clen;
            while (next < n && text[next] == ' ') {
                next++;
            }
            src = next;
            continue;
        }

        src += (size_t)clen;
    }

    text[dst] = '\0';
    *text_size = dst;
}

static void normalize_ascii_double_quotes_to_gbk(char** text_ptr, size_t* text_size) {
    if (!text_ptr || !*text_ptr || !text_size || *text_size == 0) return;

    char* src = *text_ptr;
    size_t n = *text_size;
    size_t quote_count = 0;

    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)src[i] == 0x22) quote_count++;
    }
    if (quote_count == 0) return;

    // Each ASCII double quote becomes one 2-byte GBK quote, so buffer grows by quote_count bytes.
    size_t out_size = n + quote_count;
    char* out = (char*)malloc(out_size + 1);
    if (!out) return;

    size_t si = 0;
    size_t di = 0;
    int in_quote = 0;
    while (si < n) {
        unsigned char c = (unsigned char)src[si];
        if (c == 0x22) {
            out[di++] = (char)0xA1;
            out[di++] = in_quote ? (char)0xB1 : (char)0xB0;
            in_quote = !in_quote;
            si++;
            continue;
        }
        out[di++] = src[si++];
    }

    out[di] = '\0';
    free(*text_ptr);
    *text_ptr = out;
    *text_size = di;
}

static size_t collect_layout_line(const char* text, size_t text_size, size_t offset,
                                  int start_x, int max_x,
                                  char* line, size_t line_cap,
                                  int* has_cn, int* saw_paragraph_break) {
    size_t len = 0;
    int x = start_x;
    int line_start_x = start_x;
    int in_ascii_word = 0;
    size_t word_start_offset = offset;
    size_t word_start_len = 0;

    if (has_cn) *has_cn = 0;
    if (saw_paragraph_break) *saw_paragraph_break = 0;

    while (offset < text_size) {
        unsigned char c = (unsigned char)text[offset];
        if (c == '\n') {
            if (saw_paragraph_break) *saw_paragraph_break = 1;
            offset++;
            break;
        }
        if (c == ' ' && x == line_start_x) {
            offset++;
            in_ascii_word = 0;
            continue;
        }
        int is_ascii_word_char = (c < 0x80) && (isalnum(c) || c == '_' || c == '\'' || c == '-');
        if (is_ascii_word_char && !in_ascii_word) {
            word_start_offset = offset;
            word_start_len = len;
            in_ascii_word = 1;
        } else if (!is_ascii_word_char) {
            in_ascii_word = 0;
        }
        int bytes = char_processor.char_len(text, text_size, offset);
        int width = (bytes > 1) ? body_cn_char_width() : body_en_char_width();
        if (x + width > max_x) {
            int is_closing_punct = 0;
            if (bytes == 1) {
                is_closing_punct = is_ascii_closing_punct(c);
            } else if (bytes == 2) {
                is_closing_punct = is_gbk_closing_punct_at(text, text_size, offset);
            }

            // Keep closing punctuation with the previous character to avoid leading punctuation on new lines.
            if (len > 0 && is_closing_punct) {
                if (line && line_cap > 0) {
                    if (len + (size_t)bytes >= line_cap) break;
                    for (int i = 0; i < bytes; i++) line[len + (size_t)i] = text[offset + (size_t)i];
                }
                if (has_cn && bytes > 1) *has_cn = 1;
                offset += (size_t)bytes;
                len += (size_t)bytes;
                x += width;
                break;
            }

            // Word doesn't fit: move entire word to next line (no hyphenation)
            if (is_ascii_word_char && word_start_len > 0) {
                offset = word_start_offset;
                len = word_start_len;
            }
            break;
        }
        if (line && line_cap > 0) {
            if (len + (size_t)bytes >= line_cap) break;
            for (int i = 0; i < bytes; i++) line[len + (size_t)i] = text[offset + (size_t)i];
        }
        if (has_cn && bytes > 1) *has_cn = 1;
        offset += (size_t)bytes;
        len += (size_t)bytes;
        x += width;
    }
    if (line && line_cap > 0) {
        if (len < line_cap) line[len] = '\0';
        else line[line_cap - 1] = '\0';
    }
    return offset;
}

// Detect if the text at `offset` starts a Chinese chapter header.
// Returns the number of bytes to skip (the chapter title line), or 0 if not a chapter.
static size_t detect_chapter_header(size_t offset) {
    if (!g_processed_text || offset >= g_processed_text_size) return 0;

    const char* p = g_processed_text + offset;
    size_t remaining = g_processed_text_size - offset;

    // Need at least "第X章" = 4+ bytes
    if (remaining < 4) return 0;

    // Check GB2312 "第" (0xB5 0xDA) or UTF-8 "第" (0xE7 0xAC 0xAC)
    int is_di = 0;
    if ((unsigned char)p[0] == 0xB5 && (unsigned char)p[1] == 0xDA) {
        is_di = 1;
    } else if ((unsigned char)p[0] == 0xE7 && (unsigned char)p[1] == 0xAC && (unsigned char)p[2] == 0xAC) {
        is_di = 1;
    }

    if (!is_di) return 0;

    // Find the chapter keyword after "第": 章/节/回/卷/部
    size_t pos = (char_processor.char_len(g_processed_text, g_processed_text_size, offset) == 1) ? 1 : 
                 ((unsigned char)p[0] > 0x80 ? 2 : 1);

    // Skip the characters between "第" and the chapter keyword
    while (pos < remaining) {
        int clen = char_processor.char_len(g_processed_text, g_processed_text_size, offset + pos);
        if (clen == 2) {
            unsigned char b0 = (unsigned char)p[pos];
            unsigned char b1 = (unsigned char)p[pos + 1];
            // GB2312: 章=D5C2, 节=BDDA, 回=BBD8, 卷=BEED, 部=B2BF
            if ((b0 == 0xD5 && b1 == 0xC2) || // 章
                (b0 == 0xBD && b1 == 0xDA) || // 节
                (b0 == 0xBB && b1 == 0xD8) || // 回
                (b0 == 0xBE && b1 == 0xED) || // 卷
                (b0 == 0xB2 && b1 == 0xBF)) { // 部
                // Chapter found. Return bytes up to the next newline or end.
                size_t end = offset + pos + clen;
                while (end < g_processed_text_size && g_processed_text[end] != '\n') {
                    end++;
                }
                if (end < g_processed_text_size && g_processed_text[end] == '\n') end++;
                return end - offset;
            }
        }
        if (clen <= 0) clen = 1;
        pos += (size_t)clen;
    }

    return 0;
}

// Convert UTF-8 text to GBK so the font table (indexed by GB2312/GBK bytes) can render Chinese glyphs.
static char* convert_utf8_to_gb2312(const char* utf8_text, size_t utf8_size, size_t* out_size) {
    // Use GBK//IGNORE: GBK superset of GB2312, far fewer dropped chars (— " " … etc all in GBK)
    iconv_t cd = iconv_open("GBK//IGNORE", "UTF-8");
    if (cd == (iconv_t)-1) {
        printf("iconv_open GBK//IGNORE failed, falling back to GB2312...\n");
        cd = iconv_open("GB2312//IGNORE", "UTF-8");
    }
    if (cd == (iconv_t)-1) {
        printf("iconv_open for GB2312/GBK failed\n");
        return NULL;
    }

    // GB2312 output is at most same byte count as UTF-8 input (Chinese: 3→2 bytes)
    size_t out_buf_size = utf8_size + 1;
    char* out_buf = malloc(out_buf_size);
    if (!out_buf) {
        iconv_close(cd);
        return NULL;
    }

    char* in_ptr = (char*)utf8_text;
    size_t in_left = utf8_size;
    char* out_ptr = out_buf;
    size_t out_left = out_buf_size;

    size_t ret = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
    iconv_close(cd);

    if (ret == (size_t)-1) {
        printf("iconv conversion finished with some unmappable characters skipped\n");
    }

    *out_size = out_buf_size - out_left;
    out_buf[*out_size] = '\0';
    printf("UTF-8→GB2312: %zu→%zu bytes converted\n", utf8_size, *out_size);

    // Shrink to actual size
    char* result = realloc(out_buf, *out_size + 1);
    return result ? result : out_buf;
}

// Process text content: merge paragraphs, remove extra line breaks
char* process_text_content(const char* raw_text, size_t raw_size) {
    if (!raw_text || raw_size == 0) return NULL;

    // Create temporary buffer to store processed text
    char* processed = malloc(raw_size + 1);  // Initialize to original size, may be slightly larger
    if (!processed) return NULL;

    size_t src_idx = 0, dst_idx = 0;
    int in_paragraph = 0;  // Flag to mark if in middle of paragraph

    while (src_idx < raw_size) {
        // Skip consecutive newlines and whitespace characters
        while (src_idx < raw_size && (raw_text[src_idx] == '\n' || raw_text[src_idx] == '\r')) {
            // Check if it's a paragraph separator (two consecutive newlines)
            size_t temp_idx = src_idx;
            int newline_count = 0;
            while (temp_idx < raw_size && (raw_text[temp_idx] == '\n' || raw_text[temp_idx] == '\r')) {
                if (raw_text[temp_idx] == '\n' || raw_text[temp_idx] == '\r') {
                    newline_count++;
                    // Skip \r\n or \n\r sequences
                    if (temp_idx+1 < raw_size && 
                        ((raw_text[temp_idx]=='\r' && raw_text[temp_idx+1]=='\n') ||
                         (raw_text[temp_idx]=='\n' && raw_text[temp_idx+1]=='\r'))) {
                        temp_idx += 2;
                    } else {
                        temp_idx++;
                    }
                }
            }
            
            // If it's a paragraph separator (at least two newlines), add a newline to mark end of paragraph
            if (newline_count >= 2) {
                if (in_paragraph) {
                    processed[dst_idx++] = '\n';  // Paragraph end marker
                    in_paragraph = 0;
                }
            } else if (in_paragraph) {
                // Replace line breaks between lines with spaces
                processed[dst_idx++] = ' ';
            }
            
            src_idx = temp_idx;
        }

        // Process regular characters
        if (src_idx < raw_size && raw_text[src_idx] != '\n' && raw_text[src_idx] != '\r') {
            // Skip leading whitespace characters (if not in paragraph)
            if (!in_paragraph) {
                while (src_idx < raw_size && raw_text[src_idx] == ' ') src_idx++;
                if (src_idx >= raw_size) break;
            }

            // Copy characters
            unsigned char c = (unsigned char)raw_text[src_idx];
            int bytes = char_processor.char_len(raw_text, raw_size, src_idx);

            // Copy character (skip extra spaces within paragraph)
            if (c == ' ' && in_paragraph && dst_idx > 0 && processed[dst_idx-1] == ' ') {
                // Skip extra spaces
            } else {
                for (int i = 0; i < bytes && src_idx+i < raw_size; i++) {
                    processed[dst_idx++] = raw_text[src_idx + i];
                }
                in_paragraph = 1;
            }

            src_idx += bytes;
        }
    }

    processed[dst_idx] = '\0';
    // Reallocate to appropriate size
    char* result = realloc(processed, dst_idx + 1);
    if (!result) result = processed;  // If realloc fails, return original pointer
    return result;
}

// Calculate total number of pages and store starting offset of each page
void calculate_page_info() {
    if (!g_processed_text) return;
    
    // Free previous page offset array
    if (page_offsets) {
        free(page_offsets);
        page_offsets = NULL;
    }
    
    // Temporary storage for page offsets, using larger buffer
    size_t *temp_offsets = malloc(sizeof(size_t) * (g_processed_text_size / 1000 + 100));
    if (!temp_offsets) {
        printf("Error: Could not allocate memory for temporary page offsets\n");
        return;
    }
    
    int count = 0;
    size_t offset = 0;

    // Loop to calculate starting offset of each page
    while (offset < g_processed_text_size) {
        if(count >= (g_processed_text_size / 1000 + 100)) {
            // If array capacity is insufficient, reallocate larger space
            size_t *new_temp_offsets = realloc(temp_offsets, sizeof(size_t) * (count + 1000));
            if(new_temp_offsets) {
                temp_offsets = new_temp_offsets;
            } else {
                printf("Warning: Could not expand memory for page offsets, stop calculation at page %d\n", count);
                break;
            }
        }
        
        temp_offsets[count++] = offset;

                int y = CONTENT_Y_START;
                const int text_bottom = FOOTER_Y_START - 5;
                const int paragraph_gap = PARAGRAPH_SPACING_PX;
                int paragraph_gap_pending = 0;
                int page_has_content = 0;

                while (offset < g_processed_text_size && y < text_bottom) {
                    // Chapter break: if we've already rendered content and a new chapter starts, finish this page.
                    if (page_has_content) {
                        size_t ch_skip = detect_chapter_header(offset);
                        if (ch_skip > 0) {
                            break;
                        }
                    }

                    // Determine paragraph indent based on first character
                    int is_cn_paragraph = 0;
                    if (offset < g_processed_text_size) {
                        unsigned char fc = (unsigned char)g_processed_text[offset];
                        is_cn_paragraph = (fc >= 0x80);
                    }
                    int indent_px = is_cn_paragraph ? body_first_line_indent_cn() : body_first_line_indent_en();

                    if (paragraph_gap_pending > 0) {
                        y += paragraph_gap_pending;
                        paragraph_gap_pending = 0;
                        if (y >= text_bottom) {
                            break;
                        }
                    }

                    int paragraph_start = (offset == 0 || g_processed_text[offset - 1] == '\n');
                    int x = CONTENT_X_MARGIN + (paragraph_start ? indent_px : 0);
                    char line[1024] = {0};
                    int has_cn = 0;
                    int saw_paragraph_break = 0;
                    size_t line_start = offset;

                    size_t next_offset = collect_layout_line(
                        g_processed_text,
                        g_processed_text_size,
                        offset,
                        x,
                        EPD_7IN5_V2_WIDTH - CONTENT_X_MARGIN * 2,
                        line,
                        sizeof(line),
                        &has_cn,
                        &saw_paragraph_break
                    );

                    if (next_offset == line_start && !saw_paragraph_break) {
                        int char_bytes = char_processor.char_len(g_processed_text, g_processed_text_size, offset);
                        if (char_bytes <= 0) char_bytes = 1;
                        offset += (size_t)char_bytes;
                        paragraph_start = 0;
                        continue;
                    }

                    if (next_offset == line_start && saw_paragraph_break) {
                        offset = next_offset;
                        paragraph_start = 1;
                        paragraph_gap_pending = paragraph_gap;
                        continue;
                    }

                    page_has_content = 1;

                    int lh = has_cn ? body_cn_line_step() : body_en_line_step();
                    if (y + lh > text_bottom)
                        break;

                    y += lh;
                    offset = next_offset;
                    paragraph_start = 0;

                    if (saw_paragraph_break) {
                        paragraph_start = 1;
                        paragraph_gap_pending = paragraph_gap;
                    }
                }

                if(!page_has_content && offset < g_processed_text_size) {
                    printf("Warning: No content placed on page %d but text remains\n", count);
                    break;
                }
    }
    
    // Allocate exact size page offset array
    total_pages = count;
    page_offsets = malloc(sizeof(size_t) * total_pages);
    if (page_offsets) {
        memcpy(page_offsets, temp_offsets, sizeof(size_t) * total_pages);
        printf("Successfully calculated %d pages\n", total_pages);
    } else {
        printf("Error: Could not allocate memory for page offsets\n");
    }
    
    free(temp_offsets);
}

// Get current page index
int get_current_page_index(size_t offset) {
    if (!page_offsets || total_pages == 0) {
        // If unable to get accurate page count, use estimation method
        return (offset / 2000) + 1;
    }
    
    // Binary search for page containing current offset
    int left = 0, right = total_pages - 1;
    int result = 0;
    
    while (left <= right) {
        int mid = left + (right - left) / 2;
        if (page_offsets[mid] <= offset) {
            result = mid;
            if (mid < total_pages - 1) {
                left = mid + 1;
            } else {
                break;  // Already at last page
            }
        } else {
            if (mid > 0) {
                right = mid - 1;
            } else {
                break;  // Already at first page
            }
        }
    }
    
    return result + 1; // Page numbers start from 1
}

// Load entire TXT file to memory (GB2312 encoding)
int load_txt_file(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        printf("Failed to open TXT: %s (errno=%d)\n", path, errno);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size <= 0 || size > MAX_BOOK_SIZE) {
        fclose(fp);
        printf("File too large or empty: %ld\n", size);
        return -1;
    }

    if (g_full_text) free(g_full_text);
    g_full_text = (char*)malloc(size + 1);
    if (!g_full_text) {
        fclose(fp);
        printf("Malloc failed for text\n");
        return -1;
    }

    fseek(fp, 0, SEEK_SET);
    size_t read_bytes = fread(g_full_text, 1, size, fp);
    fclose(fp);

    if (read_bytes != (size_t)size) {
        free(g_full_text);
        g_full_text = NULL;
        return -1;
    }
    g_full_text[read_bytes] = '\0';
    g_text_size = read_bytes;

    // Add: Detect file encoding
    int is_gb2312 = detect_file_encoding(g_full_text, g_text_size);
    char_processor.char_len = is_gb2312 ? 
        (int (*)(const char*, size_t, size_t))gb2312_char_len : 
        utf8_char_len_pos;
    char_processor.is_gb2312 = is_gb2312;
    
    printf("Detected file encoding: %s\n", is_gb2312 ? "GB2312" : "UTF-8");

    // Process text: remove extra line breaks, merge paragraphs
    if (g_processed_text) free(g_processed_text);
    g_processed_text = process_text_content(g_full_text, g_text_size);
    if (!g_processed_text) {
        printf("Failed to process text content\n");
        return -1;
    }
    g_processed_text_size = strlen(g_processed_text);

    // If the source was UTF-8, convert processed text to GB2312 for font table lookup.
    if (!is_gb2312) {
        size_t gb_size = 0;
        char* gb_text = convert_utf8_to_gb2312(g_processed_text, g_processed_text_size, &gb_size);
        if (gb_text && gb_size > 0) {
            free(g_processed_text);
            g_processed_text = gb_text;
            g_processed_text_size = gb_size;
            char_processor.char_len = (int (*)(const char*, size_t, size_t))gb2312_char_len;
            char_processor.is_gb2312 = 1;
            printf("Converted UTF-8 to GB2312: %zu bytes\n", g_processed_text_size);
        } else {
            printf("Warning: UTF-8→GB2312 conversion failed, Chinese may not display\n");
            if (gb_text) free(gb_text);
        }
    }

    // Convert ASCII quotes to full-width Chinese quotes for better CJK typography.
    normalize_ascii_double_quotes_to_gbk(&g_processed_text, &g_processed_text_size);

    // Normalize spacing around Chinese punctuation after final GB2312/GBK text is ready.
    normalize_cn_punct_spacing_gbk(g_processed_text, &g_processed_text_size);

    // Reset status
    g_current_char_offset = 0;
    g_displayed_page_offset = 0;
    history_top = -1; // Clear history
    first_display_done = 0;
    
    // Calculate page info
    calculate_page_info();
    current_page_index = 1;  // Reset to first page

    printf("Loaded %zu bytes from %s, processed to %zu bytes\n", g_text_size, path, g_processed_text_size);
    return 0;
}

void show_error(const char* msg) {
    printf("ERROR: %s\n", msg);
    if (g_frame_buffer == NULL) return;
    Paint_SelectImage(g_frame_buffer);
    Paint_Clear(WHITE);
    Paint_DrawString_EN(10, 10, "ERROR", &Font16, BLACK, WHITE);
    Paint_DrawString_EN(10, 40, msg, &Font16, BLACK, WHITE);
    EPD_7IN5_V2_Display(g_frame_buffer);
    sleep(3);
}

// Draw mixed ASCII/CJK title text. Convert UTF-8 filename to GBK for CN font lookup.
static void draw_header_title_mixed(UWORD x, UWORD y, const char* utf8_title, UWORD fg, UWORD bg) {
    if (!utf8_title) return;

    size_t gb_size = 0;
    char* gb_title = convert_utf8_to_gb2312(utf8_title, strlen(utf8_title), &gb_size);
    const char* text = gb_title ? gb_title : utf8_title;
    size_t text_size = gb_title ? gb_size : strlen(utf8_title);

    UWORD draw_x = x;
    for (size_t i = 0; i < text_size;) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) {
            char ch[2] = {(char)c, '\0'};
            Paint_DrawString_EN(draw_x, y, ch, &BODY_EN_FONT, bg, fg);
            draw_x += body_en_char_width();
            i++;
        } else if (i + 1 < text_size) {
            char cn[3] = {text[i], text[i + 1], '\0'};
            Paint_DrawString_CN(draw_x, y, cn, &BODY_CN_FONT, bg, fg);
            draw_x += body_cn_char_width();
            i += 2;
        } else {
            break;
        }
    }

    if (gb_title) {
        free(gb_title);
    }
}

/* Core: Draw one page from specified offset and return starting offset of next page */
size_t display_txt_page_from_offset(size_t start_offset)
{
    // Use processed text instead of original text
    if (!g_processed_text || start_offset >= g_processed_text_size) {
        Paint_SelectImage(g_frame_buffer);
        Paint_Clear(WHITE);
        EPD_7IN5_V2_Display(g_frame_buffer);
        return g_processed_text_size;
    }

    // Record the page that is actually rendered, so wake-up can restore it reliably.
    g_displayed_page_offset = start_offset;

    Paint_SelectImage(g_frame_buffer);

    /* =====================================================
     * 1. First display or book switch: Full screen initialization
     * ===================================================== */
    if (!first_display_done || book_changed) {
        Paint_Clear(WHITE);

        /* Header —— Permanent area */
        char title[512];
        const char* name = strrchr(current_file, '/');
        name = name ? name + 1 : current_file;
        
        char display_name[500];
        safe_truncate_filename(display_name, name, sizeof(display_name));

        char *dot = strrchr(display_name, '.');
        if (dot && dot != display_name) {
            *dot = 0;
        }

        snprintf(title, sizeof(title), "Book: %s", display_name);
        draw_header_title_mixed(CONTENT_X_MARGIN, 5, title, BLACK, WHITE);

        Paint_DrawLine(
            CONTENT_X_MARGIN,
            HEADER_HEIGHT,
            EPD_7IN5_V2_WIDTH - CONTENT_X_MARGIN,
            HEADER_HEIGHT,
            BLACK,
            DOT_PIXEL_1X1,
            LINE_STYLE_SOLID
        );
        EPD_7IN5_V2_Init_Fast();
        EPD_7IN5_V2_Clear();
        EPD_7IN5_V2_Display(g_frame_buffer);
        EPD_7IN5_V2_Init_Part();

        first_display_done = 1;
        book_changed = 0;
        header_drawn = 1;
    }
    else if (!header_drawn) {
        /* =================================================
         * 2. Ensure Header area always exists
         *    Including cases where header needs to be redrawn after screen-off recovery
         * ===================================================== */
        // Clear header area
        // Paint_ClearWindows(0, 0, EPD_7IN5_V2_WIDTH, HEADER_HEIGHT + 5, WHITE);
        // Add: Clear content area and footer area
        Paint_ClearWindows(
            0,
            0,
            EPD_7IN5_V2_WIDTH,
            EPD_7IN5_V2_HEIGHT,
            BLACK
        );
      
        char title[512];
        const char* name = strrchr(current_file, '/');
        name = name ? name + 1 : current_file;
        
        char display_name[500];
        safe_truncate_filename(display_name, name, sizeof(display_name));
        
        char *ext = strrchr(display_name, '.');
        if (ext && ext != display_name) {
            *ext = 0;
        }

        snprintf(title, sizeof(title), "Book: %s", display_name);
        draw_header_title_mixed(CONTENT_X_MARGIN, 5, title, WHITE, BLACK);
        Paint_DrawLine(
            CONTENT_X_MARGIN,
            HEADER_HEIGHT,
            EPD_7IN5_V2_WIDTH - CONTENT_X_MARGIN,
            HEADER_HEIGHT,
            WHITE,
            DOT_PIXEL_1X1,
            LINE_STYLE_SOLID
        );
        header_drawn = 1;
    }
    else {
        /* =================================================
         * 3. Page turning: Clear and redraw HEADER + CONTENT + FOOTER
         *    Ensures header is always visible and consistent
         * ================================================= */
        // Clear header area
        Paint_ClearWindows(
            0,
            0,
            EPD_7IN5_V2_WIDTH,
            CONTENT_Y_START,
            BLACK
        );
        // Redraw header
        {
            char title[512];
            const char* name = strrchr(current_file, '/');
            name = name ? name + 1 : current_file;
            char display_name[500];
            safe_truncate_filename(display_name, name, sizeof(display_name));
            char *ext = strrchr(display_name, '.');
            if (ext && ext != display_name) *ext = 0;
            snprintf(title, sizeof(title), "Book: %s", display_name);
            draw_header_title_mixed(CONTENT_X_MARGIN, 5, title, WHITE, BLACK);
            Paint_DrawLine(
                CONTENT_X_MARGIN,
                HEADER_HEIGHT,
                EPD_7IN5_V2_WIDTH - CONTENT_X_MARGIN,
                HEADER_HEIGHT,
                WHITE,
                DOT_PIXEL_1X1,
                LINE_STYLE_SOLID
            );
        }
        // Clear content + footer area
        Paint_ClearWindows(
            0,
            CONTENT_Y_START,
            EPD_7IN5_V2_WIDTH,
            EPD_7IN5_V2_HEIGHT-CONTENT_Y_START,
            BLACK
        );
    }

    /* =====================================================
     * 4. Text layout drawing
     * ===================================================== */
    const int left_margin = CONTENT_X_MARGIN;
    const int right_margin = CONTENT_X_MARGIN;
    int y = CONTENT_Y_START;
    const int text_bottom = FOOTER_Y_START - 5;
    const int paragraph_gap = PARAGRAPH_SPACING_PX;
    int paragraph_gap_pending = 0;

    // Use processed text
    size_t i = start_offset;

    size_t result_offset = i;

    while (i < g_processed_text_size && y < text_bottom) {
        // Chapter break: if we've already rendered content and a new chapter starts, finish this page.
        if (i != start_offset) {
            size_t ch_skip = detect_chapter_header(i);
            if (ch_skip > 0) {
                break;
            }
        }

        // Determine paragraph indent based on first character
        int is_cn_paragraph = 0;
        if (i < g_processed_text_size) {
            unsigned char fc = (unsigned char)g_processed_text[i];
            is_cn_paragraph = (fc >= 0x80);
        }
        int indent_px = is_cn_paragraph ? body_first_line_indent_cn() : body_first_line_indent_en();

        if (paragraph_gap_pending > 0) {
            y += paragraph_gap_pending;
            paragraph_gap_pending = 0;
            if (y >= text_bottom) {
                break;
            }
        }

        int paragraph_start = (i == 0 || g_processed_text[i - 1] == '\n');
        int x = left_margin + (paragraph_start ? indent_px : 0);
        char line[1024] = {0};
        int has_cn = 0;
        int saw_paragraph_break = 0;
        size_t line_start = i;

        size_t next_offset = collect_layout_line(
            g_processed_text,
            g_processed_text_size,
            i,
            x,
            EPD_7IN5_V2_WIDTH - left_margin - right_margin,
            line,
            sizeof(line),
            &has_cn,
            &saw_paragraph_break
        );

        int len = (int)strlen(line);

        if (next_offset == line_start && !saw_paragraph_break) {
            int char_bytes = char_processor.char_len(g_processed_text, g_processed_text_size, i);
            if (char_bytes <= 0) char_bytes = 1;
            i += (size_t)char_bytes;
            paragraph_start = 0;
            continue;
        }

        if (next_offset == line_start && saw_paragraph_break) {
            i = next_offset;
            paragraph_start = 1;
            paragraph_gap_pending = paragraph_gap;
            continue;
        }

        if (len > 0) {
            int lh = has_cn ? body_cn_line_step() : body_en_line_step();
            if (y + lh > text_bottom)
                break;

            int draw_x = left_margin + (paragraph_start ? indent_px : 0);
            if (has_cn) {
                // For mixed lines (CN + EN), draw each character individually
                // with the correct font so ASCII uses Font20 (14px width)
                // matching the layout calculation, not Font12CN's narrow 8px.
                const char* p = line;
                while (*p) {
                    unsigned char c = (unsigned char)*p;
                    if (c <= 0x7F) {
                        // ASCII character — draw with EN font at 14px width
                        char buf[2] = {c, '\0'};
                        Paint_DrawString_EN(draw_x, y, buf, &BODY_EN_FONT, BLACK, WHITE);
                        draw_x += body_en_char_width();
                        p++;
                    } else {
                        // Chinese character (2 bytes) — draw with CN font at 18px width
                        char cn_buf[3] = {p[0], p[1], '\0'};
                        Paint_DrawString_CN(draw_x, y, cn_buf, &BODY_CN_FONT, WHITE, BLACK);
                        draw_x += body_cn_char_width();
                        p += 2;
                    }
                }
            } else {
                Paint_DrawString_EN(draw_x, y, line, &BODY_EN_FONT, BLACK, WHITE);
            }

            y += lh;
        }

        i = next_offset;
        result_offset = next_offset;
        paragraph_start = 0;

        if (saw_paragraph_break) {
            paragraph_start = 1;
            paragraph_gap_pending = paragraph_gap;
        }
    }

    if (y < FOOTER_Y_START) {
        Paint_ClearWindows(
            0,
            y,
            EPD_7IN5_V2_WIDTH,
            FOOTER_Y_START,
            BLACK
        );
    }

    // Use accurate page count calculation
    int cur_page = get_current_page_index(start_offset);
    int total_pages_calc = total_pages > 0 ? total_pages : (g_processed_text_size / 2000) + 1;

    char page[64];
    snprintf(page, sizeof(page), "Page %d / %d", cur_page, total_pages_calc);

    int page_str_width = (int)strlen(page) * BODY_EN_FONT.Width;
    int page_x = EPD_7IN5_V2_WIDTH - page_str_width - CONTENT_X_MARGIN;
    if (page_x < 0) page_x = 0;

    Paint_DrawString_EN(
        page_x,
        FOOTER_Y_START+5,  // Adjust page number Y coordinate to avoid overlapping with content
        page,
        &BODY_EN_FONT,
        BLACK,
        WHITE
    );

        EPD_7IN5_V2_Display_Part(
            g_frame_buffer,
            0,
            0,  // Starting from top, including Header
            EPD_7IN5_V2_WIDTH,
            EPD_7IN5_V2_HEIGHT // Refresh entire screen height
        );
    return result_offset;  // Return actual ending offset
}

// Enter screen-off mode
void enter_screen_off_mode() {
    if (screen_off) return; // If already in screen-off state, return directly

    printf("Entering screen off mode...\n");
    screen_off = 1;
    // Create screen-off image
    Paint_SelectImage(g_frame_buffer);
    Paint_Clear(WHITE);
    
        // Display screen-off image using GUI_ReadBmp function
    GUI_ReadBmp_Scale_Centered("./components/e-Paper/Quectel-Pi-H1/c/pic/2.bmp", 0, 0,EPD_7IN5_V2_WIDTH,EPD_7IN5_V2_HEIGHT,0.7) ;
    
    // Display screen-off image
    EPD_7IN5_V2_Display(g_frame_buffer);
    // EPD_7IN5_V2_Sleep(); // Enter sleep mode to save power
}

// Exit screen-off mode (optimized version)
void exit_screen_off_mode() {
    if (!screen_off) return; // If not in screen-off state, return directly

    printf("Exiting screen off mode...\n");
    screen_off = 0;
    // Fast wake up: only initialize partial refresh mode
    EPD_7IN5_V2_Init_Part();
    
    // Set flags to ensure only content and footer are refreshed
    first_display_done = 1;
    book_changed = 0;
    header_drawn = 0;  // Mark only that header needs to be redrawn, handled by display_txt_page_from_offset

        // Use fast recovery: redraw the page that was visible before screen-off.
            if (g_frame_buffer && g_processed_text) {
                size_t restore_offset = g_displayed_page_offset;
                if (restore_offset >= g_processed_text_size) {
                        restore_offset = (g_current_char_offset < g_processed_text_size) ? g_current_char_offset : 0;
                }
                // Directly call display_txt_page_from_offset, which will redraw Header based on header_drawn=0
                display_txt_page_from_offset(restore_offset);
    }
    // Clear accumulated events from eye control device
    struct input_event ev;
    if (eye_key_fd >= 0) {
        while (read(eye_key_fd, &ev, sizeof(ev)) == sizeof(ev)) {}
    }
    
    // Activate anti-flicker protection for 1.5 seconds
    struct timeval tv;
    gettimeofday(&tv, NULL);
    anti_flicker_until = tv.tv_sec + 2; // Extend to 2-second safety delay period
}

// Function to safely truncate filename for display
void safe_truncate_filename(char* dest, const char* src, size_t dest_size) {
    if (!src || !dest || dest_size == 0) return;
    
    size_t src_len = strlen(src);
    if (src_len < dest_size) {
        strncpy(dest, src, dest_size - 1);
        dest[dest_size - 1] = '\0';
    } else {
        // Need to truncate - try to preserve the extension
        const char* ext = strrchr(src, '.');
        if (ext) {
            size_t ext_len = strlen(ext);
            size_t base_len = dest_size - ext_len - 4; // 3 dots + null terminator
            
            if (base_len > 0) {
                strncpy(dest, src, base_len);
                strcpy(dest + base_len, "...");
                strcat(dest, ext);
            } else {
                // Extension is too long, just truncate from beginning
                strncpy(dest, src + (src_len - dest_size + 1), dest_size - 1);
                dest[dest_size - 1] = '\0';
            }
        } else {
            // No extension, just truncate
            strncpy(dest, src, dest_size - 4);
            strcpy(dest + dest_size - 4, "...");
        }
    }
}

// Switch to next book
void next_book() {
    if (book_count > 1) {
        current_book_index = (current_book_index + 1) % book_count;
        snprintf(current_file, sizeof(current_file), "%s", book_list[current_book_index]);
        if (load_txt_file(current_file) == 0) {
            g_current_char_offset = 0;
            g_current_char_offset = display_txt_page_from_offset(0);  // Update current offset
            // Start of new book, clear history, push first page
            history_top = -1;
            if (history_top < MAX_HISTORY - 1) {
                history_stack[++history_top] = 0;
            }
            // Reset title flag to redraw title when switching to new book
            title_drawn = 0;
            current_page_index = 1;  // Reset to first page
            printf("Switched to book [%d]: %s\n", current_book_index, current_file);
        }
    }
}

// Switch to previous book
void prev_book() {
    if (book_count > 1) {
        current_book_index = (current_book_index - 1 + book_count) % book_count;
        snprintf(current_file, sizeof(current_file), "%s", book_list[current_book_index]);
        if (load_txt_file(current_file) == 0) {
            g_current_char_offset = 0;
            g_current_char_offset = display_txt_page_from_offset(0);  // Update current offset
            // Start of new book, clear history, push first page
            history_top = -1;
            if (history_top < MAX_HISTORY - 1) {
                history_stack[++history_top] = 0;
            }
            // Reset title flag to redraw title when switching to new book
            title_drawn = 0;
            current_page_index = 1;  // Reset to first page
            printf("Switched to book [%d]: %s\n", current_book_index, current_file);
        }
    }
}

// New: Key state tracking structure
typedef struct {
    struct timeval press_time;
    int pressed;
    int key_id;
} KeyState;

static KeyState key_states[3] = {0}; // Index 0 unused, 1=KEY1, 2=KEY2

// New: Key event handling function
void handle_key_event(int key_id, struct input_event *ev) {
    if (ev->type != EV_KEY) return;
    
    // Check if currently in anti-flicker mode (within 1.5 seconds after screen-on)
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    if (current_time.tv_sec < anti_flicker_until) {
        // In anti-flicker mode, only allow screen-off events to pass through
        if (!(ev->code == CUSTOM_SCREEN_OFF_BTN)) {
            printf("Anti-flicker protection active, ignoring key event\n");
            return;
        }
    }

    // New: Print actual received key codes for debugging
    printf("Received key event: id=%d, code=%d, value=%d\n", key_id, ev->code, ev->value);

    // Check if this is a supported key code
    if (key_id == 1) {
        if (!(ev->code == KEY_PAGEDOWN || 
              ev->code == BTN_LEFT || 
              ev->code == BTN_RIGHT || 
              ev->code == BTN_MIDDLE || 
              ev->code == KEY_NEXTSONG ||
              ev->code == BTN_EXTRA ||
              ev->code == KEY_VOLUMEDOWN)) {  // Add actually used key codes
            printf("Key1: Ignoring code %d\n", ev->code);
            return;
        }
    } else if (key_id == 2) {
        if (!(ev->code == KEY_PAGEUP || 
              ev->code == BTN_BASE ||
              ev->code == KEY_VOLUMEUP)) {  // Add actually used key codes
            printf("Key2: Ignoring code %d\n", ev->code);
            return;
        }
    }

    if (ev->value == 1) {  // key down
        gettimeofday(&key_states[key_id].press_time, NULL);
        key_states[key_id].pressed = 1;
    }
    else if (ev->value == 0 && key_states[key_id].pressed) { // key up
        struct timeval now;
        gettimeofday(&now, NULL);

        long press_ms =
            (now.tv_sec - key_states[key_id].press_time.tv_sec) * 1000 +
            (now.tv_usec - key_states[key_id].press_time.tv_usec) / 1000;

        key_states[key_id].pressed = 0;

        if (press_ms > 1000) {
            if (key_id == 1) next_book();
            else prev_book();
        } else {
            if (key_id == 1) {
                if (g_current_char_offset < g_processed_text_size) {
                    if (history_top < MAX_HISTORY - 1) {
                        history_stack[++history_top] = g_current_char_offset;
                    }
                    size_t next_offset = display_txt_page_from_offset(g_current_char_offset);
                    g_current_char_offset = next_offset;
                    printf("Next page from key1 at offset %zu\n", g_current_char_offset);
                } else {
                    printf("End of book.\n");
                }
            } else {
                if (history_top > -1) {
                    g_current_char_offset = history_stack[history_top--];
                    display_txt_page_from_offset(g_current_char_offset);
                    printf("Back to page at offset %zu\n", g_current_char_offset);
                } else {
                    printf("Already at first page.\n");
                }
            }
        }
    }
}

// Modify: Use poll mechanism to handle key events
void handle_keys(void) {
    struct pollfd fds[3];
    struct input_event ev;

    // Set up poll descriptors
    fds[0].fd = key1_fd;
    fds[0].events = POLLIN;
    fds[1].fd = key2_fd;
    fds[1].events = POLLIN;

    // Add eye control device to poll
    int num_fds = 2;
    if (eye_key_fd >= 0) {
        fds[2].fd = eye_key_fd;
        fds[2].events = POLLIN;
        num_fds = 3;
    }

    // Non-blocking polling
    int ret = poll(fds, num_fds, 0);
    if (ret <= 0) return;

    // Handle KEY1 events
    if (fds[0].revents & POLLIN) {
        if (read(key1_fd, &ev, sizeof(ev)) == sizeof(ev)) {
            handle_key_event(1, &ev);
        }
    }

    // Handle KEY2 events
    if (fds[1].revents & POLLIN) {
        if (read(key2_fd, &ev, sizeof(ev)) == sizeof(ev)) {
            handle_key_event(2, &ev);
        }
    }

    // Handle eye control device events
    if (num_fds > 2 && (fds[2].revents & POLLIN)) {
        if (read(eye_key_fd, &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_KEY) {
                if (ev.code == CUSTOM_SCREEN_OFF_BTN && ev.value == 1) {
                    enter_screen_off_mode();
                } else if (ev.code == CUSTOM_SCREEN_ON_BTN && ev.value == 1) {
                    exit_screen_off_mode();
                } else if (ev.code == KEY_PAGEDOWN) {
                    handle_key_event(1, &ev); 
                }
            }
        }
    }
}

// Main function
void EPD_7in5_V2_reader_txt(void) {
    printf("E-Ink Reader: Full Continuity, No Truncation, Exact Page History\n");

#ifndef QUECPI
    if (DEV_Module_Init() != 0) return;
#else
    extern int GPIO_Handle;
    GPIO_Handle = lgGpiochipOpen(4);
    if (GPIO_Handle < 0) {
        printf("Failed to open gpiochip4\n");
        return;
    }
    lgGpioClaimOutput(GPIO_Handle, 0, 47, 0);
    extern int SPI_Handle;
    SPI_Handle = lgSpiOpen(10, 0, 10000000, 0);
    if (SPI_Handle < 0) {
        printf("Failed to open spidev10.0\n");
        return;
    }
    DEV_GPIO_Init();
#endif
    // Open physical key device
    key1_fd = open("/dev/input/event3", O_RDONLY);
    key2_fd = open("/dev/input/event1", O_RDONLY);

    if (key1_fd < 0 || key2_fd < 0) {
        printf("Physical key devices not found\n");
        goto cleanup;
    }

    // Initialize and find virtual eye control device
    init_eye_control_device();
    eye_key_fd = find_eye_control_device();
    if (eye_key_fd < 0) {
        printf("Warning: Failed to find eye control device, attempting to open event9 as fallback\n");
        // Fallback option: try opening event9
        eye_key_fd = open("/dev/input/event9", O_RDONLY | O_NONBLOCK);
        if(eye_key_fd < 0) {
            printf("Warning: Failed to open fallback eye control device\n");
        }
    }

    DIR* dir = opendir(BOOK_PATH);
    if (!dir) {
        show_error("Books dir not found");
        goto cleanup;
    }
    struct dirent* entry;
    book_count = 0;
    while ((entry = readdir(dir)) != NULL && book_count < MAX_BOOKS) {
        if (entry->d_type == DT_REG) {
            const char* ext = get_ext(entry->d_name);
            if (strcasecmp(ext, "txt") == 0) {
                // Check if the combined path would fit in our buffer
                size_t path_len = strlen(BOOK_PATH) + 1 + strlen(entry->d_name);
                if (path_len < sizeof(book_list[0])) {
                    snprintf(book_list[book_count], sizeof(book_list[0]), "%s/%s", BOOK_PATH, entry->d_name);
                    book_count++;
                } else {
                    printf("Skipping file with path too long: %s\n", entry->d_name);
                }
            }
        }
    }
    closedir(dir);
    if (book_count == 0) {
        show_error("No TXT file found");
        goto cleanup;
    }

    current_book_index = 0;
    // Copy safely with truncation check
    if (strlen(book_list[current_book_index]) >= sizeof(current_file)) {
        printf("Warning: Book path too long, truncating\n");
        strncpy(current_file, book_list[current_book_index], sizeof(current_file) - 1);
        current_file[sizeof(current_file) - 1] = '\0';
    } else {
        strcpy(current_file, book_list[current_book_index]);
    }

    if (load_txt_file(current_file) != 0) {
        show_error("TXT load failed");
        goto cleanup;
    }

    UDOUBLE Imagesize = ((EPD_7IN5_V2_WIDTH % 8 == 0) ? (EPD_7IN5_V2_WIDTH / 8) : (EPD_7IN5_V2_WIDTH / 8 + 1)) * EPD_7IN5_V2_HEIGHT;
    g_frame_buffer = (UBYTE *)malloc(Imagesize);
    if (!g_frame_buffer) {
        printf("Malloc failed\n");
        goto cleanup;
    }
    // Allocate previous frame buffer for comparison and partial refresh
    g_prev_frame_buffer = (UBYTE *)malloc(Imagesize);
    if (!g_prev_frame_buffer) {
        printf("Malloc for previous frame failed\n");
        free(g_frame_buffer);
        goto cleanup;
    }
    Paint_NewImage(g_frame_buffer, EPD_7IN5_V2_WIDTH, EPD_7IN5_V2_HEIGHT, ROTATE_180, WHITE);

    // Display first page - Ensure first display is correct
    g_current_char_offset = 0;  // Ensure starting from the beginning of the text
    g_current_char_offset = display_txt_page_from_offset(g_current_char_offset);  // Update current offset to the start of next page
    // Push first page history (to allow backing to start)
    if (history_top < MAX_HISTORY - 1) {
        history_stack[++history_top] = 0;  // Store the starting offset of the first page
    }

    printf("Reader started. Books: %d\n", book_count);
    while (1) {
        handle_keys(); // Handle physical keys and virtual eye control keys
        usleep(50000);
    }

cleanup:
    free(g_full_text);
    free(g_processed_text);  // Free processed text
    free(page_offsets);      // Free page offset array
    free(g_frame_buffer);
    free(g_prev_frame_buffer);
    if (key1_fd >= 0) close(key1_fd);
    if (key2_fd >= 0) close(key2_fd);
    if (eye_key_fd >= 0) close(eye_key_fd);
    EPD_7IN5_V2_Sleep();
}
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

// Define custom screen off and on signals
#define CUSTOM_SCREEN_OFF_BTN BTN_LEFT
#define CUSTOM_SCREEN_ON_BTN BTN_RIGHT

#define BOOK_PATH "/home/pi/e-ink-reader/demo-inkscreen-reader/books"
#define MAX_BOOK_SIZE (4* 1024 * 1024) // 4MB
#define MAX_BOOKS 20
#define MAX_HISTORY 500 // Record up to 500 page history entries

// Partial refresh area definitions
#define HEADER_HEIGHT   30
#define FOOTER_HEIGHT   30  // Increase footer height to provide space for page numbers

#define CONTENT_Y_START   (HEADER_HEIGHT + 5)  // Reduce margin at top of content area
#define FOOTER_Y_START    (EPD_7IN5_V2_HEIGHT - FOOTER_HEIGHT)

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
        
        // Use same display logic to calculate how many characters fit on one page
        const int left_margin = 0;
        const int max_x = EPD_7IN5_V2_WIDTH ;
        
        const int lh_en = Font16.Height;
        const int lh_cn = Font12CN.Height;
        
        int y = CONTENT_Y_START + 10;
        const int text_bottom = FOOTER_Y_START - 5;

        // Calculate content for one page
        int page_has_content = 0; // Flag to mark if this page has content
        
        while (offset < g_processed_text_size && y < text_bottom) {
            int x = left_margin;
            int has_cn = 0;

            // Build a line of text until reaching maximum width or encountering paragraph end marker
            while (offset < g_processed_text_size) {
                unsigned char c = (unsigned char)g_processed_text[offset];
                // Encounter paragraph end marker, move to next line
                if (c == '\n') {
                    offset++;  // Skip paragraph end marker
                    // First line after paragraph needs indent, so set x to indent distance
                    x = left_margin + (Font16.Width * 30);  // Indent 30 character widths
                    continue;  // Continue to next iteration
                }

                // Modify: Use dynamic character length detection
                int bytes = char_processor.char_len(g_processed_text, g_processed_text_size, offset);

                int width = (bytes > 1) ? Font12CN.Width : Font16.Width;

                if (x + width > max_x)
                    break;  // Reached line width limit, move to next line

                if (bytes > 1) has_cn = 1;
                offset += bytes;
                x += width;
            }

            page_has_content = 1; // At least one line of content
                
            int lh = has_cn ? lh_cn : lh_en;
            if (y + lh > text_bottom)
                break;

            y += lh;
        }
        
        // If this page has no content but there's still remaining text, the text exceeds page space
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

    // Reset status
    g_current_char_offset = 0;
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
        Paint_DrawString_EN(10, 10, title, &Font16, WHITE,BLACK);

        Paint_DrawLine(
            10,
            HEADER_HEIGHT,
            EPD_7IN5_V2_WIDTH - 10,
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
        Paint_DrawString_EN(10, 10, title, &Font16, BLACK, WHITE);
        Paint_DrawLine(
            10,
            HEADER_HEIGHT,
            EPD_7IN5_V2_WIDTH - 10,
            HEADER_HEIGHT,
            WHITE,
            DOT_PIXEL_1X1,
            LINE_STYLE_SOLID
        );
        header_drawn = 1;
    }
    else {
        /* =================================================
         * 3. Page turning: Only clear CONTENT + FOOTER (not Header)
         *    But skip during screen-off recovery
         * ================================================= */
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
    const int left_margin = 0;
    const int max_x = EPD_7IN5_V2_WIDTH;

    const int lh_en = Font16.Height;
    const int lh_cn = Font12CN.Height;

    // Modify: Correct content area start Y coordinate to ensure sufficient spacing from footer
    int y = CONTENT_Y_START;  // Removed +10 extra margin to bring text closer to top
    const int text_bottom = FOOTER_Y_START-5; // Adjust to -5 to ensure sufficient spacing from footer

    // Use processed text
    size_t i = start_offset;

    // Record starting position before entering loop
    size_t initial_i = i;

    while (i < g_processed_text_size && y < text_bottom) {
        int x = left_margin;
        char line[512] = {0};
        int len = 0;
        int has_cn = 0;

        // Build a line of text until reaching maximum width or encountering paragraph end marker
        while (i < g_processed_text_size) {
            unsigned char c = (unsigned char)g_processed_text[i];
            // Encounter paragraph end marker, move to next line
            if (c == '\n') {
                i++;  // Skip paragraph end marker
                // First line after paragraph needs indent, so set x to indent distance
                x = left_margin + (Font16.Width * 2);  // Indent two character widths
                continue;  // Continue to next iteration
            }

            // Modify: Use boundary-checked character length detection
            int bytes = char_processor.char_len(g_processed_text, g_processed_text_size, i);

            int width = (bytes > 1) ? Font12CN.Width : Font16.Width;

            if (x + width > max_x)
                break;  // Reached line width limit, move to next line

            for (int k = 0; k < bytes && i + k < g_processed_text_size; k++)
                line[len++] = g_processed_text[i + k];

            if (bytes > 1) has_cn = 1;
            i += bytes;
            x += width;
        }

        if (len > 0) {
            int lh = has_cn ? lh_cn : lh_en;
            if (y + lh > text_bottom)
                break;

            line[len] = '\0';
            if (has_cn)
                Paint_DrawString_CN(left_margin, y, line, &Font12CN, WHITE,BLACK);
            else
                Paint_DrawString_EN(left_margin, y, line, &Font16, BLACK, WHITE);

            y += lh;
        }

        // If no progress was made in the loop (i did not increase), break to prevent infinite loop
        if (i == initial_i && i < g_processed_text_size) {
            // Skip one character to prevent infinite loop
            int char_bytes = char_processor.char_len(g_processed_text, g_processed_text_size, i);
            i += char_bytes;  // Skip the character based on detected encoding
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

    Paint_DrawString_EN(
        EPD_7IN5_V2_WIDTH - 160,
        FOOTER_Y_START+5,  // Adjust page number Y coordinate to avoid overlapping with content
        page,
        &Font16,
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
    return i;  // Return actual ending offset
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
    GUI_ReadBmp_Scale_Centered("/home/pi/e-ink-reader/demo-inkscreen-reader/components/e-Paper/Quectel-Pi-H1/c/pic/2.bmp", 0, 0,EPD_7IN5_V2_WIDTH,EPD_7IN5_V2_HEIGHT,0.7) ;
    
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

    // Use fast recovery: directly refresh current page
      if (g_frame_buffer && g_processed_text) {
        // Directly call display_txt_page_from_offset, which will redraw Header based on header_drawn=0
        display_txt_page_from_offset(g_current_char_offset);
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
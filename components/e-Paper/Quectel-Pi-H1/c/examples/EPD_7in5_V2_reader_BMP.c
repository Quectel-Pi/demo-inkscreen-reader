// examples/EPD_7in5_V2_reader.c
#include "EPD_7in5_V2.h"
#include "GUI_Paint.h"
#include "GUI_BMPfile.h"
#include "DEV_Config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <dirent.h>

// ==================== 配置 ====================
#define BOOK_PATH      "/tools/books"
#define CACHE_DIR      "/tools/cache"
#define MAX_PAGES      500
#define REFRESH_CYCLE  5  // 每5页全刷一次

// ==================== 全局变量 ====================
static int current_page = 0;
static int total_pages = 0;
static char current_file[256] = {0};
static UBYTE *g_frame_buffer = NULL;
static int key1_fd = -1; // KEY1: event2 -> next
static int key2_fd = -1; // KEY2: event1 -> prev
static int first_display_done = 0;

// ==================== 工具函数 ====================
void create_cache_dir(void) {
    mkdir(CACHE_DIR, 0755);
}

const char* get_ext(const char* filename) {
    const char* dot = strrchr(filename, '.');
    return (dot && dot != filename) ? dot + 1 : "";
}

int render_pdf_to_bmp(const char* pdf_path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "mkdir -p %s && "
        "/tools/usr/bin/mutool draw  -r 200 -o %s/page-%%d.png '%s' >/dev/null 2>&1 && "
        "for f in %s/page-*.png; do "
            "[ -f \"$f\" ] || continue; "
            "ppm=\"${f%%.png}.ppm\"; bmp=\"${f%%.png}.bmp\"; "
            "ffmpeg -y -i \"$f\" -vf \"scale=800:480:force_original_aspect_ratio=disable\" -f image2 -vcodec ppm \"$ppm\" >/dev/null 2>&1 && "
            "/c/lib/png2bmp/ppm2bmp1bit \"$ppm\" \"$bmp\" && "
            "rm -f \"$f\" \"$ppm\"; "
        "done",
        CACHE_DIR, CACHE_DIR, pdf_path, CACHE_DIR
    );
    return system(cmd);
}

int scan_cached_pages(void) {
    int count = 0;
    char path[256];
    for (int i = 1; i <= MAX_PAGES; i++) {
        snprintf(path, sizeof(path), "%s/page-%d.bmp", CACHE_DIR, i);
        if (access(path, F_OK) != 0) break;
        count++;
    }
    return count;
}

void show_error(const char* msg) {
    Paint_SelectImage(g_frame_buffer);
    Paint_Clear(WHITE);
    Paint_DrawString_EN(10, 10, "ERROR", &Font24, BLACK, WHITE);
    Paint_DrawString_EN(10, 40, msg, &Font16, BLACK, WHITE);
    EPD_7IN5_V2_Display(g_frame_buffer);
    sleep(3);
}

void display_page_info(int page_num) {
    Paint_SelectImage(g_frame_buffer);
    Paint_Clear(WHITE);
    Paint_DrawString_EN(10, 10, "E-Ink Reader", &Font20, BLACK, WHITE);
    Paint_DrawString_EN(10, 40, "Page:", &Font16, BLACK, WHITE);
    Paint_DrawNum(70, 40, page_num + 1, &Font16, BLACK, WHITE);
    Paint_DrawString_EN(10, 70, "File:", &Font12, BLACK, WHITE);
    Paint_DrawString_EN(10, 85, current_file, &Font12, BLACK, WHITE);
}

// ==================== 显示页面 ====================
void display_page(int page) {
    if (page < 0 || page >= total_pages) return;

    char bmp_path[256];
    snprintf(bmp_path, sizeof(bmp_path), "%s/page-%d.bmp", CACHE_DIR, page + 1);

    Paint_SelectImage(g_frame_buffer);
    Paint_Clear(WHITE);

    if (access(bmp_path, F_OK) == 0) {
        GUI_ReadBmp(bmp_path, 0, 0);
    } else {
        display_page_info(page);
    }

    // ========== 关键：首次显示必须 Clear + Display ==========
    if (!first_display_done) {
        printf("First display: full clear + show page %d\n", page + 1);
        EPD_7IN5_V2_Init();
        EPD_7IN5_V2_Clear();           // 全刷清白
        DEV_Delay_ms(5000);            // 等待稳定
        EPD_7IN5_V2_Display(g_frame_buffer); // 显示内容
        first_display_done = 1;
    } else {
        // 后续翻页：快刷
        if ((page + 1) % REFRESH_CYCLE == 0) {
            printf("Full refresh at page %d\n", page + 1);
            EPD_7IN5_V2_Init();
            EPD_7IN5_V2_Clear();
            DEV_Delay_ms(1000);
            EPD_7IN5_V2_Display(g_frame_buffer);
        } else {
            EPD_7IN5_V2_Display(g_frame_buffer); // 快刷
        }
    }

    current_page = page;
}

// ==================== 按键处理 ====================
void handle_keys(void) {
    struct input_event ev;
    // KEY2: prev (event1)
    while (read(key2_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_KEY && ev.value == 1) {
            if (current_page > 0) {
                display_page(current_page - 1);
            }
            break;
        }
    }
    // KEY1: next (event2)
    while (read(key1_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_KEY && ev.value == 1) {
            if (current_page < total_pages - 1) {
                display_page(current_page + 1);
            }
            break;
        }
    }
}

// ==================== 主函数 ====================
void EPD_7in5_V2_reader_test(void) {
    printf("E-Ink Book Reader - Waveshare 7.5inch V2 (B/W)\n");

    // ========== 1. 硬件初始化 ==========
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
        // ========== 2. 按键初始化 ==========
    key1_fd = open("/dev/input/event3", O_RDONLY | O_NONBLOCK);
    key2_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (key1_fd < 0 || key2_fd < 0) {
        printf("Key devices not found\n");
        goto cleanup;
    }

    // ========== 3. 扫描书籍 ==========
    DIR* dir = opendir(BOOK_PATH);
    if (!dir) {
        show_error("Books dir not found");
        goto cleanup;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            const char* ext = get_ext(entry->d_name);
            if (strcasecmp(ext, "pdf") == 0) {
                snprintf(current_file, sizeof(current_file), "%s/%s", BOOK_PATH, entry->d_name);
                break;
            }
        }
    }
    closedir(dir);
    if (current_file[0] == 0) {
        show_error("No PDF found");
        goto cleanup;
    }

    // ========== 4. 渲染 PDF ==========
    create_cache_dir();
    if (render_pdf_to_bmp(current_file) != 0) {
        show_error("PDF render failed");
        goto cleanup;
    }
    total_pages = scan_cached_pages();
    if (total_pages <= 0) {
        show_error("No pages rendered");
        goto cleanup;
    }
    // ========== 5. 分配帧缓冲 ==========
    UDOUBLE Imagesize = ((EPD_7IN5_V2_WIDTH % 8 == 0) ?
        (EPD_7IN5_V2_WIDTH / 8) : (EPD_7IN5_V2_WIDTH / 8 + 1)) * EPD_7IN5_V2_HEIGHT;
    g_frame_buffer = (UBYTE *)malloc(Imagesize);
    if (!g_frame_buffer) {
        printf("Malloc failed\n");
        return;
    }
    Paint_NewImage(g_frame_buffer, EPD_7IN5_V2_WIDTH, EPD_7IN5_V2_HEIGHT, ROTATE_0, WHITE);



    // ========== 6. 显示第一页 ==========
    display_page(0);

    // ========== 7. 主循环 ==========
    printf("Reader started. Total pages: %d\n", total_pages);
    while (1) {
        handle_keys();
        usleep(50000); // 50ms poll
    }

cleanup:
    free(g_frame_buffer);
    if (key1_fd >= 0) close(key1_fd);
    if (key2_fd >= 0) close(key2_fd);
    EPD_7IN5_V2_Sleep();
}
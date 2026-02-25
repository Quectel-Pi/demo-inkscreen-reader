// ppm2bmp1bit.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;      // "BM"
    uint32_t bfSize;      // file size
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;   // 62
} BMPHeader;

typedef struct {
    uint32_t biSize;           // 40
    int32_t  biWidth;          // 800
    int32_t  biHeight;         // 480
    uint16_t biPlanes;         // 1
    uint16_t biBitCount;       // 1
    uint32_t biCompression;    // 0
    uint32_t biSizeImage;      // row_bytes * 480
    int32_t  biXPelsPerMeter;  // 2835
    int32_t  biYPelsPerMeter;  // 2835
    uint32_t biClrUsed;        // 2
    uint32_t biClrImportant;   // 0
} BMPInfoHeader;
#pragma pack(pop)

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s input.ppm output.bmp\n", argv[0]);
        return 1;
    }

    FILE* fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen ppm"); return 1; }

    // Read magic "P6"
    char magic[3];
    if (fread(magic, 1, 2, fp) != 2 || memcmp(magic, "P6", 2) != 0) {
        fprintf(stderr, "Error: Only P6 PPM supported\n");
        fclose(fp);
        return 1;
    }

    // Skip comments (lines starting with #)
    int ch;
    while ((ch = fgetc(fp)) == '#') {
        while ((ch = fgetc(fp)) != '\n' && ch != EOF);
    }
    ungetc(ch, fp);

    int width, height, maxval;
    if (fscanf(fp, "%d %d %d", &width, &height, &maxval) != 3) {
        fprintf(stderr, "Failed to parse PPM header\n");
        fclose(fp);
        return 1;
    }
    fgetc(fp); // consume newline

    if (width != 800 || height != 480) {
        fprintf(stderr, "Error: Input must be 800x480 (got %dx%d)\n", width, height);
        fclose(fp);
        return 1;
    }

    size_t pixel_bytes = width * height * 3;
    uint8_t* rgb = malloc(pixel_bytes);
    if (!rgb) { perror("malloc"); fclose(fp); return 1; }

    if (fread(rgb, 1, pixel_bytes, fp) != pixel_bytes) {
        fprintf(stderr, "Failed to read pixel data\n");
        free(rgb);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    // BMP row must be aligned to 4-byte boundary
    int row_bytes = (width + 31) / 32 * 4; // = 100 for 800
    uint8_t* bmp_data = calloc(row_bytes * height, 1);
    if (!bmp_data) { perror("calloc"); free(rgb); return 1; }

    // Initialize as white (0xFF = all bits 1)
    memset(bmp_data, 0xFF, row_bytes * height);

    // Convert RGB to 1-bit (0=black, 1=white), flip vertically
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int src_y = height - 1 - y; // BMP is bottom-up
            uint8_t* pixel = &rgb[(src_y * width + x) * 3];
            uint8_t gray = (uint8_t)(0.299f * pixel[0] + 0.587f * pixel[1] + 0.114f * pixel[2]);
            if (gray < 128) {
                int byte_idx = y * row_bytes + x / 8;
                int bit_idx = 7 - (x % 8); // MSB first
                bmp_data[byte_idx] &= ~(1U << bit_idx);
            }
        }
    }

    // Write BMP file
    FILE* out = fopen(argv[2], "wb");
    if (!out) { perror("fopen bmp"); free(rgb); free(bmp_data); return 1; }

    BMPHeader hdr = {0x4D42, 62 + (uint32_t)(row_bytes * height), 0, 0, 62};
    BMPInfoHeader info = {40, 800, 480, 1, 1, 0, (uint32_t)(row_bytes * height), 2835, 2835, 2, 0};

    fwrite(&hdr, sizeof(hdr), 1, out);
    fwrite(&info, sizeof(info), 1, out);
    uint32_t palette[2] = {0x00000000, 0x00FFFFFF};
    fwrite(palette, sizeof(uint32_t), 2, out);
    fwrite(bmp_data, 1, row_bytes * height, out);

    fclose(out);
    free(rgb);
    free(bmp_data);
    return 0;
}
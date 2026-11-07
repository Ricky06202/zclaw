#include "wt32_display.h"

#if CONFIG_ZCLAW_WT32_SC01_DISPLAY

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "wt32_display"

#define LCD_WIDTH 480
#define LCD_HEIGHT 320
#define DRAW_CHUNK_ROWS 8
#define DRAW_BUFFER_PIXELS (LCD_WIDTH * DRAW_CHUNK_ROWS)
#define GLYPH_SCALE_MAX 8
#define GLYPH_BUFFER_PIXELS (5 * GLYPH_SCALE_MAX * 7 * GLYPH_SCALE_MAX)
#define CHAT_MAX_LINES 18
#define CHAT_LINE_CHARS 38
#define CHAT_FONT_SCALE 2
#define CHAT_GLYPH_HEIGHT (7 * CHAT_FONT_SCALE)
#define CHAT_LINE_HEIGHT 16
#define CHAT_BODY_Y 32
#define CHAT_LEFT_MARGIN 8
#define CHAT_CHAR_ADVANCE (6 * CHAT_FONT_SCALE)

#define COLOR_BACKGROUND 0x0010
#define COLOR_TEXT 0xFFFF
#define COLOR_ACCENT 0x07FF
#define COLOR_CHAT_HEADER 0x18C3
#define COLOR_CHAT_USER 0x07FF
#define COLOR_CHAT_ASSISTANT 0xBFE0

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_color_done;
static SemaphoreHandle_t s_chat_mutex;
static bool s_initialized;

DMA_ATTR static uint16_t s_draw_buffer[DRAW_BUFFER_PIXELS];
DMA_ATTR static uint16_t s_glyph_buffer[GLYPH_BUFFER_PIXELS];

typedef struct {
    char text[CHAT_LINE_CHARS + 1];
    uint16_t color;
} chat_line_t;

static chat_line_t s_chat_lines[CHAT_MAX_LINES];
static size_t s_chat_line_start;
static size_t s_chat_line_count;

/* Five-column, seven-row uppercase glyphs for the short boot splash. */
static const uint8_t s_font5x7[26][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* A */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* B */
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, /* C */
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, /* D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, /* E */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, /* F */
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, /* G */
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* H */
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* I */
    {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C}, /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* L */
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, /* M */
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, /* N */
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* O */
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, /* P */
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, /* Q */
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, /* S */
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, /* W */
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, /* Y */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Z */
};

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *event_data,
                                void *user_ctx)
{
    (void)panel_io;
    (void)event_data;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static esp_err_t draw_bitmap_and_wait(int x_start, int y_start, int x_end, int y_end,
                                      const uint16_t *pixels)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, pixels);
    if (err != ESP_OK) {
        return err;
    }
    return xSemaphoreTake(s_color_done, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t fill_rect(int x, int y, int width, int height, uint16_t color)
{
    for (int row_start = 0; row_start < height; row_start += DRAW_CHUNK_ROWS) {
        int rows = height - row_start;
        if (rows > DRAW_CHUNK_ROWS) {
            rows = DRAW_CHUNK_ROWS;
        }
        for (int i = 0; i < width * rows; i++) {
            s_draw_buffer[i] = color;
        }
        esp_err_t err = draw_bitmap_and_wait(x, y + row_start, x + width,
                                             y + row_start + rows, s_draw_buffer);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t draw_text(const char *text, int y, int scale, uint16_t color)
{
    const int glyph_width = 5 * scale;
    const int glyph_height = 7 * scale;
    const int advance = 6 * scale;
    const size_t length = strlen(text);
    const int text_width = (int)length * advance - scale;
    int x = (LCD_WIDTH - text_width) / 2;

    for (size_t character = 0; character < length; character++) {
        char ch = text[character];
        if (ch >= 'A' && ch <= 'Z') {
            const uint8_t *glyph = s_font5x7[ch - 'A'];
            for (int py = 0; py < glyph_height; py++) {
                for (int px = 0; px < glyph_width; px++) {
                    int source_x = px / scale;
                    int source_y = py / scale;
                    bool set = (glyph[source_y] & (1U << (4 - source_x))) != 0;
                    s_glyph_buffer[py * glyph_width + px] = set ? color : COLOR_BACKGROUND;
                }
            }
            esp_err_t err = draw_bitmap_and_wait(x, y, x + glyph_width, y + glyph_height,
                                                 s_glyph_buffer);
            if (err != ESP_OK) {
                return err;
            }
        }
        x += advance;
    }
    return ESP_OK;
}

static const uint8_t s_font5x7_digits[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, /* 0 */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, /* 2 */
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}, /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, /* 4 */
    {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}, /* 5 */
    {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}, /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, /* 8 */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E}, /* 9 */
};

static const uint8_t *chat_glyph_for_char(char ch)
{
    static const uint8_t blank[7] = {0};
    static const uint8_t punctuation[17][7] = {
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}, /* . */
        {0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x08}, /* , */
        {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}, /* : */
        {0x04, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00}, /* ' */
        {0x0A, 0x0A, 0x05, 0x00, 0x00, 0x00, 0x00}, /* " */
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}, /* ? */
        {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}, /* ! */
        {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, /* - */
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}, /* _ */
        {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}, /* ( */
        {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}, /* ) */
        {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}, /* / */
        {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}, /* + */
        {0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00, 0x00}, /* = */
        {0x00, 0x11, 0x0A, 0x1F, 0x0A, 0x11, 0x00}, /* * */
        {0x0E, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0E}, /* [ */
        {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}, /* ] */
    };

    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    if (ch >= 'A' && ch <= 'Z') {
        return s_font5x7[ch - 'A'];
    }
    if (ch >= '0' && ch <= '9') {
        return s_font5x7_digits[ch - '0'];
    }

    switch (ch) {
        case ' ': return blank;
        case '.': return punctuation[0];
        case ',': return punctuation[1];
        case ':': return punctuation[2];
        case '\'': return punctuation[3];
        case '"': return punctuation[4];
        case '?': return punctuation[5];
        case '!': return punctuation[6];
        case '-': return punctuation[7];
        case '_': return punctuation[8];
        case '(': return punctuation[9];
        case ')': return punctuation[10];
        case '/': return punctuation[11];
        case '+': return punctuation[12];
        case '=': return punctuation[13];
        case '*': return punctuation[14];
        case '[': return punctuation[15];
        case ']': return punctuation[16];
        default: return punctuation[5];
    }
}

static char normalize_chat_char(const unsigned char **cursor)
{
    const unsigned char *p = *cursor;
    unsigned char first = *p++;
    char ch = '?';

    if (first < 0x80) {
        ch = (char)first;
    } else if (first == 0xC3 && *p != '\0') {
        switch (*p++) {
            case 0x81: case 0xA1: ch = 'A'; break; /* Á á */
            case 0x89: case 0xA9: ch = 'E'; break; /* É é */
            case 0x8D: case 0xAD: ch = 'I'; break; /* Í í */
            case 0x93: case 0xB3: ch = 'O'; break; /* Ó ó */
            case 0x9A: case 0xBA: case 0x9C: case 0xBC: ch = 'U'; break; /* Ú ú Ü ü */
            case 0x91: case 0xB1: ch = 'N'; break; /* Ñ ñ */
            default: break;
        }
    } else if (first == 0xC2 && *p != '\0') {
        unsigned char second = *p++;
        if (second == 0xBF) ch = '?';      /* ¿ */
        else if (second == 0xA1) ch = '!'; /* ¡ */
    } else {
        while ((*p & 0xC0) == 0x80) {
            p++;
        }
    }

    *cursor = p;
    if (ch == '\t') {
        return ' ';
    }
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    return ch;
}

static void push_chat_line(const char *text, uint16_t color)
{
    size_t index;
    if (s_chat_line_count < CHAT_MAX_LINES) {
        index = (s_chat_line_start + s_chat_line_count) % CHAT_MAX_LINES;
        s_chat_line_count++;
    } else {
        index = s_chat_line_start;
        s_chat_line_start = (s_chat_line_start + 1) % CHAT_MAX_LINES;
    }
    strncpy(s_chat_lines[index].text, text, CHAT_LINE_CHARS);
    s_chat_lines[index].text[CHAT_LINE_CHARS] = '\0';
    s_chat_lines[index].color = color;
}

static esp_err_t draw_chat_line(const chat_line_t *line, int y)
{
    const int glyph_width = 5 * CHAT_FONT_SCALE;
    const int glyph_height = CHAT_GLYPH_HEIGHT;
    const size_t length = strlen(line->text);

    for (int chunk_y = 0; chunk_y < glyph_height; chunk_y += DRAW_CHUNK_ROWS) {
        int chunk_rows = glyph_height - chunk_y;
        if (chunk_rows > DRAW_CHUNK_ROWS) {
            chunk_rows = DRAW_CHUNK_ROWS;
        }
        size_t pixels = (size_t)LCD_WIDTH * (size_t)chunk_rows;
        for (size_t i = 0; i < pixels; i++) {
            s_draw_buffer[i] = COLOR_BACKGROUND;
        }

        for (size_t ci = 0; ci < length; ci++) {
            const uint8_t *glyph = chat_glyph_for_char(line->text[ci]);
            int glyph_x = CHAT_LEFT_MARGIN + (int)ci * CHAT_CHAR_ADVANCE;
            for (int py = chunk_y; py < chunk_y + chunk_rows; py++) {
                int source_y = py / CHAT_FONT_SCALE;
                for (int px = 0; px < glyph_width; px++) {
                    int source_x = px / CHAT_FONT_SCALE;
                    if ((glyph[source_y] & (1U << (4 - source_x))) == 0) {
                        continue;
                    }
                    int x = glyph_x + px;
                    if (x >= 0 && x < LCD_WIDTH) {
                        s_draw_buffer[(size_t)(py - chunk_y) * LCD_WIDTH + x] = line->color;
                    }
                }
            }
        }

        esp_err_t err = draw_bitmap_and_wait(0, y + chunk_y, LCD_WIDTH,
                                             y + chunk_y + chunk_rows, s_draw_buffer);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t render_chat(void)
{
    esp_err_t err = fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_BACKGROUND);
    if (err == ESP_OK) {
        err = fill_rect(0, 0, LCD_WIDTH, CHAT_BODY_Y, COLOR_CHAT_HEADER);
    }
    if (err == ESP_OK) {
        err = draw_text("DEEPSEEK CHAT", 7, 3, COLOR_TEXT);
    }
    for (size_t i = 0; err == ESP_OK && i < s_chat_line_count; i++) {
        size_t index = (s_chat_line_start + i) % CHAT_MAX_LINES;
        err = draw_chat_line(&s_chat_lines[index], CHAT_BODY_Y + (int)i * CHAT_LINE_HEIGHT);
    }
    return err;
}

esp_err_t wt32_display_append_chat_message(wt32_display_chat_role_t role, const char *text)
{
    if (!s_initialized || s_chat_mutex == NULL || text == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (role != WT32_DISPLAY_CHAT_USER && role != WT32_DISPLAY_CHAT_ASSISTANT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_chat_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const char *label = role == WT32_DISPLAY_CHAT_USER ? "TU: " : "DEEPSEEK: ";
    uint16_t color = role == WT32_DISPLAY_CHAT_USER ? COLOR_CHAT_USER : COLOR_CHAT_ASSISTANT;
    size_t label_len = strlen(label);
    char line[CHAT_LINE_CHARS + 1] = {0};
    memcpy(line, label, label_len);
    size_t line_len = label_len;
    bool pushed = false;

    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor != '\0') {
        char ch = normalize_chat_char(&cursor);
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            line[line_len] = '\0';
            push_chat_line(line, color);
            pushed = true;
            memset(line, ' ', label_len);
            line_len = label_len;
            continue;
        }
        if (line_len == CHAT_LINE_CHARS) {
            line[line_len] = '\0';
            push_chat_line(line, color);
            pushed = true;
            memset(line, ' ', label_len);
            line_len = label_len;
        }
        line[line_len++] = ch;
    }
    if (line_len > label_len || !pushed) {
        line[line_len] = '\0';
        push_chat_line(line, color);
    }

    esp_err_t err = render_chat();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Chat render failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_chat_mutex);
    return err;
}

esp_err_t wt32_display_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_color_done = xSemaphoreCreateBinary();
    s_chat_mutex = xSemaphoreCreateMutex();
    if (s_color_done == NULL || s_chat_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << GPIO_NUM_23,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&backlight_config);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_set_level(GPIO_NUM_23, 1);
    if (err != ESP_OK) {
        return err;
    }

    const spi_bus_config_t bus_config = {
        .sclk_io_num = GPIO_NUM_14,
        .mosi_io_num = GPIO_NUM_13,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DRAW_BUFFER_PIXELS * sizeof(uint16_t),
    };
    err = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = GPIO_NUM_21,
        .cs_gpio_num = GPIO_NUM_15,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = s_color_done,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI panel I/O init failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_22,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7796(s_panel_io, &panel_config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7796 panel creation failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(s_panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_swap_xy(s_panel, true);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(s_panel, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7796 initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    err = fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_BACKGROUND);
    if (err == ESP_OK) {
        err = draw_text("HOLA PAPA", 108, 8, COLOR_TEXT);
    }
    if (err == ESP_OK) {
        err = fill_rect(78, 176, LCD_WIDTH - 156, 4, COLOR_ACCENT);
    }
    if (err == ESP_OK) {
        err = draw_text("ZCLAW", 195, 3, COLOR_TEXT);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Boot splash draw failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WT32-SC01 ST7796 initialized; boot splash drawn");
    return ESP_OK;
}

#else

esp_err_t wt32_display_init(void)
{
    return ESP_OK;
}

#endif

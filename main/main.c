#include <stdio.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "hal/gpio_types.h"
#include "hal/spi_types.h"
#include "string.h"
#include "jpeg_decoder.h"

#include "hello.c"

#define PIN_NUM_SCLK 12
#define PIN_NUM_MOSI 11
#define PIN_NUM_MISO 13
#define PIN_NUM_CS 10
#define PIN_NUM_RST 8
#define PIN_NUM_DC 9

#define LCD_PIXEL_CLOCK_HZ 20000000
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8

#define LCD_H_RES 240
#define LCD_V_RES 320

#define FRAME_BUFFER_SIZE (LCD_H_RES * LCD_V_RES * sizeof(uint16_t))

extern const uint8_t frame001_jpg_start[] asm("_binary_frame001_jpg_start");
extern const uint8_t frame001_jpg_end[] asm("_binary_frame001_jpg_end");

extern const uint8_t frame002_jpg_start[] asm("_binary_frame002_jpg_start");
extern const uint8_t frame002_jpg_end[] asm("_binary_frame002_jpg_end");

extern const uint8_t frame003_jpg_start[] asm("_binary_frame003_jpg_start");
extern const uint8_t frame003_jpg_end[] asm("_binary_frame003_jpg_end");

extern const uint8_t frame004_jpg_start[] asm("_binary_frame004_jpg_start");
extern const uint8_t frame004_jpg_end[] asm("_binary_frame004_jpg_end");

esp_lcd_panel_io_handle_t io_handle = NULL;
esp_lcd_panel_handle_t lcd_panel_handle = NULL;
static uint16_t *frame_buffer = NULL;

static void Display_Init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FRAME_BUFFER_SIZE,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO)); // Enable the DMA feature

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &lcd_panel_handle));

    esp_lcd_panel_reset(lcd_panel_handle);
    ESP_LOGI("RGB", "panel reset done");
    esp_lcd_panel_init(lcd_panel_handle);
    ESP_LOGI("RGB", "panel init done");
    esp_lcd_panel_disp_on_off(lcd_panel_handle, true);
    ESP_LOGI("RGB", "disp on done");
}

static void Frame_Buffer_Init(void)
{
    frame_buffer = heap_caps_malloc(FRAME_BUFFER_SIZE, MALLOC_CAP_DMA);
    if (frame_buffer == NULL)
    {
        ESP_LOGE("FrameBuffer", "Failed to allocate frame buffer");
        return;
    }
    memset(frame_buffer, 0x00, FRAME_BUFFER_SIZE); // Clear the frame buffer
}

static uint16_t RGB565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) |
           ((g & 0xFC) << 3) |
           ((b & 0xF8) >> 3);
}
static void Fill_Frame(uint16_t color)
{
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++)
    {
        frame_buffer[i] = color;
    }
}

static void Draw_rectangle(uint16_t x_start, uint16_t y_start, uint16_t width, uint16_t height, uint16_t color)
{
    for (uint16_t j = y_start; j < y_start + height; j++)
    {
        for (uint16_t x = x_start; x < x_start + width; x++)
        {
            if (x < LCD_H_RES && j < LCD_V_RES)
            {
                frame_buffer[j * LCD_H_RES + x] = color;
            }
        }
    }
}

static esp_err_t Decode_JPEG(const uint8_t *jpeg_data, size_t jpeg_size)
{
    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = (uint8_t *)jpeg_data,
        .indata_size = jpeg_size,
        .outbuf = (uint8_t *)frame_buffer,
        .outbuf_size = FRAME_BUFFER_SIZE,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {
            .swap_color_bytes = 1,
        },
    };

    esp_jpeg_image_output_t output_image = {0};

    esp_err_t ret = esp_jpeg_decode(&jpeg_cfg, &output_image);

    if (ret != ESP_OK)
    {
        ESP_LOGE("JPEG", "JPEG decode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI("JPEG", "JPEG decoded: %lu x %lu, output = %lu bytes",
             (unsigned long)output_image.width,
             (unsigned long)output_image.height,
             (unsigned long)output_image.output_len);

    return ESP_OK;
}

static bool Play_Frame(const char *name, const uint8_t *jpg_start, const uint8_t *jpg_end)
{
    size_t jpg_size = jpg_end - jpg_start;

    ESP_LOGI("VIDEO", "Playing %s, JPEG size = %u bytes", name, (unsigned int)jpg_size);

    esp_err_t ret = Decode_JPEG(jpg_start, jpg_size);

    if (ret != ESP_OK)
    {
        ESP_LOGE("VIDEO", "%s decode FAILED: %s", name, esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI("VIDEO", "%s decode SUCCESS", name);
    ret = esp_lcd_panel_draw_bitmap(lcd_panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, frame_buffer);

    if (ret != ESP_OK)
    {
        ESP_LOGE("VIDEO", "%s decode FAILED: %s", name, esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI("VIDEO", "%s displayed", name);

    return true;
}
void app_main(void)
{
    Display_Init();
    Frame_Buffer_Init();

    while (1)
    {
        Play_Frame("frame001", frame001_jpg_start, frame001_jpg_end);

        vTaskDelay(pdMS_TO_TICKS(500));

        Play_Frame("frame002", frame002_jpg_start, frame002_jpg_end);

        vTaskDelay(pdMS_TO_TICKS(500));

        Play_Frame("frame003", frame003_jpg_start, frame003_jpg_end);

        vTaskDelay(pdMS_TO_TICKS(500));

        Play_Frame("frame004", frame004_jpg_start, frame004_jpg_end);

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /****** FOR Single Image *****/
    // size_t frame001_size = frame001_jpg_end - frame001_jpg_start;
    // ESP_LOGI("MAIN", "JPEG size = %lu bytes", (unsigned long)frame001_size);

    // ESP_ERROR_CHECK(Decode_JPEG(frame001_jpg_start, frame001_size));
    // esp_lcd_panel_draw_bitmap(lcd_panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, frame_buffer);

    // while (1)
    // {
    //     vTaskDelay(pdMS_TO_TICKS(1000));
    // }

    /****** FOR Rectangles as drawn using the function *****/
    // while (1)
    // {
    //     Fill_Frame(RGB565(0, 0, 0));
    //     esp_lcd_panel_draw_bitmap(lcd_panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, frame_buffer);
    //     vTaskDelay(pdMS_TO_TICKS(500));

    //     Draw_rectangle(20, 100, 50, 50, RGB565(255, 0, 0));
    //     esp_lcd_panel_draw_bitmap(lcd_panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, frame_buffer);
    //     vTaskDelay(pdMS_TO_TICKS(500));

    //     Draw_rectangle(80, 100, 50, 50, RGB565(0, 255, 0));
    //     esp_lcd_panel_draw_bitmap(lcd_panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, frame_buffer);
    //     vTaskDelay(pdMS_TO_TICKS(500));

    //     Draw_rectangle(140, 100, 50, 50, RGB565(0, 0, 255));
    //     esp_lcd_panel_draw_bitmap(lcd_panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, frame_buffer);
    //     vTaskDelay(pdMS_TO_TICKS(500));
    // }
}

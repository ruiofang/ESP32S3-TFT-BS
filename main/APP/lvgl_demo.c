#include "lvgl_demo.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "spi.h"
#include "lcd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lv_demos.h"

#define MY_DISP_HOR_RES  428
#define MY_DISP_VER_RES 142

static void increase_lvgl_tick(void *arg);
static void disp_init(void);
static void disp_flush(lv_display_t * disp_drv, const lv_area_t * area, uint8_t * px_map);
void lv_port_disp_init(void);

static const char *TAG = "LVGL";
#define BYTE_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565)) /*will be 2 for RGB565 */

void lvgl_task(void *pvParameter);
TaskHandle_t lvgl_task_handle;
/** 
 * @brief       lvgl_demo入口函数
 * @param       无
 * @retval      无
 */
void lvgl_demo(void)
{
    lv_init();              /* 初始化LVGL图形库 */
    lv_port_disp_init();
    /* 为LVGL提供时基单元 */
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 1 * 1000));

    // 创建lvgl任务
    xTaskCreate(lvgl_task, "lvgl_task", 1024*10, NULL, 1, &lvgl_task_handle);
}

void lv_port_disp_init(void)
{
    /*-------------------------
     * Initialize your display
     * -----------------------*/
    disp_init();

    /*------------------------------------
     * Create a display and set a flush_cb
     * -----------------------------------*/
    lv_display_t * disp = lv_display_create(MY_DISP_HOR_RES, MY_DISP_VER_RES);
    lv_display_set_flush_cb(disp, disp_flush);
    // lv_display_set_bpp(disp, 16);
    /* Example 1
     * One buffer for partial rendering*/
    // LV_ATTRIBUTE_MEM_ALIGN
    // static uint8_t buf_1_1[MY_DISP_HOR_RES * 10 * BYTE_PER_PIXEL];            /*A buffer for 10 rows*/
    // lv_display_set_buffers(disp, buf_1_1, NULL, sizeof(buf_1_1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Example 2
     * Two buffers for partial rendering
     * In flush_cb DMA or similar hardware should be used to update the display in the background.*/
    // LV_ATTRIBUTE_MEM_ALIGN
    // static uint8_t buf_2_1[MY_DISP_HOR_RES * 10 * BYTE_PER_PIXEL];

    // LV_ATTRIBUTE_MEM_ALIGN
    // static uint8_t buf_2_2[MY_DISP_HOR_RES * 10 * BYTE_PER_PIXEL];
    
    // LVGL 缓冲: 放在内部 DMA-capable RAM, 让 SPI 可以直接 DMA 传输,
    // 避免每帧分配 ~8KB 的 bounce buffer (BLE/WiFi 运行后内部 RAM 紧张)。
    // 4 行 ~3.4 KB/buffer x 2 = 6.8 KB, 在 32 KB DRAM 里可控。
    uint32_t buf_size = MY_DISP_HOR_RES * 4 * BYTE_PER_PIXEL;
    uint8_t* buf_2_1 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    uint8_t* buf_2_2 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

    if (buf_2_1 == NULL || buf_2_2 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffers in DMA RAM, trying PSRAM");
        if (buf_2_1) free(buf_2_1);
        if (buf_2_2) free(buf_2_2);
        buf_2_1 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        buf_2_2 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf_2_1 == NULL || buf_2_2 == NULL) {
            ESP_LOGE(TAG, "Failed to allocate LVGL buffers");
            return;
        }
        ESP_LOGW(TAG, "LVGL buffers allocated in PSRAM (SPI will need bounce buffer)");
    } else {
        ESP_LOGI(TAG, "LVGL buffers allocated in internal DMA RAM (%u bytes each)",
                 (unsigned)buf_size);
    }
    
    lv_display_set_buffers(disp, buf_2_1, buf_2_2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Example 3
     * Two buffers screen sized buffer for double buffering.
     * Both LV_DISPLAY_RENDER_MODE_DIRECT and LV_DISPLAY_RENDER_MODE_FULL works, see their comments*/
    // LV_ATTRIBUTE_MEM_ALIGN
    // static uint8_t buf_3_1[MY_DISP_HOR_RES * MY_DISP_VER_RES * BYTE_PER_PIXEL];

    // LV_ATTRIBUTE_MEM_ALIGN
    // static uint8_t buf_3_2[MY_DISP_HOR_RES * MY_DISP_VER_RES * BYTE_PER_PIXEL];
    // lv_display_set_buffers(disp, buf_3_1, buf_3_2, sizeof(buf_3_1), LV_DISPLAY_RENDER_MODE_DIRECT);

}

// lvgl任务
void lvgl_task(void *pvParameter) {
    ESP_LOGI(TAG, "lvgl_task");
    
    // lv_obj_t *label = lv_label_create(lv_screen_active());
    // lv_label_set_text(label,"hello world");
    // lv_obj_align(label,LV_ALIGN_CENTER, 0, 0);
    lv_demo_widgets();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        lv_timer_handler(); 
        // if(xSemaphoreTake(gui_MuxSem_Handle, portMAX_DELAY) == pdPASS){
        //     lv_timer_handler();             /* LVGL计时器 */
        //     xSemaphoreGive(gui_MuxSem_Handle);
        // }
    }
}

static void disp_init(void)
{
    /*You code here*/
    LCD_Init();
}


static void disp_flush(lv_display_t * disp_drv, const lv_area_t * area, uint8_t *px_map)
{
        /*The most simple case (but also the slowest) to put all pixels to the screen one-by-one*/

    // int32_t x;
    // int32_t y;
    //     LCD_Address_Set(area->x1, area->y1, area->x2, area->y2);
    // // // LCD_Fill_1(area->x1,area->y1,area->x2,area->y2,px_map);
    // // LCD_Address_Set(area->x1,area->y1,area->x2,area->y2);
    // for(y = area->y1; y <= area->y2; y++) {
    //     for(x = area->x1; x <= area->x2; x++) {
    //         /*Put a pixel to the display. For example:*/
    //         /*put_px(x, y, *px_map)*/
    //         // LCD_Fill(area->x1,area->y1,area->x2,area->y2,(uint16_t) );
    //         LCD_WR_Byte(*px_map);
    //         px_map++;
    //     }
    // }

        // 1. 设置 LCD 的绘制区域
    // 假设你的 LCD_Address_Set 接口接受的坐标为 (x_start, y_start, x_end, y_end)
    LCD_Address_Set(area->x1, area->y1, area->x2, area->y2);

    // 2. 计算像素总数，并写入像素数据到 LCD
    uint16_t width = area->x2 - area->x1 + 1;
    uint16_t height = area->y2 - area->y1 + 1;
    uint32_t pixel_count = width * height;

    // 发送像素数据（假设 px_map 已经是 RGB565 格式）
    LCD_WR_DATA_1(px_map, pixel_count * 2);


    // 计算像素数量
    // uint16_t width = area->x2 - area->x1 + 1;
    // uint16_t height = area->y2 - area->y1 + 1;
    // uint32_t pixel_count = width * height;

    // 发送像素数据（假设 px_map 已经是 RGB565 格式）
    // LCD_WR_DATA(px_map, pixel_count * 2);

    /*IMPORTANT!!!
     *Inform the graphics library that you are ready with the flushing*/
    lv_display_flush_ready(disp_drv);
}

/**
 * @brief       告诉LVGL运行时间
 * @param       arg : 传入参数(未用到)
 * @retval      无
 */
static void increase_lvgl_tick(void *arg)
{
    /* 告诉LVGL已经过了多少毫秒 */
    lv_tick_inc(1);
}



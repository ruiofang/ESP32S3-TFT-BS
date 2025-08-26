#include "lcd_init.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

/* SPI句柄 */
spi_device_handle_t LCD_Handle;

/* 定义LCD最大传输字节数 */
// #define LCD_MAX_BUF_SIZE (LCD_W * LCD_H * 2)  // 全屏缓冲区太大，会导致内存不足

/* 定义一次连续写入的字节数 - 横屏模式调整 */
// 横屏：428 * 142 = 60776 像素，60776 * 2 = 121552 字节
// 使用分块大小：428 * 10 * 2 = 8560 字节（每次10行）
#define LCD_BUF_SIZE (LCD_W * 10 * 2)

/* 定义帧缓存 牺牲空间换时间 */
// uint8_t LCD_Buf[LCD_MAX_BUF_SIZE];  // 全屏缓冲区改为分块缓冲区
// uint8_t LCD_Buf[LCD_BUF_SIZE];  // 只分配分块大小的缓冲区
uint8_t *LCD_Buf = NULL;  // 动态分配缓冲区指针

/**
 * @brief       液晶端口初始化
 * @param       无
 * @retval      无
 */
void LCD_GPIOInit(void)
{
    gpio_config_t gpio_init_struct = {0};
    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;           /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT;           /* 输入输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;         /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;    /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << LCD_RES_GPIO_PIN; /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                           /* 配置GPIO */

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;          /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT;          /* 输入输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;        /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;   /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << LCD_DC_GPIO_PIN; /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                          /* 配置GPIO */

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;           /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT;           /* 输入输出模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;         /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;    /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ull << LCD_BLK_GPIO_PIN; /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                           /* 配置GPIO */
}

/**
 * @brief       向液晶写寄存器命令
 * @param       reg: 要写的命令
 * @retval      无
 */
void LCD_WR_REG(uint8_t reg)
{
    LCD_DC_Clr();
    BSP_SPI_WR_Bus(LCD_Handle, reg);
    LCD_DC_Set();
}

/**
 * @brief       向液晶写一个字节数据
 * @param       dat: 要写的数据
 * @retval      无
 */
void LCD_WR_Byte(uint8_t dat)
{
    LCD_DC_Set();
    BSP_SPI_WR_Bus(LCD_Handle, dat);
    LCD_DC_Set();
}


/**
 * @brief       向液晶写一个半字数据
 * @param       dat: 要写的数据
 * @retval      无
 */
void LCD_WR_HalfWord(uint16_t dat)
{
    LCD_DC_Set();
    BSP_SPI_WR_Bus(LCD_Handle, dat >> 8);
    BSP_SPI_WR_Bus(LCD_Handle, dat & 0xFF);
    LCD_DC_Set();
}

/**
 * @brief       向液晶连续写入len个字节数据
 * @param       dat: 要写的数据地址
 * @param       len：要写入字节长度
 * @retval      无
 */
void LCD_WR_DATA(const uint8_t *dat, int len)
{
    LCD_DC_Set();
    BSP_SPI_Write_Data(LCD_Handle, dat, len);
    LCD_DC_Set();
}

void LCD_WR_DATA_1(uint8_t *dat, int len)
{
    LCD_DC_Set();
    BSP_SPI_Write_Data(LCD_Handle, dat, len);
    LCD_DC_Set();
}

/**
 * @brief       液晶进入休眠
 * @param       无
 * @retval      无
 */
void LCD_Enter_Sleep(void)
{
    LCD_WR_REG(0x28);
    vTaskDelay(120 / portTICK_PERIOD_MS);
    LCD_WR_REG(0x10);
    vTaskDelay(50 / portTICK_PERIOD_MS);
}

/**
 * @brief       液晶退出休眠
 * @param       无
 * @retval      无
 */
void LCD_Exit_Sleep(void)
{
    LCD_WR_REG(0x11);
    vTaskDelay(120 / portTICK_PERIOD_MS);
    LCD_WR_REG(0x29);
}


/**
 * @brief       设置显示窗口
 * @param       xs:窗口列起始地址
 * @param       ys:坐标行起始地址
 * @param       xe:窗口列结束地址
 * @param       ye:坐标行结束地址
 * @retval      无
 */
void LCD_Address_Set(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye)
{
    // 添加边界检查，防止坐标超出屏幕范围
    if (xs >= LCD_W) xs = LCD_W - 1;
    if (xe >= LCD_W) xe = LCD_W - 1;
    if (ys >= LCD_H) ys = LCD_H - 1;
    if (ye >= LCD_H) ye = LCD_H - 1;
    
    #if USE_HORIZONTIAL==0
    LCD_WR_REG(0x2a); /* 列地址设置 */
    LCD_WR_HalfWord(xs+LCD_X_OFFSET);
    LCD_WR_HalfWord(xe+LCD_X_OFFSET);
    LCD_WR_REG(0x2b); /* 行地址设置 */
    LCD_WR_HalfWord(ys+LCD_Y_OFFSET);
    LCD_WR_HalfWord(ye+LCD_Y_OFFSET);
    LCD_WR_REG(0x2c); /* 储存器写 */
    #elif USE_HORIZONTIAL==1
    LCD_WR_REG(0x2a); /* 列地址设置 */
    LCD_WR_HalfWord(xs+LCD_X_OFFSET);
    LCD_WR_HalfWord(xe+LCD_X_OFFSET);
    LCD_WR_REG(0x2b); /* 行地址设置 */
    LCD_WR_HalfWord(ys+LCD_Y_OFFSET);
    LCD_WR_HalfWord(ye+LCD_Y_OFFSET);
    LCD_WR_REG(0x2c); /* 储存器写 */
    #elif USE_HORIZONTIAL==2
    LCD_WR_REG(0x2a); /* 列地址设置 */
    LCD_WR_HalfWord(xs+LCD_X_OFFSET);
    LCD_WR_HalfWord(xe+LCD_X_OFFSET);
    LCD_WR_REG(0x2b); /* 行地址设置 */
    LCD_WR_HalfWord(ys+LCD_Y_OFFSET);
    LCD_WR_HalfWord(ye+LCD_Y_OFFSET);
    LCD_WR_REG(0x2c); /* 储存器写 */
    #else
    LCD_WR_REG(0x2a); /* 列地址设置 */
    LCD_WR_HalfWord(xs+LCD_X_OFFSET);
    LCD_WR_HalfWord(xe+LCD_X_OFFSET);
    LCD_WR_REG(0x2b); /* 行地址设置 */
    LCD_WR_HalfWord(ys+LCD_Y_OFFSET);
    LCD_WR_HalfWord(ye+LCD_Y_OFFSET);
    LCD_WR_REG(0x2c); /* 储存器写 */
    #endif
}

/**
 * @brief       指定颜色填充区域
 * @param       xs:填充区域列起始地址
 * @param       ys:填充区域行起始地址
 * @param       xe:填充区域列结束地址
 * @param       ye:填充区域行结束地址
 * @param       color:填充颜色值
 * @retval      无
 */
void LCD_Fill(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint16_t color)
{
    uint16_t i;
    uint32_t pixel_count;
    
    // 确保坐标在有效范围内
    if (xs >= LCD_W) xs = LCD_W - 1;
    if (xe >= LCD_W) xe = LCD_W - 1;  // xe应该是最后一个像素的坐标，不能等于LCD_W
    if (ys >= LCD_H) ys = LCD_H - 1;
    if (ye >= LCD_H) ye = LCD_H - 1;  // ye应该是最后一个像素的坐标，不能等于LCD_H
    
    // 设置显示窗口（xe, ye已经是最后一个像素的坐标）
    LCD_Address_Set(xs, ys, xe, ye);
    
    // 计算实际需要填充的像素数量
    pixel_count = (xe - xs + 1) * (ye - ys + 1);
    
    // 使用精确的像素计数来避免越界
    for (i = 0; i < pixel_count; i++)
    {
        LCD_WR_HalfWord(color);
    }
}

/**
 * @brief       快刷清屏区域
 * @param       color:填充颜色值
 * @retval      无
 */
void LCD_FastFill(uint16_t color)
{
    uint32_t i;
    uint32_t total_pixels = LCD_W * LCD_H;
    uint8_t data[2] = {0};
    
    data[0] = color >> 8;
    data[1] = color;
    
    // 设置整个屏幕显示窗口
    LCD_Address_Set(0, 0, LCD_W - 1, LCD_H - 1);
    
    // 填充缓冲区为指定颜色
    for (i = 0; i < LCD_BUF_SIZE / 2; i++)
    {
        LCD_Buf[i * 2] = data[0];
        LCD_Buf[i * 2 + 1] = data[1];
    }
    
    // 分块发送数据，确保覆盖整个屏幕
    uint32_t remaining_bytes = total_pixels * 2;
    uint32_t chunk_bytes = LCD_BUF_SIZE;
    
    while (remaining_bytes > 0) {
        if (remaining_bytes < chunk_bytes) {
            chunk_bytes = remaining_bytes;
        }
        LCD_WR_DATA(LCD_Buf, chunk_bytes);
        remaining_bytes -= chunk_bytes;
    }
}

/**
 * @brief       初始化LCD
 * @param       无
 * @retval      无
 */
void LCD_Init(void)
{
    esp_err_t ret = 0;
    
    // 动态分配LCD缓冲区，优先使用PSRAM
    if (LCD_Buf == NULL) {
        LCD_Buf = (uint8_t*)heap_caps_malloc(LCD_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (LCD_Buf == NULL) {
            // 如果PSRAM分配失败，使用内部RAM
            LCD_Buf = (uint8_t*)heap_caps_malloc(LCD_BUF_SIZE, MALLOC_CAP_8BIT);
            if (LCD_Buf == NULL) {
                ESP_LOGE("LCD", "Failed to allocate LCD buffer memory");
                return;
            }
            ESP_LOGW("LCD", "LCD buffer allocated in internal RAM");
        } else {
            ESP_LOGI("LCD", "LCD buffer allocated in PSRAM");
        }
    }
    
    /* 初始化SPI总线 */
    BSP_SPI_GPIOInit();
    /* SPI驱动接口配置 */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 40 * 1000 * 1000,  /* SPI时钟降低到40MHz，提高稳定性 */
        .mode = 3,                           /* SPI模式0 */
        .spics_io_num = BSP_SPI_CS_GPIO_PIN, /* SPI设备引脚 */
        .queue_size = 7,                     /* 事务队列尺寸 7个 */
    };
    /* 添加SPI总线设备 */
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &LCD_Handle); /* 配置SPI总线设备 */
    ESP_ERROR_CHECK(ret);
    
    LCD_GPIOInit();
    
    // 确保背光初始关闭，避免初始化时闪烁
    LCD_BLK_Clr();
    
    // 硬件复位序列
    LCD_RES_Set();
    vTaskDelay(10 / portTICK_PERIOD_MS);
    LCD_RES_Clr();
    vTaskDelay(10 / portTICK_PERIOD_MS);
    LCD_RES_Set();
    vTaskDelay(120 / portTICK_PERIOD_MS);  // 确保复位后稳定
    
    // 严格按照 NV3006A1N IVO2.66 参考文件初始化（注意型号差异）
    // 添加必要的初始延时
    vTaskDelay(10 / portTICK_PERIOD_MS);
    
    LCD_WR_REG(0xff);
    LCD_WR_Byte(0xa5);
    vTaskDelay(1 / portTICK_PERIOD_MS);  // 添加小延时确保寄存器切换
    
    LCD_WR_REG(0x9a);
    LCD_WR_Byte(0x08);
    LCD_WR_REG(0x9b);
    LCD_WR_Byte(0x08);
    LCD_WR_REG(0x9c);
    LCD_WR_Byte(0xb0);
    LCD_WR_REG(0x9d);
    LCD_WR_Byte(0x16);
    LCD_WR_REG(0x9e);
    LCD_WR_Byte(0xc4);
    
    // 关键电源设置 - 这些参数直接影响显示质量
    LCD_WR_REG(0x8f);
    LCD_WR_Byte(0x55);
    LCD_WR_Byte(0x04);
    LCD_WR_REG(0x84);
    LCD_WR_Byte(0x90);
    LCD_WR_REG(0x83);
    LCD_WR_Byte(0x7b);
    LCD_WR_REG(0x85);
    LCD_WR_Byte(0x33);
    
    // GAMMA 参数设置 - 严格按照参考文件
    LCD_WR_REG(0x60);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0x70);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0x61);
    LCD_WR_Byte(0x02);
    LCD_WR_REG(0x71);
    LCD_WR_Byte(0x02);
    LCD_WR_REG(0x62);
    LCD_WR_Byte(0x04);
    LCD_WR_REG(0x72);
    LCD_WR_Byte(0x04);
    LCD_WR_REG(0x6c);
    LCD_WR_Byte(0x29);
    LCD_WR_REG(0x7c);
    LCD_WR_Byte(0x29);
    LCD_WR_REG(0x6d);
    LCD_WR_Byte(0x31);
    LCD_WR_REG(0x7d);
    LCD_WR_Byte(0x31);
    LCD_WR_REG(0x6e);
    LCD_WR_Byte(0x0f);
    LCD_WR_REG(0x7e);
    LCD_WR_Byte(0x0f);
    LCD_WR_REG(0x66);
    LCD_WR_Byte(0x21);
    LCD_WR_REG(0x76);
    LCD_WR_Byte(0x21);
    LCD_WR_REG(0x68);
    LCD_WR_Byte(0x3A);
    LCD_WR_REG(0x78);
    LCD_WR_Byte(0x3A);
    LCD_WR_REG(0x63);
    LCD_WR_Byte(0x07);
    LCD_WR_REG(0x73);
    LCD_WR_Byte(0x07);
    LCD_WR_REG(0x64);
    LCD_WR_Byte(0x05);
    LCD_WR_REG(0x74);
    LCD_WR_Byte(0x05);
    LCD_WR_REG(0x65);
    LCD_WR_Byte(0x02);
    LCD_WR_REG(0x75);
    LCD_WR_Byte(0x02);
    LCD_WR_REG(0x67);
    LCD_WR_Byte(0x23);
    LCD_WR_REG(0x77);
    LCD_WR_Byte(0x23);
    LCD_WR_REG(0x69);
    LCD_WR_Byte(0x08);
    LCD_WR_REG(0x79);
    LCD_WR_Byte(0x08);
    LCD_WR_REG(0x6a);
    LCD_WR_Byte(0x13);
    LCD_WR_REG(0x7a);
    LCD_WR_Byte(0x13);
    LCD_WR_REG(0x6b);
    LCD_WR_Byte(0x13);
    LCD_WR_REG(0x7b);
    LCD_WR_Byte(0x13);
    LCD_WR_REG(0x6f);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0x7f);
    LCD_WR_Byte(0x00);
    
    // 源极驱动设置
    LCD_WR_REG(0x50);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0x52);
    LCD_WR_Byte(0xd6);
    LCD_WR_REG(0x53);
    LCD_WR_Byte(0x08);
    LCD_WR_REG(0x54);
    LCD_WR_Byte(0x08);
    LCD_WR_REG(0x55);
    LCD_WR_Byte(0x1e);
    LCD_WR_REG(0x56);
    LCD_WR_Byte(0x1c);
    
    // GOA map_sel 设置
    LCD_WR_REG(0xa0);
    LCD_WR_Byte(0x2b);
    LCD_WR_Byte(0x24);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0xa1);
    LCD_WR_Byte(0x87);
    LCD_WR_REG(0xa2);
    LCD_WR_Byte(0x86);
    LCD_WR_REG(0xa5);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0xa6);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0xa7);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0xa8);
    LCD_WR_Byte(0x36);
    LCD_WR_REG(0xa9);
    LCD_WR_Byte(0x7e);
    LCD_WR_REG(0xaa);
    LCD_WR_Byte(0x7e);
    
    // B9-D1 寄存器设置
    LCD_WR_REG(0xB9);
    LCD_WR_Byte(0x85);
    LCD_WR_REG(0xBA);
    LCD_WR_Byte(0x84);
    LCD_WR_REG(0xBB);
    LCD_WR_Byte(0x83);
    LCD_WR_REG(0xBC);
    LCD_WR_Byte(0x82);
    LCD_WR_REG(0xBD);
    LCD_WR_Byte(0x81);
    LCD_WR_REG(0xBE);
    LCD_WR_Byte(0x80);
    LCD_WR_REG(0xBF);
    LCD_WR_Byte(0x01);
    LCD_WR_REG(0xC0);
    LCD_WR_Byte(0x02);
    LCD_WR_REG(0xc1);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0xc2);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0xc3);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0xc4);
    LCD_WR_Byte(0x33);
    LCD_WR_REG(0xc5);
    LCD_WR_Byte(0x7e);
    LCD_WR_REG(0xc6);
    LCD_WR_Byte(0x7e);
    
    // C8-D1 栅极信号设置
    LCD_WR_REG(0xC8);
    LCD_WR_Byte(0x33);
    LCD_WR_Byte(0x33);
    LCD_WR_REG(0xC9);
    LCD_WR_Byte(0x68);
    LCD_WR_REG(0xCA);
    LCD_WR_Byte(0x69);
    LCD_WR_REG(0xCB);
    LCD_WR_Byte(0x6a);
    LCD_WR_REG(0xCC);
    LCD_WR_Byte(0x6b);
    LCD_WR_REG(0xCD);
    LCD_WR_Byte(0x33);
    LCD_WR_Byte(0x33);
    LCD_WR_REG(0xCE);
    LCD_WR_Byte(0x6c);
    LCD_WR_REG(0xCF);
    LCD_WR_Byte(0x6d);
    LCD_WR_REG(0xD0);
    LCD_WR_Byte(0x6e);
    LCD_WR_REG(0xD1);
    LCD_WR_Byte(0x6f);
    
    // AB-AE 多路输出设置
    LCD_WR_REG(0xAB);
    LCD_WR_Byte(0x03);
    LCD_WR_Byte(0x67);
    LCD_WR_REG(0xAC);
    LCD_WR_Byte(0x03);
    LCD_WR_Byte(0x6b);
    LCD_WR_REG(0xAD);
    LCD_WR_Byte(0x03);
    LCD_WR_Byte(0x68);
    LCD_WR_REG(0xAE);
    LCD_WR_Byte(0x03);
    LCD_WR_Byte(0x6c);
    
    // B3-B8 设置
    LCD_WR_REG(0xb3);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0xb4);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0xb5);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0xB6);
    LCD_WR_Byte(0x32);
    LCD_WR_REG(0xB7);
    LCD_WR_Byte(0x7e);
    LCD_WR_REG(0xB8);
    LCD_WR_Byte(0x7e);
    
    // E0-F1 电源管理设置
    LCD_WR_REG(0xe0);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0xe1);
    LCD_WR_Byte(0x03);
    LCD_WR_Byte(0x0f);
    LCD_WR_REG(0xe2);
    LCD_WR_Byte(0x04);
    LCD_WR_REG(0xe3);
    LCD_WR_Byte(0x01);
    LCD_WR_REG(0xe4);
    LCD_WR_Byte(0x0e);
    LCD_WR_REG(0xe5);
    LCD_WR_Byte(0x01);
    LCD_WR_REG(0xe6);
    LCD_WR_Byte(0x19);
    LCD_WR_REG(0xe7);
    LCD_WR_Byte(0x10);
    LCD_WR_REG(0xe8);
    LCD_WR_Byte(0x10);
    LCD_WR_REG(0xea);
    LCD_WR_Byte(0x12);
    LCD_WR_REG(0xeb);
    LCD_WR_Byte(0xd0);
    LCD_WR_REG(0xec);
    LCD_WR_Byte(0x04);
    LCD_WR_REG(0xed);
    LCD_WR_Byte(0x07);
    LCD_WR_REG(0xee);
    LCD_WR_Byte(0x07);
    LCD_WR_REG(0xef);
    LCD_WR_Byte(0x09);
    LCD_WR_REG(0xf0);
    LCD_WR_Byte(0xd0);
    LCD_WR_REG(0xf1);
    LCD_WR_Byte(0x0e);
    
    // 关键修正：根据参考文件，这里可能有特殊的序列
    // 参考文件显示在F1寄存器后面有特殊的处理方式
    // 注释显示先设置F9寄存器，然后有一个单独的DAT(0x17)
    // 这可能意味着需要特殊的时序处理
    
    // 尝试第一种方式：不设置F9，直接发送数据（这在某些驱动中是可能的）
    // LCD_DC_Set(); // 确保数据模式
    // BSP_SPI_WR_Bus(LCD_Handle, 0x17);
    
    // 尝试第二种方式：标准F9寄存器设置但按参考文件的精确时序
    LCD_WR_REG(0xF9);
    LCD_WR_Byte(0x17);
    
    // F2寄存器设置 - 这个很关键，影响刷屏质量
    LCD_WR_REG(0xf2);
    LCD_WR_Byte(0x2c);  // 第一个参数从0x2e改为0x2c（参考文件中的修正）
    LCD_WR_Byte(0x1b);
    LCD_WR_Byte(0x0b);
    LCD_WR_Byte(0x20);
    
    // 1 dot 设置
    LCD_WR_REG(0xe9);
    LCD_WR_Byte(0x29);
    LCD_WR_REG(0xec);
    LCD_WR_Byte(0x04);
    
    // TE (Tearing Effect) 设置
    LCD_WR_REG(0x35);
    LCD_WR_Byte(0x00);
    LCD_WR_REG(0x44);
    LCD_WR_Byte(0x00);
    LCD_WR_Byte(0x10);
    LCD_WR_REG(0x46);
    LCD_WR_Byte(0x10);
    
    // 恢复寄存器页面
    LCD_WR_REG(0xff);
    LCD_WR_Byte(0x00);
    
    // 像素格式设置 (16位RGB565)
    LCD_WR_REG(0x3a);
    // 像素格式设置 (16位RGB565)
    LCD_WR_REG(0x3a);
    LCD_WR_Byte(0x05);
    
    // 退出睡眠模式 - 严格按照参考文件时序
    LCD_WR_REG(0x11);
    vTaskDelay(220 / portTICK_PERIOD_MS);  // Delay(220) - 参考文件的精确延时
    
    // 开启显示
    LCD_WR_REG(0x29);
    vTaskDelay(200 / portTICK_PERIOD_MS);  // Delay(200) - 参考文件的精确延时
    
    // 重要：参考文件中没有设置0x36寄存器，我们也不设置
    // 显示方向将由硬件默认设置决定
    
    // 设置显示方向
    LCD_WR_REG(0x36);
    if (USE_HORIZONTIAL == 0)
    {
        LCD_WR_Byte(0x00);  // 0度：正常竖屏
    }
    else if (USE_HORIZONTIAL == 1)
    {
        LCD_WR_Byte(0xC0);  // 180度：倒置竖屏
    }
    else if (USE_HORIZONTIAL == 2)
    {
        LCD_WR_Byte(0x60);  // 90度：横屏
    }
    else
    {
        LCD_WR_Byte(0xA0);  // 270度：横屏倒置
    }
    
    // 完成所有初始化后再开启背光，避免闪烁
    vTaskDelay(50 / portTICK_PERIOD_MS);  // 等待显示稳定
    LCD_BLK_Set();  // 开启背光
}


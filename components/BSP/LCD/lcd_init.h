#ifndef _LCD_INIT_H_
#define _LCD_INIT_H_

#include "spi.h"

/* 设置LCD显示方向 */
#define USE_HORIZONTIAL 3 /* 0: 竖屏0； 1: 竖屏180；  2: 横屏90；  3: 横屏270； */

/* 显示偏移量调整 - 横屏模式调整 */
#if USE_HORIZONTIAL == 2 
    #define LCD_X_OFFSET 0x00  /* 横屏模式下列偏移量 */
    #define LCD_Y_OFFSET 0x0E  /* 横屏模式下行偏移量，减少偏移（0x0E=14像素）*/
#elif USE_HORIZONTIAL == 3
    #define LCD_X_OFFSET 0x00  /* 横屏模式下列偏移量 */
    #define LCD_Y_OFFSET 0x0C  /* 横屏模式下行偏移量，减少偏移（0x0C=12像素）*/
#else
    #define LCD_X_OFFSET 0x0C  /* 竖屏模式下列偏移量 */
    #define LCD_Y_OFFSET 0x00  /* 竖屏模式下行偏移量 */
#endif

/* LCD缓冲区大小定义 */
#define LCD_BUF_SIZE (LCD_W * 10 * 2)  /* 10行的缓冲区：142*10*2 = 2840 bytes */

/* 液晶屏分辨率 */
#if USE_HORIZONTIAL == 0 || USE_HORIZONTIAL == 1
#define LCD_W 142
#define LCD_H 428
#else
#define LCD_W 428
#define LCD_H 142
#endif


/* GPIO引脚定义 */
#define LCD_RES_GPIO_PIN GPIO_NUM_12
#define LCD_DC_GPIO_PIN GPIO_NUM_13
#define LCD_BLK_GPIO_PIN GPIO_NUM_10

/* GPIO操作宏定义 */
#define LCD_RES_Set() gpio_set_level(LCD_RES_GPIO_PIN, 1)
#define LCD_RES_Clr() gpio_set_level(LCD_RES_GPIO_PIN, 0)

#define LCD_DC_Set() gpio_set_level(LCD_DC_GPIO_PIN, 1)
#define LCD_DC_Clr() gpio_set_level(LCD_DC_GPIO_PIN, 0)

#define LCD_BLK_Set() gpio_set_level(LCD_BLK_GPIO_PIN, 1)
#define LCD_BLK_Clr() gpio_set_level(LCD_BLK_GPIO_PIN, 0)


/* 函数声明 */
void LCD_GPIOInit(void);                                                           // 初始化LCD相关GPIO
void LCD_WR_REG(uint8_t reg);                                                      // LCD写寄存器命令
void LCD_WR_Byte(uint8_t dat);                                                     // LCD写8位数据
void LCD_WR_HalfWord(uint16_t dat);                                                // LCD写16位数据
void LCD_SetCursor(uint16_t x, uint16_t y);                                        // 设置光标位置
void LCD_Address_Set(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye);          // 设置显示区域
void LCD_Fill(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint16_t color); // 区域填充
void LCD_FastFill(uint16_t color);                                                 // 全屏快速填充
void LCD_Enter_Sleep(void);                                                        // 进入睡眠模式
void LCD_Exit_Sleep(void);                                                         // 退出睡眠模式
void LCD_Init(void);                                                               // 初始化LCD显示器
void LCD_WR_DATA_1(uint8_t *dat, int len);

/* 颜色宏定义 */
#define WHITE 0xFFFF
#define BLACK 0x0000
#define BLUE 0x001F
#define BRED 0XF81F
#define GRED 0XFFE0
#define GBLUE 0X07FF
#define RED 0xF800
#define MAGENTA 0xF81F
#define GREEN 0x07E0
#define CYAN 0x7FFF
#define YELLOW 0xFFE0
#define BROWN 0XBC40      // 棕色
#define BRRED 0XFC07      // 棕红色
#define GRAY 0X8430       // 灰色
#define DARKBLUE 0X01CF   // 深蓝色
#define LIGHTBLUE 0X7D7C  // 浅蓝色
#define GRAYBLUE 0X5458   // 灰蓝色
#define LIGHTGREEN 0X841F // 浅绿色
#define LGRAY 0XC618      // 浅灰色(PANNEL),窗体背景色
#define LGRAYBLUE 0XA651  // 浅灰蓝色(中间层颜色)
#define LBBLUE 0X2B12     // 浅棕蓝色(选择条目的反色)

#endif

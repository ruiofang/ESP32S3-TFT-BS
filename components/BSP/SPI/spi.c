#include "spi.h"
#include "lcd_init.h"

/**
 * @brief       初始化SPI
 * @param       无
 * @retval      无
 */
void BSP_SPI_GPIOInit(void)
{
    esp_err_t ret = 0;
    spi_bus_config_t spi_bus_conf = {0};
    /* SPI总线配置 */
    spi_bus_conf.miso_io_num = -1;
    spi_bus_conf.mosi_io_num = BSP_SPI_MOSI_GPIO_PIN; /* SPI_MOSI引脚 */
    spi_bus_conf.sclk_io_num = BSP_SPI_CLK_GPIO_PIN;  /* SPI_SCLK引脚 */
    spi_bus_conf.quadwp_io_num = -1;                  /* SPI写保护信号引脚，该引脚未使能 */
    spi_bus_conf.quadhd_io_num = -1;                  /* SPI保持信号引脚，该引脚未使能 */
    spi_bus_conf.max_transfer_sz = LCD_W * LCD_H * 2; /* 配置最大传输大小，以字节为单位 */
    /* 初始化SPI总线 */
    ret = spi_bus_initialize(SPI2_HOST, &spi_bus_conf, SPI_DMA_CH_AUTO); /* SPI总线初始化 */
    ESP_ERROR_CHECK(ret);                                                /* 校验参数值 */
}

/**
 * @brief       SPI发送一个字节
 * @param       handle : SPI句柄
 * @param       dat    : 字节内容
 * @retval      无
 */
void BSP_SPI_WR_Bus(spi_device_handle_t handle, uint8_t dat)
{
    esp_err_t ret;
    spi_transaction_t t = {0};
    t.length = 8;                                  /* 要传输的位数 一个字节 8位 */
    t.tx_buffer = &dat;                            /* 将命令填充进去 */
    ret = spi_device_polling_transmit(handle, &t); /* 开始传输 */
    ESP_ERROR_CHECK(ret);
}

/**
 * @brief       SPI连续发送len个字节
 * @param       handle : SPI句柄
 * @param       dat    : 要发送的字节地址
 * @param       len    : 要发送的字节长度
 * @retval      无
 */
void BSP_SPI_Write_Data(spi_device_handle_t handle, const uint8_t *dat, int len)
{
    esp_err_t ret;
    spi_transaction_t t = {0};
    if (len == 0)
    {
        return; /* 长度为0 没有数据要传输 */
    }
    t.length = len * 8;                            /* 要传输的位数 一个字节 8位 */
    t.tx_buffer = dat;                             /* 将命令填充进去 */
    ret = spi_device_polling_transmit(handle, &t); /* 开始传输 */
    ESP_ERROR_CHECK(ret);                          /* 一般不会有问题 */
}

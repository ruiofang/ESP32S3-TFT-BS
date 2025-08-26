#ifndef _SPI_H_
#define _SPI_H_

#include <string.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#define BSP_SPI_MOSI_GPIO_PIN GPIO_NUM_13
#define BSP_SPI_CLK_GPIO_PIN GPIO_NUM_14
#define BSP_SPI_CS_GPIO_PIN GPIO_NUM_10

#define BSP_CS_Set() gpio_set_level(BSP_SPI_CS_GPIO_PIN,1)
#define BSP_CS_Clr() gpio_set_level(BSP_SPI_CS_GPIO_PIN,0)

void BSP_SPI_GPIOInit(void);
void BSP_SPI_WR_Bus(spi_device_handle_t handle, uint8_t dat);
void BSP_SPI_Write_Data(spi_device_handle_t handle, const uint8_t *dat, int len);

#endif




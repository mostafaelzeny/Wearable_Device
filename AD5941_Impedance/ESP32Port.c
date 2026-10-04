#include "ad5940.h"

#include <assert.h>
#include <string.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"

/* ESP32 Dev Module / ESP32-WROOM-32 wiring for the custom AD5941 board. */
#define AD5940_SPI_HOST       VSPI_HOST
#define AD5940_SPI_FREQ_HZ    (8000000)
#define AD5940_SCLK_PIN       GPIO_NUM_18
#define AD5940_MISO_PIN       GPIO_NUM_19
#define AD5940_MOSI_PIN       GPIO_NUM_23
#define AD5940_CS_PIN         GPIO_NUM_5
#define AD5940_RST_PIN        GPIO_NUM_4
#define AD5940_GP1INT_PIN     GPIO_NUM_2

static spi_device_handle_t spi_handle;
volatile static uint8_t ucInterrupted = 0;

static void AD5940_SPITransactionDelay(void)
{
  ets_delay_us(10);
}

/**
 * @brief Pull !CS pin high
*/
void AD5940_CsSet(void)
{
  gpio_set_level(AD5940_CS_PIN, 1);
  AD5940_SPITransactionDelay();
}

/**
 * @brief Pull !CS pin low
*/
void AD5940_CsClr(void)
{
  AD5940_SPITransactionDelay();
  gpio_set_level(AD5940_CS_PIN, 0);
}

/**
 * @brief Pull !RESET pin high
*/
void AD5940_RstSet(void)
{
  gpio_set_level(AD5940_RST_PIN, 1);
}

/**
 * @brief Pull !RESET pin low
*/
void AD5940_RstClr(void)
{
  gpio_set_level(AD5940_RST_PIN, 0);
}

uint32_t AD5940_GetMCUIntFlag(void)
{
	return ucInterrupted;
}

uint32_t AD5940_ClrMCUIntFlag(void)
{
	ucInterrupted = 0;
	return 1;
}

static void IRAM_ATTR ad5940_gpio1_isr_handler(void* arg)
{
  (void)arg;
  ucInterrupted = 1;
}

/**
 * @brief Block the processor for a certain amount of time. Total delay is 10*time microseconds
 * @param time: number of 10us delays.
 * @return None
*/
void AD5940_Delay10us(uint32_t time)
{
  while(time--)
    ets_delay_us(10);
}

void AD5940_DelayMs(uint32_t time)
{
  while(time--)
    ets_delay_us(1000);
}

/**
  @brief Using SPI to transmit one byte and return the received byte. 
  @param pSendBuffer: Pointer to the data to be sent
    - Set to NULL to skip write phase
  @param pRecvBuff: Pointer to the buffer used to store received data.
    - Set to NULL to skip read phase
  @param length: data length in SendBuffer in bytes
  @note this function does not use command bits for compatibility with the AD5940 library
  @return None
**/
void AD5940_ReadWriteNBytes(unsigned char *pSendBuffer,unsigned char *pRecvBuff,unsigned long length)
{
  if(length == 0)
    return;

  spi_transaction_t t;
  memset(&t, 0, sizeof(t));

  t.tx_buffer = pSendBuffer;
  t.rx_buffer = pRecvBuff;
  t.length = length * 8;

  spi_device_transmit(spi_handle, &t);
}

static unsigned char AD5940_PortReadWrite8B(unsigned char data)
{
  uint8_t tx[1] = {data};
  uint8_t rx[1] = {0};
  AD5940_ReadWriteNBytes(tx, rx, 1);
  return rx[0];
}

static uint16_t AD5940_PortReadWrite16B(uint16_t data)
{
  uint8_t tx[2] = {(uint8_t)(data >> 8), (uint8_t)data};
  uint8_t rx[2] = {0};
  AD5940_ReadWriteNBytes(tx, rx, 2);
  return ((uint16_t)rx[0] << 8) | rx[1];
}

static uint32_t AD5940_PortReadWrite32B(uint32_t data)
{
  uint8_t tx[4] = {
    (uint8_t)(data >> 24),
    (uint8_t)(data >> 16),
    (uint8_t)(data >> 8),
    (uint8_t)data
  };
  uint8_t rx[4] = {0};
  AD5940_ReadWriteNBytes(tx, rx, 4);
  return ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
         ((uint32_t)rx[2] << 8) | rx[3];
}

void AD5940_SPIWriteReg(uint16_t RegAddr, uint32_t RegData)
{
  AD5940_CsClr();
  AD5940_PortReadWrite8B(SPICMD_SETADDR);
  AD5940_PortReadWrite16B(RegAddr);
  AD5940_CsSet();

  AD5940_CsClr();
  AD5940_PortReadWrite8B(SPICMD_WRITEREG);
  if((RegAddr >= 0x1000) && (RegAddr <= 0x3014))
    AD5940_PortReadWrite32B(RegData);
  else
    AD5940_PortReadWrite16B((uint16_t)RegData);
  AD5940_CsSet();
}

uint32_t AD5940_SPIReadReg(uint16_t RegAddr)
{
  uint32_t data;

  AD5940_CsClr();
  AD5940_PortReadWrite8B(SPICMD_SETADDR);
  AD5940_PortReadWrite16B(RegAddr);
  AD5940_CsSet();

  AD5940_CsClr();
  AD5940_PortReadWrite8B(SPICMD_READREG);
  AD5940_PortReadWrite8B(0);
  if((RegAddr >= 0x1000) && (RegAddr <= 0x3014))
    data = AD5940_PortReadWrite32B(0);
  else
    data = AD5940_PortReadWrite16B(0);
  AD5940_CsSet();

  return data;
}

/**
  @brief Initialise SPI and GPIO peripherals for ESP32. 
  @param pCfg: Optional configuration flags.
  @return always 0.
**/
uint32_t AD5940_MCUResourceInit(void *pCfg)
{
  (void)pCfg;

	spi_bus_config_t buscfg={
		.mosi_io_num = AD5940_MOSI_PIN,
		.miso_io_num = AD5940_MISO_PIN,
		.sclk_io_num = AD5940_SCLK_PIN,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
    .max_transfer_sz = 4096
	};

  spi_device_interface_config_t devcfg={
    .command_bits = 0,
    .address_bits = 0,
    .dummy_bits = 0,
    .mode = 0,
    .duty_cycle_pos = 128,
    .cs_ena_posttrans = 0,
    .cs_ena_pretrans = 0,
    .clock_speed_hz = AD5940_SPI_FREQ_HZ,
    .spics_io_num = -1,
    .queue_size = 1
  };

  gpio_config_t ad5940_outputs = {
    .pin_bit_mask = (1ULL << AD5940_CS_PIN) | (1ULL << AD5940_RST_PIN),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
  };

  gpio_config_t ad5940_int = {
    .pin_bit_mask = (1ULL << AD5940_GP1INT_PIN),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_NEGEDGE
  };

	gpio_config(&ad5940_outputs);
  AD5940_CsSet();
  AD5940_RstSet();

  gpio_config(&ad5940_int);
  esp_err_t isr_ret = gpio_install_isr_service(0);
  if((isr_ret != ESP_OK) && (isr_ret != ESP_ERR_INVALID_STATE))
    return (uint32_t)isr_ret;
  gpio_isr_handler_add(AD5940_GP1INT_PIN, ad5940_gpio1_isr_handler, NULL);

	esp_err_t ret;

	ret = spi_bus_initialize(AD5940_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    assert(ret == ESP_OK);

	ret = spi_bus_add_device(AD5940_SPI_HOST, &devcfg, &spi_handle);
	assert(ret == ESP_OK);

  return 0;
}

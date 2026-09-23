/**
  ******************************************************************************
  * @file    device_config.c
  * @brief   See device_config.h. Storage: the last Flash sector (11,
  *          0x080E0000, 128 KB on the STM32F407VG's 1 MB Flash) - far above
  *          the firmware image (currently ~177 KB / 17 %), so it can grow a
  *          lot before ever approaching this sector. Sector 5 (0x08020000)
  *          would have been the *next* free one after the firmware, but
  *          that's exactly the kind of margin that erodes as the firmware
  *          grows - sector 11 is deliberately as far from the code as
  *          possible instead.
  ******************************************************************************
  */
#include "device_config.h"

#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "stm32f4xx_hal.h"

#define DEVICE_CONFIG_MAGIC          0x43464731U  /* ASCII "CFG1" */
#define DEVICE_CONFIG_VERSION        1U
#define DEVICE_CONFIG_FLASH_SECTOR   FLASH_SECTOR_11
#define DEVICE_CONFIG_FLASH_ADDR     0x080E0000U

/* Compiled-in defaults, used the first time this firmware runs on a chip
   (blank Flash) or after a version bump with no migration path. Same
   server address tcp_echo_client.h used to hardcode. */
#define DEFAULT_SERVER_IP0  10U
#define DEFAULT_SERVER_IP1  0U
#define DEFAULT_SERVER_IP2  1U
#define DEFAULT_SERVER_IP3  16U
#define DEFAULT_SERVER_PORT 5000U
#define DEFAULT_NAME        "stm32f407"

static DeviceConfig s_config;

/* Standalone reflected CRC-32 (poly 0xEDB88320, init/final XOR
   0xFFFFFFFF) - identical algorithm to Display/Src/tft_app.c's
   crc32_compute(), duplicated rather than shared so Config/ and Display/
   stay independent of each other (neither includes the other's headers) -
   ~15 lines, not worth a shared utility module for. */
static uint32_t crc32_compute(const uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFU;

  for (uint32_t i = 0; i < len; i++)
  {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8U; bit++)
    {
      crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

static uint32_t config_crc(const DeviceConfig *cfg)
{
  return crc32_compute((const uint8_t *)cfg, (uint32_t)offsetof(DeviceConfig, crc32));
}

static void set_defaults(DeviceConfig *cfg)
{
  memset(cfg, 0, sizeof(*cfg));
  cfg->magic   = DEVICE_CONFIG_MAGIC;
  cfg->version = DEVICE_CONFIG_VERSION;
  cfg->server_ip[0] = DEFAULT_SERVER_IP0;
  cfg->server_ip[1] = DEFAULT_SERVER_IP1;
  cfg->server_ip[2] = DEFAULT_SERVER_IP2;
  cfg->server_ip[3] = DEFAULT_SERVER_IP3;
  cfg->server_port  = DEFAULT_SERVER_PORT;
  snprintf(cfg->name, sizeof(cfg->name), DEFAULT_NAME);
  cfg->use_static_ip = 0U;
  /* static_ip/static_netmask/static_gw left zeroed - unused while
     use_static_ip == 0, and 0.0.0.0 is an obvious "not set" if printed
     before ever being configured. */
}

void device_config_load(void)
{
  const DeviceConfig *flash_cfg = (const DeviceConfig *)(const void *)DEVICE_CONFIG_FLASH_ADDR;

  if ((flash_cfg->magic == DEVICE_CONFIG_MAGIC) &&
      (flash_cfg->version == DEVICE_CONFIG_VERSION) &&
      (config_crc(flash_cfg) == flash_cfg->crc32))
  {
    memcpy(&s_config, flash_cfg, sizeof(s_config));
  }
  else
  {
    set_defaults(&s_config);
  }
}

DeviceConfig *device_config_get(void)
{
  return &s_config;
}

uint8_t device_config_save(void)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t sector_error = 0;
  HAL_StatusTypeDef status;
  const uint32_t *src;
  uint32_t addr;
  uint32_t words;

  s_config.crc32 = config_crc(&s_config);

  HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                          FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

  erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
  erase.Sector       = DEVICE_CONFIG_FLASH_SECTOR;
  erase.NbSectors    = 1U;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;  /* 2.7-3.6 V - this board runs at 3.3 V,
                                                   confirmed by OpenOCD's target-voltage
                                                   reading during flashing */

  status = HAL_FLASHEx_Erase(&erase, &sector_error);

  if (status == HAL_OK)
  {
    /* Word (32-bit) programming: fastest option Voltage range 3 allows,
       and sizeof(DeviceConfig) is a multiple of 4 (its last member is a
       uint32_t, and C struct padding rounds the whole size up to its
       strictest member's alignment) - no partial-word tail to handle. */
    src   = (const uint32_t *)(const void *)&s_config;
    addr  = DEVICE_CONFIG_FLASH_ADDR;
    words = sizeof(s_config) / 4U;

    for (uint32_t i = 0; (i < words) && (status == HAL_OK); i++)
    {
      status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, src[i]);
      addr += 4U;
    }
  }

  HAL_FLASH_Lock();
  return (status == HAL_OK) ? 1U : 0U;
}

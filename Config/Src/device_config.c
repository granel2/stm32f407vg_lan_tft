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
#define DEVICE_CONFIG_VERSION        2U
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
/* Static IP / "IP по умолчанию": used in static mode, and in DHCP mode as the
   fallback address when no DHCP server answers (see main.c). */
#define DEFAULT_STATIC_IP   {192U, 168U, 1U, 100U}
#define DEFAULT_NETMASK     {255U, 255U, 255U, 0U}
#define DEFAULT_GATEWAY     {192U, 168U, 1U, 1U}
/* TFT backlight (version 2) */
#define DEFAULT_BL_LEVEL      100U
#define DEFAULT_BL_DIM_LEVEL  20U
#define DEFAULT_BL_DIM_MIN    5U

/* Layout of version 1, exactly as it was saved by older firmware - only
   used to migrate such a config to the current version at boot. */
typedef struct
{
  uint32_t magic;
  uint32_t version;
  uint8_t  server_ip[4];
  uint16_t server_port;
  char     name[24];
  uint8_t  use_static_ip;
  uint8_t  static_ip[4];
  uint8_t  static_netmask[4];
  uint8_t  static_gw[4];
  uint32_t crc32;
} DeviceConfigV1;

static const uint8_t k_default_ip[4]   = DEFAULT_STATIC_IP;
static const uint8_t k_default_mask[4] = DEFAULT_NETMASK;
static const uint8_t k_default_gw[4]   = DEFAULT_GATEWAY;

static DeviceConfig s_config;
static uint32_t     s_revision;
static uint32_t     s_last_access;

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
  memcpy(cfg->static_ip,      k_default_ip,   4U);
  memcpy(cfg->static_netmask, k_default_mask, 4U);
  memcpy(cfg->static_gw,      k_default_gw,   4U);
  cfg->bl_level     = DEFAULT_BL_LEVEL;
  cfg->bl_dim_level = DEFAULT_BL_DIM_LEVEL;
  cfg->bl_dim_min   = DEFAULT_BL_DIM_MIN;
}

static uint8_t flash_config_valid(const DeviceConfig *cfg)
{
  return ((cfg->magic == DEVICE_CONFIG_MAGIC) &&
          (cfg->version == DEVICE_CONFIG_VERSION) &&
          (config_crc(cfg) == cfg->crc32)) ? 1U : 0U;
}

/* A valid version-1 config in Flash: keep every v1 setting, take defaults
   for what v2 added, and write it back as v2 right away (one ~1 s sector
   erase on the first boot of the new firmware) - so the web page's
   "saved for next boot" view (device_config_stored()) works at once. */
static uint8_t migrate_v1(const void *flash)
{
  const DeviceConfigV1 *v1 = (const DeviceConfigV1 *)flash;

  if ((v1->magic != DEVICE_CONFIG_MAGIC) || (v1->version != 1U) ||
      (crc32_compute((const uint8_t *)v1, (uint32_t)offsetof(DeviceConfigV1, crc32)) != v1->crc32))
  {
    return 0U;
  }
  set_defaults(&s_config);
  memcpy(s_config.server_ip, v1->server_ip, 4U);
  s_config.server_port = v1->server_port;
  memcpy(s_config.name, v1->name, sizeof(s_config.name));
  s_config.name[sizeof(s_config.name) - 1U] = '\0';
  s_config.use_static_ip = v1->use_static_ip;
  memcpy(s_config.static_ip,      v1->static_ip,      4U);
  memcpy(s_config.static_netmask, v1->static_netmask, 4U);
  memcpy(s_config.static_gw,      v1->static_gw,      4U);
  s_config.crc32 = config_crc(&s_config);
  (void)device_config_store(&s_config);
  return 1U;
}

void device_config_load(void)
{
  const DeviceConfig *flash_cfg = (const DeviceConfig *)(const void *)DEVICE_CONFIG_FLASH_ADDR;

  if (flash_config_valid(flash_cfg) != 0U)
  {
    memcpy(&s_config, flash_cfg, sizeof(s_config));
  }
  else if (migrate_v1(flash_cfg) == 0U)
  {
    set_defaults(&s_config);
  }
}

DeviceConfig *device_config_get(void)
{
  return &s_config;
}

uint8_t device_config_store(const DeviceConfig *cfg)
{
  DeviceConfig img = *cfg;
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t sector_error = 0;
  HAL_StatusTypeDef status;
  const uint32_t *src;
  uint32_t addr;
  uint32_t words;

  img.magic   = DEVICE_CONFIG_MAGIC;
  img.version = DEVICE_CONFIG_VERSION;
  img.crc32   = config_crc(&img);

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
    src   = (const uint32_t *)(const void *)&img;
    addr  = DEVICE_CONFIG_FLASH_ADDR;
    words = sizeof(img) / 4U;

    for (uint32_t i = 0; (i < words) && (status == HAL_OK); i++)
    {
      status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, src[i]);
      addr += 4U;
    }
  }

  HAL_FLASH_Lock();
  return (status == HAL_OK) ? 1U : 0U;
}

uint8_t device_config_save(void)
{
  if (device_config_store(&s_config) == 0U) { return 0U; }
  s_config.crc32 = config_crc(&s_config);
  s_revision++;
  return 1U;
}

const DeviceConfig *device_config_stored(void)
{
  const DeviceConfig *flash_cfg = (const DeviceConfig *)(const void *)DEVICE_CONFIG_FLASH_ADDR;
  return (flash_config_valid(flash_cfg) != 0U) ? flash_cfg : NULL;
}

void device_config_fallback_addr(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{
  /* Configs saved by older firmware have 0.0.0.0 here (static fields used
     to default to zero) - use the compiled-in default in that case. */
  const uint8_t use_cfg = ((s_config.static_ip[0] | s_config.static_ip[1] |
                            s_config.static_ip[2] | s_config.static_ip[3]) != 0U) &&
                          ((s_config.static_netmask[0] | s_config.static_netmask[1] |
                            s_config.static_netmask[2] | s_config.static_netmask[3]) != 0U);

  memcpy(ip,   use_cfg ? s_config.static_ip      : k_default_ip,   4U);
  memcpy(mask, use_cfg ? s_config.static_netmask : k_default_mask, 4U);
  memcpy(gw,   use_cfg ? s_config.static_gw      : k_default_gw,   4U);
}



void device_config_touch(void)
{
  s_last_access = HAL_GetTick() | 1U;   /* never 0 - 0 means "never" */
}

uint32_t device_config_last_access(void)
{
  return s_last_access;
}

uint32_t device_config_revision(void)
{
  return s_revision;
}

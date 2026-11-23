/**
  ******************************************************************************
  * @file    board_mac.c
  * @brief   See board_mac.h.
  ******************************************************************************
  */
#include "board_mac.h"

#include <stdio.h>

#include "stm32f4xx_hal.h"

/* Standard reflected CRC-32 (poly 0xEDB88320, init/final XOR 0xFFFFFFFF) -
   the same algorithm as zlib's crc32()/Python's binascii.crc32()/`cksum`,
   so the CHIP ID on screen can be cross-checked with any of those against
   the full UID logged over UART. Bit-by-bit, no table: runs over 12 bytes. */
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

static void uid_bytes(uint8_t bytes[12])
{
  const uint32_t w[3] = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };

  for (uint8_t i = 0; i < 3U; i++)
  {
    bytes[(i * 4U) + 0U] = (uint8_t)(w[i] >> 24);
    bytes[(i * 4U) + 1U] = (uint8_t)(w[i] >> 16);
    bytes[(i * 4U) + 2U] = (uint8_t)(w[i] >> 8);
    bytes[(i * 4U) + 3U] = (uint8_t)(w[i]);
  }
}

uint32_t board_chip_id(void)
{
  uint8_t bytes[12];

  uid_bytes(bytes);
  return crc32_compute(bytes, sizeof(bytes));
}

void board_mac_get(uint8_t mac[6])
{
  const uint32_t id = board_chip_id();

  /* 02:00 + the 4 CHIP ID bytes: the MAC can be read straight off the
     TFT's CHIP ID row. */
  mac[0] = 0x02U;                      /* locally administered, unicast */
  mac[1] = 0x00U;
  mac[2] = (uint8_t)(id >> 24);
  mac[3] = (uint8_t)(id >> 16);
  mac[4] = (uint8_t)(id >> 8);
  mac[5] = (uint8_t)(id);
}

void board_default_name(char *out, uint32_t out_size)
{
  snprintf(out, out_size, BOARD_LEGACY_DEFAULT_NAME "-%04lX",
           (unsigned long)(board_chip_id() & 0xFFFFU));
}

/**
  ******************************************************************************
  * @file    board_mac.h
  * @brief   Per-chip identity from the STM32 96-bit unique ID (UID,
  *          0x1FFF7A10), so one and the same firmware image can run on many
  *          modules on one LAN without MAC/DHCP collisions. All three values
  *          are tied to the CHIP ID shown on the TFT SETUP page:
  *
  *            CHIP ID = CRC32 of the 12 UID bytes        e.g. 1A2B3C4D
  *            MAC     = 02:00:<CHIP ID>                  02:00:1A:2B:3C:4D
  *                      02 = locally administered, unicast
  *            NAME    = "stm32f407-" + low 4 hex of ID   stm32f407-3C4D
  ******************************************************************************
  */
#ifndef BOARD_MAC_H
#define BOARD_MAC_H

#include <stdint.h>

/* Name the firmware used before board_mac existed - a config saved in Flash
   with exactly this name is treated as "never named" (see device_config.c). */
#define BOARD_LEGACY_DEFAULT_NAME "stm32f407"

uint32_t board_chip_id(void);
void     board_mac_get(uint8_t mac[6]);

/* "stm32f407-XXXX"; out_size >= 15. */
void     board_default_name(char *out, uint32_t out_size);

#endif /* BOARD_MAC_H */

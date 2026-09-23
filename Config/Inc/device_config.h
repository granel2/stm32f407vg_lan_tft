/**
  ******************************************************************************
  * @file    device_config.h
  * @brief   Persistent device configuration: TCP echo server address, device
  *          name/label, and static-IP-vs-DHCP choice. Backed by the last
  *          internal Flash sector (see device_config.c) so it survives
  *          power cycles - previously these were compile-time-only
  *          constants in tcp_echo_client.h, meaning changing any of them
  *          needed a full rebuild+reflash.
  *
  *          Layering: this module knows nothing about lwIP or the TCP
  *          config protocol that writes it (see Config/Src/config_server.c)
  *          - it only owns the in-RAM working copy and the Flash
  *          read/verify/write. Callers (main.c, tcp_echo_client.c,
  *          Display/) go through device_config_get() for the current
  *          values and device_config_save() to persist changes.
  ******************************************************************************
  */
#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct
{
  uint32_t magic;              /* DEVICE_CONFIG_MAGIC - distinguishes a real
                                   saved config from erased (0xFF...) or
                                   uninitialised flash */
  uint32_t version;            /* DEVICE_CONFIG_VERSION - bump and add
                                   migration logic in device_config_load()
                                   if this struct's layout ever changes */
  uint8_t  server_ip[4];       /* tcp_echo_client's target, e.g. {10,0,1,16} */
  uint16_t server_port;
  char     name[24];           /* device name/label, NUL-terminated; shown on
                                   the SETUP page and usable as a short
                                   human identifier alongside the CRC32 chip
                                   ID (see Display/README.md) */
  uint8_t  use_static_ip;      /* 0 = DHCP (default), 1 = use static_* below */
  uint8_t  static_ip[4];
  uint8_t  static_netmask[4];
  uint8_t  static_gw[4];
  uint32_t crc32;              /* over every byte above - device_config_load()
                                   falls back to defaults on a mismatch (blank
                                   flash, partial write, or a version bump
                                   with no migration path) */
} DeviceConfig;

/**
  * @brief  Loads the config from Flash into the in-RAM working copy, or
  *         resets it to compiled-in defaults if Flash holds no valid config
  *         (wrong magic/version, or a CRC32 mismatch). Call once at boot,
  *         before anything that needs server_ip/port, name, or the
  *         static-IP settings (MX_LWIP_Init(), TFT_App_SmokeTest(), ...).
  */
void device_config_load(void);

/**
  * @brief  The in-RAM working copy. Callers may read it directly and may
  *         write individual fields (that's how config_server.c applies
  *         "SET ..." commands) - call device_config_save() afterwards to
  *         persist, otherwise changes are lost on reset/power loss.
  */
DeviceConfig *device_config_get(void);

/**
  * @brief  Erases the config Flash sector and writes the current in-RAM
  *         copy (recomputing its CRC32 first). Blocking - a sector erase
  *         takes on the order of 1 s, during which the CPU cannot fetch
  *         from Flash at all, so the whole system (including Ethernet
  *         RX/TX and lwIP timers) is stalled for that long. This is a
  *         rare, operator-triggered action (config_server.c's "SAVE"
  *         command), not something on any periodic path, so that pause is
  *         an acceptable trade-off - same precedent as the boot-time
  *         TFT smoke test's ~7 s of blocking delays.
  * @retval 1 on success, 0 if the erase or any word program failed.
  */
uint8_t device_config_save(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_CONFIG_H */

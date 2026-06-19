/**
  ******************************************************************************
  * @file    ethernetif.h
  * @brief   lwIP <-> STM32 HAL_ETH (LAN8720, RMII) glue, NO_SYS=1 bare-metal port.
  ******************************************************************************
  */
#ifndef __ETHERNETIF_H__
#define __ETHERNETIF_H__

#include <stdint.h>
#include "lwip/err.h"
#include "lwip/netif.h"

/* Called once from MX_LWIP_Init() via netif_add(); brings up the MAC+PHY. */
err_t ethernetif_init(struct netif *netif);

/* Call every iteration of the main loop: drains all RX packets currently
   queued by the MAC and feeds them into the lwIP stack. Cheap to call when
   there is nothing to do. */
void ethernetif_input(struct netif *netif);

/* Returns non-zero if the ETH ISR has flagged a received frame (or an RX
   DMA error) since the last call to ethernetif_input(). Use this to decide
   whether it's worth calling ethernetif_input() this iteration. */
uint8_t ethernetif_rx_pending(void);

/* Call periodically (e.g. every 100-500 ms) from the main loop: polls the
   PHY link status and brings the netif up/down accordingly. Internally
   rate-limited, so calling it more often than that is harmless. */
void ethernetif_poll_link(struct netif *netif);

/* Full ETH MAC/DMA + PHY re-init, without rebooting the MCU. Use when the
   link/CPU are alive (HAL_GetTick advancing, no HardFault) but the network
   has gone silent - e.g. after a burst of traffic wedges the DMA descriptor
   ring so no further frames go in or out. Mirrors what a physical reset
   does to the ETH peripheral. */
void ethernetif_reset(struct netif *netif);

/* TEMPORARY bring-up breadcrumb, see ethernetif.c. */
extern volatile uint32_t g_eth_debug_marker;

/* TEMPORARY diagnostic: 0 = RX_ALLOC_OK, 1 = RX_ALLOC_ERROR (RX_POOL
   exhausted, low_level_input() stops calling HAL_ETH_ReadData() entirely
   until some pbuf is freed). See PROJECT_GUIDE.md. */
extern uint8_t g_eth_rx_alloc_status;

/* TEMPORARY diagnostic: raw return value of the last LAN8742_GetLinkState()
   call made inside ethernetif_poll_link() (see lan8742.h for the
   LAN8742_STATUS_* codes - e.g. 1 = LINK_DOWN, 2..5 = various up states,
   negative = MDIO read/write error). Lets the heartbeat show what the PHY
   chip itself is actually reporting, independent of netif_is_link_up(),
   to tell apart "PHY says link is up the whole time" (hardware/signal
   issue) from "PHY correctly reports down but netif logic doesn't act on
   it" (firmware bug). See PROJECT_GUIDE.md. */
extern int32_t g_eth_last_phy_link_state;

/* Dedicated INIT_FAIL flag, set by low_level_init() (ethernetif.c) on a
   HAL_ETH_Init() failure and cleared on success. Use this instead of
   g_eth_debug_marker==100 to detect an init failure from main.c:
   g_eth_debug_marker gets unconditionally overwritten by MX_LWIP_Init()
   right after the very first low_level_init() call (via netif_add()), so
   it is NOT safe for that purpose - confirmed on real hardware that the
   fast-escalation logic never fired because of exactly this. See
   PROJECT_GUIDE.md. */
extern uint8_t g_eth_hw_failed;

#endif /* __ETHERNETIF_H__ */

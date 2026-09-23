/**
  ******************************************************************************
  * @file    ethernetif.c
  * @brief   lwIP <-> STM32 HAL_ETH (LAN8720, RMII) glue, NO_SYS=1 bare-metal port.
  *
  * Adapted from ST's STM32CubeF4 LwIP_HTTP_Server_Netconn_RTOS example
  * (Projects/STM32F429ZI-Nucleo/Applications/LwIP/...): the FreeRTOS
  * semaphores/threads were replaced with plain polling, since this project
  * runs bare-metal (NO_SYS=1, see main.c's while(1) loop).
  ******************************************************************************
  */
#include "stm32f4xx_hal.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"
#include "lwip/stats.h"
#include "lwip/snmp.h"
#include "ethernetif.h"
#include "lan8742.h"
#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef huart1;
static void eth_debug(const char *s)
{
  HAL_UART_Transmit(&huart1, (const uint8_t *)s, (uint16_t)strlen(s), 50);
}

/* Define those to better describe your network interface. */
#define IFNAME0 's'
#define IFNAME1 't'

#define ETH_DMA_TRANSMIT_TIMEOUT       (20U)

/* LAN8720's internal REFCLKO-generation block (PLL/buffer downstream of the
   crystal - confirmed by scope that the crystal itself keeps oscillating
   fine) occasionally stops outputting 50 MHz on pin14/PA1, so
   HAL_ETH_Init() fails with "DMABMR.SWR timeout - no RMII REF_CLK" -
   confirmed by software edge-counting on PA1 (see eth_check_refclk_pa1())
   that the clock is genuinely absent, not just a STM32-side fluke.
   ROOT CAUSE FOUND (PROJECT_GUIDE.md): the actual fix was a too-weak
   pulldown on the LED2/nINTSEL strap pin racing the PHY's internal pull-up
   at nRST release, fixed in hardware (resistor swapped to 1k) - NOT a long
   nRST pulse, which was an earlier, disproven theory. With the strap fixed,
   a single HAL_ETH_Init() attempt should normally succeed, so this retry
   loop is kept only as a cheap safety net against rare transient glitches -
   small count, short gap between attempts. */
#define ETH_INIT_RETRY_COUNT           (1U)
#define ETH_INIT_RETRY_DELAY_MS        (200U)

/* If low_level_init() exhausts ETH_INIT_RETRY_COUNT and still fails this
   many times in a row, reboot the MCU immediately instead of waiting for
   the slower, generic "no IP" watchdog in main.c to notice - see
   PROJECT_GUIDE.md. */
#define ETH_INIT_FAIL_REBOOT_THRESHOLD (2U)

/* This app buffers receive packets of its primary service protocol for
   processing later. */
#define ETH_RX_BUFFER_CNT              (10U)

/*
@Note: This interface operates in zero-copy mode only:
   - Rx buffers come from a dedicated lwIP memory pool (RX_POOL below), handed
     to the ETH HAL driver via the RxAllocateCallback/RxLinkCallback hooks.
   - Tx buffers are simply the pbuf payloads already held by lwIP; we keep a
     ref on the pbuf until HAL_ETH_TxFreeCallback tells us it was sent.
*/
typedef enum
{
  RX_ALLOC_OK       = 0x00,
  RX_ALLOC_ERROR    = 0x01
} g_eth_rx_alloc_statusTypeDef;

typedef struct
{
  struct pbuf_custom pbuf_custom;
  uint8_t buff[(ETH_RX_BUF_SIZE + 31U) & ~31U] __ALIGNED(32);
} RxBuff_t;

static ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT]; /* Ethernet Rx DMA Descriptors */
static ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT]; /* Ethernet Tx DMA Descriptors */

LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(RxBuff_t), "Zero-copy RX PBUF pool");

uint8_t g_eth_rx_alloc_status;
int32_t g_eth_last_phy_link_state = LAN8742_STATUS_READ_ERROR;

/* Dedicated INIT_FAIL flag - g_eth_debug_marker alone isn't safe for this:
   MX_LWIP_Init() (main.c) unconditionally overwrites g_eth_debug_marker
   right after the very first low_level_init() call (via netif_add()), so a
   marker==100 set by a cold-boot HAL_ETH_Init() failure was getting wiped
   before the heartbeat/watchdog in main.c ever got a chance to see it -
   confirmed on real hardware (PROJECT_GUIDE.md): the fast escalation never
   fired because of exactly this. This flag is only ever written here, in
   low_level_init(), so nothing else can clobber it. */
uint8_t g_eth_hw_failed = 0U;

/* Set by HAL_ETH_RxCpltCallback()/HAL_ETH_ErrorCallback() (ISR context),
   cleared by ethernetif_input() once it has drained the queue. Avoids
   calling HAL_ETH_ReadData() on every single main-loop iteration when there
   is nothing to do. */
static volatile uint8_t EthRxPending = 0;

/* TEMPORARY bring-up breadcrumb: read via OpenOCD (`mdw &g_eth_debug_marker`)
   to see exactly how far low_level_init() got if it never returns. Remove
   once ETH/lwIP bring-up is confirmed stable. */
volatile uint32_t g_eth_debug_marker = 0;

/* Global Ethernet handle */
ETH_HandleTypeDef EthHandle;
static ETH_TxPacketConfig TxConfig;
lan8742_Object_t LAN8742;

/* Diagnostic: is the LAN8720 actually driving RMII_REF_CLK on PA1?
   Temporarily steals PA1 into TIM2_CH2 (AF1) "External Clock Mode 1" so
   TIM2 counts every rising edge it sees for ~1ms, then restores PA1 to its
   normal ETH AF11 function (HAL_ETH_MspInit() would redo this anyway on
   the next attempt, but restore now in case none follows). A healthy
   50 MHz REFCLKO should give a count in the tens of thousands; a dead/
   absent clock gives ~0. Only call this when ETH is already known broken
   (e.g. right after HAL_ETH_Init() fails) - it has nothing to do with
   normal operation. See PROJECT_GUIDE.md. */
static uint32_t eth_check_refclk_pa1(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  uint32_t count;

  __HAL_RCC_TIM2_CLK_ENABLE();

  GPIO_InitStruct.Pin = GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF1_TIM2;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  TIM2->CR1 = 0U;
  TIM2->CCMR1 = TIM_CCMR1_CC2S_0;                  /* CC2 channel as input, IC2 = TI2, no filter */
  TIM2->SMCR = TIM_SMCR_TS_2 | TIM_SMCR_TS_1 |      /* TS = 110 = TI2FP2 */
               TIM_SMCR_SMS_2 | TIM_SMCR_SMS_1 | TIM_SMCR_SMS_0; /* SMS = 111 = External Clock Mode 1 */
  TIM2->CNT = 0U;
  TIM2->CR1 |= TIM_CR1_CEN;

  HAL_Delay(1);

  count = TIM2->CNT;

  TIM2->CR1 = 0U;
  __HAL_RCC_TIM2_CLK_DISABLE();

  GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  return count;
}

/* Attempts to recover a stuck LAN8720 REFCLKO/PLL block via MDIO, as an
   alternative to a real power-cycle. Per the SMSC LAN8720/LAN8720i datasheet
   (Rev 1.0, Section 5.3.5.1 "General Power-Down" and the architecture block
   diagram in Figure 1.2): General Power-Down (BCR/register-0 bit 11) powers
   down "the entire transceiver, except the management interface" and, on
   wake, "the transceiver powers up and is automatically reset" - this is a
   different code path from the plain software-reset bit (BCR bit 15,
   already tried and confirmed NOT to fix this exact symptom - see
   PROJECT_GUIDE.md), and the datasheet's own block diagram shows the
   PLL/oscillator block as a separate block from "Reset Control" with no
   drawn connection between them, consistent with nRST/soft-reset never
   reaching it while General Power-Down might (it is documented to drop
   power to the analog core, not just reset digital state).
   This is raw MDIO (MACMIIAR/MACMIIDR), same technique as the diagnostic
   dump in HAL_ETH_MspInit() (stm32f4xx_hal_msp.c) - works independently of
   HAL_ETH_Init()/DMABMR state, only needs ETH_CLK + MDIO/MDC GPIO already
   configured (true here: HAL_ETH_MspInit() ran as part of every HAL_ETH_Init()
   attempt above, regardless of whether it ultimately timed out).
   Returns 1 if REFCLK came back (PA1 edges > 0 afterwards), 0 otherwise. */
static uint32_t eth_try_phy_power_cycle(void)
{
  volatile uint32_t *macmiiar = (volatile uint32_t *)0x40028010UL;
  volatile uint32_t *macmiidr = (volatile uint32_t *)0x40028014UL;
  uint32_t t;
  uint32_t addr;
  uint32_t found_addr = 0xFFUL;

#define PHY_MDIO_RD(a, r, v) do {                                         \
  *macmiiar = ((uint32_t)(a) << 11) | ((uint32_t)(r) << 6) | 0x14UL | 0x01UL; \
  t = HAL_GetTick();                                                      \
  while ((*macmiiar & 0x01U) && ((HAL_GetTick() - t) < 100U));            \
  (v) = (*macmiiar & 0x01U) ? 0xFFFFUL : (*macmiidr & 0xFFFFUL);          \
} while (0)

#define PHY_MDIO_WR(a, r, v) do {                                         \
  *macmiidr = (v);                                                        \
  *macmiiar = ((uint32_t)(a) << 11) | ((uint32_t)(r) << 6) | 0x14UL | 0x03UL; \
  t = HAL_GetTick();                                                      \
  while ((*macmiiar & 0x01U) && ((HAL_GetTick() - t) < 100U));            \
} while (0)

  for (addr = 0U; addr < 32U; addr++)
  {
    uint32_t id1;
    PHY_MDIO_RD(addr, 2, id1);
    if (id1 != 0xFFFFUL) { found_addr = addr; break; }
  }

  if (found_addr != 0xFFUL)
  {
    eth_debug("[eth] phy power-cycle: trying General Power-Down (BCR.11) toggle\r\n");
    PHY_MDIO_WR(found_addr, 0, 0x0800U); /* BCR: power down, clear all other bits */
    /* Confirmed on real hardware (PROJECT_GUIDE.md) that this toggle - at
       both 100 ms and 5 s hold - never actually recovers REFCLK (the real
       fault was a strap mis-latch, not anything an MDIO-side PHY register
       write can reach). Kept only as a cheap, near-free fallback attempt,
       so the hold is short - no point burning seconds on something with no
       evidence it ever helps. */
    HAL_Delay(200U);
    PHY_MDIO_WR(found_addr, 0, 0x0000U); /* BCR: clear power down - datasheet: "powers up and is automatically reset" */
    HAL_Delay(100U); /* let the crystal/PLL restart - same margin used for nRST release elsewhere */
  }
  else
  {
    eth_debug("[eth] phy power-cycle: no PHY responds on MDIO, skipping\r\n");
  }

#undef PHY_MDIO_RD
#undef PHY_MDIO_WR

  if (found_addr == 0xFFUL)
  {
    return 0U;
  }

  {
    uint32_t edges = eth_check_refclk_pa1();
    char msg[64];
    snprintf(msg, sizeof(msg),
             "[eth] phy power-cycle: PA1 edge count after toggle: %lu\r\n",
             (unsigned long)edges);
    eth_debug(msg);
    return (edges > 0U) ? 1U : 0U;
  }
}

static int32_t ETH_PHY_IO_Init(void);
static int32_t ETH_PHY_IO_DeInit(void);
static int32_t ETH_PHY_IO_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal);
static int32_t ETH_PHY_IO_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal);
static int32_t ETH_PHY_IO_GetTick(void);

static lan8742_IOCtx_t LAN8742_IOCtx = {ETH_PHY_IO_Init,
                                         ETH_PHY_IO_DeInit,
                                         ETH_PHY_IO_WriteReg,
                                         ETH_PHY_IO_ReadReg,
                                         ETH_PHY_IO_GetTick};

/*******************************************************************************
                       LL Driver Interface ( LwIP stack --> ETH)
*******************************************************************************/
static void low_level_init(struct netif *netif)
{
  uint32_t duplex, speed = 0;
  int32_t PHYLinkState = 0;
  ETH_MACConfigTypeDef MACConf = {0};
  uint8_t macaddress[6] = {ETH_MAC_ADDR0, ETH_MAC_ADDR1, ETH_MAC_ADDR2,
                            ETH_MAC_ADDR3, ETH_MAC_ADDR4, ETH_MAC_ADDR5};

  EthHandle.Instance = ETH;
  EthHandle.Init.MACAddr = macaddress;
  EthHandle.Init.MediaInterface = HAL_ETH_RMII_MODE;
  EthHandle.Init.RxDesc = DMARxDscrTab;
  EthHandle.Init.TxDesc = DMATxDscrTab;
  EthHandle.Init.RxBuffLen = ETH_RX_BUF_SIZE;

  g_eth_debug_marker = 1;
  /* configure ethernet peripheral (GPIOs/clocks done in HAL_ETH_MspInit, MAC+DMA here).
     Retry on failure - see ETH_INIT_RETRY_COUNT comment above. */
  {
    uint32_t init_try;
    HAL_StatusTypeDef init_status = HAL_ERROR;
    static uint32_t s_init_fail_streak = 0;

    for (init_try = 0U; init_try < ETH_INIT_RETRY_COUNT; init_try++)
    {
      if (init_try > 0U)
      {
        char retry_msg[48];
        snprintf(retry_msg, sizeof(retry_msg),
                 "[eth] HAL_ETH_Init retry %lu/%lu\r\n",
                 (unsigned long)(init_try + 1U), (unsigned long)ETH_INIT_RETRY_COUNT);
        eth_debug(retry_msg);
        /* Deliberate pause before the next nRST attempt - see
           ETH_INIT_RETRY_DELAY_MS comment above. */
        HAL_Delay(ETH_INIT_RETRY_DELAY_MS);
        HAL_ETH_DeInit(&EthHandle);
      }

      init_status = HAL_ETH_Init(&EthHandle);
      if (init_status == HAL_OK)
      {
        break;
      }
    }

    if (init_status != HAL_OK)
    {
      uint32_t refclk_edges = eth_check_refclk_pa1();
      char refclk_msg[64];

      g_eth_debug_marker = 100;
      g_eth_hw_failed = 1U;
      /* DMABMR.SWR never cleared — RMII 50 MHz REF_CLK from LAN8720 absent.
         EthHandle.gState stays ERROR so ethernetif_poll_link skips PHY access. */
      eth_debug("[eth] HAL_ETH_Init FAILED after retries (DMABMR.SWR timeout - no RMII REF_CLK?)\r\n");
      snprintf(refclk_msg, sizeof(refclk_msg),
               "[eth] PA1 edge count in ~1ms: %lu (>>0 = REFCLK present)\r\n",
               (unsigned long)refclk_edges);
      eth_debug(refclk_msg);

      /* Before escalating to a full MCU reboot (which only works because
         nRST happens to be asserted early in main() - see main.c - not
         because it actually fixes the PHY by itself): try a software-only
         PHY power-cycle via MDIO (General Power-Down, BCR bit 11) - see
         eth_try_phy_power_cycle() above for why this is a genuinely
         different mechanism from the nRST/soft-reset paths already proven
         insufficient on real hardware. If it brings REFCLK back, retry
         HAL_ETH_Init() once more right away instead of escalating. */
      if (refclk_edges == 0U && eth_try_phy_power_cycle())
      {
        eth_debug("[eth] phy power-cycle: REFCLK recovered, retrying HAL_ETH_Init\r\n");
        HAL_ETH_DeInit(&EthHandle);
        init_status = HAL_ETH_Init(&EthHandle);
      }

      if (init_status != HAL_OK)
      {
        /* Don't wait out the slow 90s "no IP" watchdog in main.c to find out
           this failed - eth_check_refclk_pa1() already gave a definitive,
           immediate answer (0 edges = REFCLK genuinely absent), so further
           delay before reacting only wastes time. A real MCU reboot (through
           main(), where nRST is now asserted before anything else - see
           main.c) is the only thing observed to reliably recover this on
           real hardware; escalate fast instead of retrying in place forever.
           See PROJECT_GUIDE.md. */
        s_init_fail_streak++;
        if (s_init_fail_streak >= ETH_INIT_FAIL_REBOOT_THRESHOLD)
        {
          eth_debug("[eth] too many consecutive HAL_ETH_Init failures, rebooting MCU\r\n");
          HAL_Delay(50); /* let the UART finish transmitting before reset */
          NVIC_SystemReset();
        }

        netif_set_link_down(netif);
        netif_set_down(netif);
        return;
      }
    }

    s_init_fail_streak = 0;
  }
  g_eth_hw_failed = 0U;
  g_eth_debug_marker = 2;
  eth_debug("[eth] HAL_ETH_Init OK\r\n");

  netif->hwaddr_len = ETH_HWADDR_LEN;
  netif->hwaddr[0] = ETH_MAC_ADDR0;
  netif->hwaddr[1] = ETH_MAC_ADDR1;
  netif->hwaddr[2] = ETH_MAC_ADDR2;
  netif->hwaddr[3] = ETH_MAC_ADDR3;
  netif->hwaddr[4] = ETH_MAC_ADDR4;
  netif->hwaddr[5] = ETH_MAC_ADDR5;

  netif->mtu = ETH_MAX_PAYLOAD;
  netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;

  g_eth_debug_marker = 3;
  LWIP_MEMPOOL_INIT(RX_POOL);
  g_eth_debug_marker = 4;

  memset(&TxConfig, 0, sizeof(ETH_TxPacketConfig));
  TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
  TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
  TxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;

  /* Set PHY IO functions, then bring the LAN8720 PHY up (PHY_ADDRESS = 1 on this board, see f407.sch) */
  LAN8742_RegisterBusIO(&LAN8742, &LAN8742_IOCtx);
  g_eth_debug_marker = 5;

  if (LAN8742_Init(&LAN8742) != LAN8742_STATUS_OK)
  {
    g_eth_debug_marker = 101;
    netif_set_link_down(netif);
    netif_set_down(netif);
    return;
  }
  g_eth_debug_marker = 6;

  PHYLinkState = LAN8742_GetLinkState(&LAN8742);
  g_eth_debug_marker = 7;

  if (PHYLinkState <= LAN8742_STATUS_LINK_DOWN)
  {
    g_eth_debug_marker = 8;
    netif_set_link_down(netif);
    netif_set_down(netif);
  }
  else
  {
    g_eth_debug_marker = 9;
    switch (PHYLinkState)
    {
      case LAN8742_STATUS_100MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE;
        speed = ETH_SPEED_100M;
        break;
      case LAN8742_STATUS_100MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE;
        speed = ETH_SPEED_100M;
        break;
      case LAN8742_STATUS_10MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE;
        speed = ETH_SPEED_10M;
        break;
      case LAN8742_STATUS_10MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE;
        speed = ETH_SPEED_10M;
        break;
      default:
        duplex = ETH_FULLDUPLEX_MODE;
        speed = ETH_SPEED_100M;
        break;
    }

    g_eth_debug_marker = 10;
    HAL_ETH_GetMACConfig(&EthHandle, &MACConf);
    MACConf.DuplexMode = duplex;
    MACConf.Speed = speed;
    HAL_ETH_SetMACConfig(&EthHandle, &MACConf);
    g_eth_debug_marker = 11;
    HAL_ETH_Start_IT(&EthHandle);
    g_eth_debug_marker = 12;
    netif_set_up(netif);
    netif_set_link_up(netif);
    g_eth_debug_marker = 13;
  }
}

/* Root cause (confirmed by source inspection of stm32f4xx_hal_eth.c):
   HAL_ETH_ReleaseTxPacket() walks the ring starting at TxDescList.releaseIndex
   for up to BuffersInUse iterations, but only commits releaseIndex/BuffersInUse
   back to the handle inside its SUCCESS branch (DESC0 OWN==0 *and*
   PacketAddress[idx]!=NULL). The moment it meets, anywhere in that walk, a
   descriptor whose OWN bit is still set, it breaks out and throws away all
   the NULL-slot skipping it already did - releaseIndex/BuffersInUse stay
   exactly as they were. Once that happens once, it happens on every later
   call too (same frozen releaseIndex, same eventual stuck slot), so
   PacketAddress[] entries for already-transmitted packets are never cleared.
   ETH_Prepare_Tx_Descriptors() then refuses to reuse a slot whenever
   PacketAddress[idx]!=NULL (stm32f4xx_hal_eth.c:3137-3141), regardless of
   OWN - so this leaks one TxDescList slot's pbuf ref and BuffersInUse count
   forever, every time it triggers, until lwIP's MEM_SIZE heap is exhausted.

   Fix: don't use HAL_ETH_ReleaseTxPacket()'s FIFO bookkeeping at all. Scan
   every physical slot in the ring directly each call; free and clear any
   slot that is actually done (PacketAddress!=NULL && OWN==0), independent
   of releaseIndex order. This keeps PacketAddress[] always accurate, so
   Prepare's busy check never trips on a stale entry. */
static void eth_tx_drain_all(void)
{
  uint32_t idx;
  uint32_t inUse = 0U;

  for (idx = 0U; idx < ETH_TX_DESC_CNT; idx++)
  {
    if (EthHandle.TxDescList.PacketAddress[idx] != NULL)
    {
      if ((EthHandle.Init.TxDesc[idx].DESC0 & ETH_DMATXDESC_OWN) == 0U)
      {
        HAL_ETH_TxFreeCallback(EthHandle.TxDescList.PacketAddress[idx]);
        EthHandle.TxDescList.PacketAddress[idx] = NULL;
      }
      else
      {
        inUse++;
      }
    }
  }
  EthHandle.TxDescList.BuffersInUse = inUse;
  EthHandle.TxDescList.releaseIndex = EthHandle.TxDescList.CurTxDesc;
}

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
  uint32_t i = 0U;
  struct pbuf *q = NULL;
  err_t errval = ERR_OK;
  ETH_BufferTypeDef Txbuffer[ETH_TX_DESC_CNT] = {0};

  LWIP_UNUSED_ARG(netif);

  memset(Txbuffer, 0, ETH_TX_DESC_CNT * sizeof(ETH_BufferTypeDef));

  for (q = p; q != NULL; q = q->next)
  {
    if (i >= ETH_TX_DESC_CNT)
    {
      return ERR_IF;
    }

    Txbuffer[i].buffer = q->payload;
    Txbuffer[i].len = q->len;

    if (i > 0)
    {
      Txbuffer[i - 1].next = &Txbuffer[i];
    }

    if (q->next == NULL)
    {
      Txbuffer[i].next = NULL;
    }

    i++;
  }

  TxConfig.Length = p->tot_len;
  TxConfig.TxBuffer = Txbuffer;
  TxConfig.pData = p;

  /* Free every descriptor slot whose transmit has actually completed,
     independent of HAL's own (buggy) releaseIndex bookkeeping - see
     eth_tx_drain_all(). */
  eth_tx_drain_all();

  /* Root cause found (PROJECT_GUIDE.md): HAL_ETH_Transmit() (polling mode,
     used here) never sets TxDescList.CurrentPacketAddress - only its
     interrupt-mode sibling HAL_ETH_Transmit_IT() does
     (stm32f4xx_hal_eth.c:1048). ETH_Prepare_Tx_Descriptors() then stores
     that (always-NULL/stale) value into PacketAddress[descidx]
     (stm32f4xx_hal_eth.c:3259), so PacketAddress[] never actually points at
     our pbuf. Every pbuf_ref() below was therefore permanently unmatched -
     eth_tx_drain_all() (and, before it, HAL's own ReleaseTxPacket()) could
     never find the slot to free it, leaking exactly one ref of every
     transmitted pbuf forever. Set it ourselves, exactly like
     HAL_ETH_Transmit_IT() does. */
  EthHandle.TxDescList.CurrentPacketAddress = (uint32_t *)p;

  pbuf_ref(p);

  EthHandle.ErrorCode = HAL_ETH_ERROR_NONE;
  if (HAL_ETH_Transmit(&EthHandle, &TxConfig, ETH_DMA_TRANSMIT_TIMEOUT) != HAL_OK)
  {
    /* HAL_ETH_ERROR_TIMEOUT means ETH_Prepare_Tx_Descriptors() already
       recorded PacketAddress[idx] = p before the wait-for-OWN-bit loop
       timed out; HAL_ETH_Transmit() force-clears the OWN bit on timeout,
       so the *next* HAL_ETH_ReleaseTxPacket() call will see it as done and
       call HAL_ETH_TxFreeCallback() -> pbuf_free(p) on its own. Freeing it
       here too would double-free the same pbuf and corrupt lwIP's heap -
       this was silently corrupting memory after every burst of traffic.
       Any other error (e.g. HAL_ETH_ERROR_BUSY) means the descriptor was
       never queued, so our extra pbuf_ref() above is the only one left
       and must be released here. */
    if ((EthHandle.ErrorCode & HAL_ETH_ERROR_TIMEOUT) == 0U)
    {
      pbuf_free(p);
    }
    errval = ERR_IF;
  }

  return errval;
}

static struct pbuf *low_level_input(struct netif *netif)
{
  struct pbuf *p = NULL;

  LWIP_UNUSED_ARG(netif);

  if (g_eth_rx_alloc_status == RX_ALLOC_OK)
  {
    HAL_ETH_ReadData(&EthHandle, (void **)&p);
  }

  return p;
}

/* The heap/pool wipe in ethernetif_reset() hands every TCP_PCB/TCP_SEG slot
   back to the free lists - but lwIP's own tcp_active_pcbs/tcp_tw_pcbs/
   tcp_bound_pcbs lists would still link those same slots. The next
   tcp_alloc() then returns a pcb that is *already* on a list, and
   re-registering it closes the list into a ring: tcp_input()/tcp_slowtmr()
   then loop forever (observed: two port-80 TIME_WAIT pcbs pointing at each
   other after a web-config save, CPU spinning in tcp_input's TIME_WAIT
   scan). So drop every non-listening pcb properly first. tcp_abort() calls
   each pcb's err callback, so tcp_echo_client/config_server clear their
   own pointers exactly as on any other connection loss. */
static uint8_t s_use_dhcp = 1U;

void ethernetif_set_use_dhcp(uint8_t use_dhcp)
{
  s_use_dhcp = use_dhcp;
}

static void tcp_abort_all_for_reset(void)
{
  while (tcp_active_pcbs != NULL) { tcp_abort(tcp_active_pcbs); }
  while (tcp_tw_pcbs != NULL)     { tcp_abort(tcp_tw_pcbs); }
  while (tcp_bound_pcbs != NULL)  { tcp_abort(tcp_bound_pcbs); }
}

void ethernetif_reset(struct netif *netif)
{
  /* tcp_abort_all_for_reset() below runs tcp_echo_client's on_err, which can
     itself decide to call ethernetif_reset() ("too many failed connects") -
     ignore that nested call, this one is already doing the reset. */
  static uint8_t in_reset;
  if (in_reset != 0U) { return; }
  in_reset = 1U;

  eth_debug("[eth] resetting ETH peripheral (no traffic, CPU alive)\r\n");

  HAL_ETH_Stop_IT(&EthHandle);
  HAL_ETH_DeInit(&EthHandle);
  netif_set_down(netif);
  netif_set_link_down(netif);

  /* HAL_ETH_DeInit() only disables the MAC/DMA via register bits - it does
     NOT toggle the AHB1 peripheral reset line, so a wedged DMA state machine
     can survive DeInit+Init and only clears on a real power-cycle. Force a
     genuine hardware reset of the ETH block via RCC, same as POR does. */
  __HAL_RCC_ETHMAC_FORCE_RESET();
  HAL_Delay(2);
  __HAL_RCC_ETHMAC_RELEASE_RESET();

  /* HAL_ETH_Init()/DeInit() never touch TxDescList/RxDescList - confirmed by
     grepping the HAL source: BuffersInUse, CurTxDesc, releaseIndex etc. are
     only ever written by Transmit/ReleaseTxPacket, never reset. Without this,
     BuffersInUse keeps climbing across every ethernetif_reset() call instead
     of going back to 0, even though the physical ring is brand new. */
  memset(&EthHandle.TxDescList, 0, sizeof(EthHandle.TxDescList));
  memset(&EthHandle.RxDescList, 0, sizeof(EthHandle.RxDescList));

  /* g_eth_rx_alloc_status/EthRxPending and the RX_POOL free list may be in an
     inconsistent state after a wedged DMA ring; low_level_init() re-creates
     all of it (HAL_ETH_Init, LWIP_MEMPOOL_INIT, PHY check, HAL_ETH_Start_IT). */
  g_eth_rx_alloc_status = RX_ALLOC_OK;
  EthRxPending  = 0;

  /* ethernetif_reset() only re-creates the HARDWARE side (MAC/DMA/PHY).
     lwIP's own ARP table is separate software state that this never
     touches - if it holds a stale/incomplete entry for the peer (or the
     gateway), TCP connect attempts can keep failing forever even with a
     perfectly healthy NIC underneath, matching the observed symptom that
     only a full power-cycle (which re-runs lwip_init() from scratch) was
     reliably able to recover - not this reset. Clear it explicitly so a
     stale entry can't outlive the hardware reset that's supposed to fix
     connectivity. */
  etharp_cleanup_netif(netif);

  /* Root cause found (PROJECT_GUIDE.md): tx pbufs held by the TX-descriptor
     ring (via the extra pbuf_ref() in low_level_output()) are never freed
     back to lwIP's MEM_SIZE heap, because HAL_ETH_ReleaseTxPacket()'s FIFO
     never actually drains under sustained traffic. Once the heap is full,
     tcp_write()/tcp_connect() keep failing with ERR_MEM forever - and since
     this heap lives in lwIP's own static memory, not the ETH peripheral,
     none of the hardware-level recovery above (RCC reset, descriptor list
     memset, ARP cleanup) touches it. This is almost certainly *the* reason
     a full power-cycle was the only thing that ever reliably recovered
     connectivity: it's the only thing that previously re-ran mem_init().
     Wipe the heap and all standard memp pools (TCP_PCB, TCP_SEG, PBUF,
     UDP_PCB, ...) here instead, now that TxDescList/RxDescList have already
     been zeroed above so nothing still references the old heap. The
     RX_POOL custom pool is separate (LWIP_MEMPOOL_DECLARE) and is already
     re-initialized inside low_level_init(). */
  /* dhcp_stop() halts the client but does NOT free the struct dhcp that
     dhcp_start() mem_malloc'd back at boot - netif->dhcp keeps pointing at
     it. mem_init() below then resets the WHOLE heap free list, so that
     same memory can be handed out again for something else while
     netif->dhcp still points at it - a dangling pointer that corrupted
     state and caused the very first observed HardFault (low_level_output,
     PC matched ethernetif.c via the .map file) shortly after a reset.
     dhcp_cleanup() actually mem_free()s the struct and clears netif's
     client-data slot, so nothing is left pointing into the heap we're
     about to wipe. */
  dhcp_stop(netif);
  dhcp_cleanup(netif);
  tcp_abort_all_for_reset();
  mem_init();
  /* Every pool except TCP_PCB_LISTEN: the listening pcbs (config_server
     port 7000, config_http port 80) own no heap/seg memory, are allocated
     once at boot and never again - wiping their pool would make them
     "free" while still listening. Keeping them means both configurators
     keep working across an ETH reset without re-initialising. */
  for (u16_t i = 0; i < (u16_t)MEMP_MAX; i++)
  {
    if (i != (u16_t)MEMP_TCP_PCB_LISTEN) { memp_init_pool(memp_pools[i]); }
  }
  /* mem_init() only resets the real allocator's free list - it does NOT
     touch lwip_stats.mem.used (that's a separate bookkeeping counter, only
     ever incremented/decremented by individual malloc/free calls). Left
     alone, it keeps accumulating across every ethernetif_reset() instead of
     reflecting the heap we just wiped, eventually reading far past
     MEM_SIZE even though the real heap is healthy - confirmed by
     instrumentation (PROJECT_GUIDE.md). Zero it so the heap= diagnostic
     stays trustworthy after a reset. */
  memset(&lwip_stats.mem, 0, sizeof(lwip_stats.mem));
  MEM_STATS_AVAIL(avail, MEM_SIZE);

  low_level_init(netif);

  /* low_level_init() never restarts DHCP (only MX_LWIP_Init() and
     ethernetif_poll_link()'s link-up transition do) - without this the
     module would keep reusing the old IP forever with no active lease
     renewal after a reset. Safe to call unconditionally: dhcp_start() is
     a no-op-safe re-arm, not a duplicate-start error. */
  if (netif_is_link_up(netif) && (s_use_dhcp != 0U))
  {
    dhcp_start(netif);
  }
  in_reset = 0U;
}

void ethernetif_input(struct netif *netif)
{
  struct pbuf *p;

  EthRxPending = 0;

  do
  {
    p = low_level_input(netif);
    if (p != NULL)
    {
      if (netif->input(p, netif) != ERR_OK)
      {
        pbuf_free(p);
      }
    }
  } while (p != NULL);
}

err_t ethernetif_init(struct netif *netif)
{
  LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
  netif->hostname = "stm32f407";
#endif

  netif->name[0] = IFNAME0;
  netif->name[1] = IFNAME1;
  netif->output = etharp_output;
  netif->linkoutput = low_level_output;

  low_level_init(netif);

  return ERR_OK;
}

void pbuf_free_custom(struct pbuf *p)
{
  struct pbuf_custom *custom_pbuf = (struct pbuf_custom *)p;
  LWIP_MEMPOOL_FREE(RX_POOL, custom_pbuf);

  if (g_eth_rx_alloc_status == RX_ALLOC_ERROR)
  {
    g_eth_rx_alloc_status = RX_ALLOC_OK;
  }
}

/**
  * @brief  Returns the current time in milliseconds, used by lwIP when
  *         NO_SYS == 1 (see lwip/timeouts.c, dhcp.c, etc).
  */
u32_t sys_now(void)
{
  return HAL_GetTick();
}

/*******************************************************************************
                       Ethernet HAL callbacks (ISR context)
*******************************************************************************/
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth)
{
  LWIP_UNUSED_ARG(heth);
  EthRxPending = 1;
}

void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *heth)
{
  if ((HAL_ETH_GetDMAError(heth) & ETH_DMASR_RBUS) == ETH_DMASR_RBUS)
  {
    EthRxPending = 1;
  }
}

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
  struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);
  if (p)
  {
    *buff = (uint8_t *)p + offsetof(RxBuff_t, buff);
    p->custom_free_function = pbuf_free_custom;
    pbuf_alloced_custom(PBUF_RAW, 0, PBUF_REF, p, *buff, ETH_RX_BUF_SIZE);
  }
  else
  {
    g_eth_rx_alloc_status = RX_ALLOC_ERROR;
    *buff = NULL;
  }
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
  struct pbuf **ppStart = (struct pbuf **)pStart;
  struct pbuf **ppEnd = (struct pbuf **)pEnd;
  struct pbuf *p = NULL;

  p = (struct pbuf *)(buff - offsetof(RxBuff_t, buff));
  p->next = NULL;
  p->tot_len = 0;
  p->len = Length;

  if (!*ppStart)
  {
    *ppStart = p;
  }
  else
  {
    (*ppEnd)->next = p;
  }
  *ppEnd = p;

  for (p = *ppStart; p != NULL; p = p->next)
  {
    p->tot_len += Length;
  }
}

void HAL_ETH_TxFreeCallback(uint32_t *buff)
{
  pbuf_free((struct pbuf *)buff);
}

/*******************************************************************************
                       PHY IO Functions (MDIO bus access via HAL_ETH)
*******************************************************************************/
static int32_t ETH_PHY_IO_Init(void)
{
  HAL_ETH_SetMDIOClockRange(&EthHandle);
  return 0;
}

static int32_t ETH_PHY_IO_DeInit(void)
{
  return 0;
}

static int32_t ETH_PHY_IO_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal)
{
  if (HAL_ETH_ReadPHYRegister(&EthHandle, DevAddr, RegAddr, pRegVal) != HAL_OK)
  {
    return -1;
  }
  return 0;
}

static int32_t ETH_PHY_IO_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal)
{
  if (HAL_ETH_WritePHYRegister(&EthHandle, DevAddr, RegAddr, RegVal) != HAL_OK)
  {
    return -1;
  }
  return 0;
}

static int32_t ETH_PHY_IO_GetTick(void)
{
  return (int32_t)HAL_GetTick();
}

/**
  * @brief  Poll the ETH MAC for a pending RX/error event. Call every
  *         main-loop iteration; cheap no-op when EthRxPending is clear.
  */
uint8_t ethernetif_rx_pending(void)
{
  return EthRxPending;
}

/**
  * @brief  Check the PHY link state and update the netif accordingly.
  *         Rate-limited internally to roughly twice a second - call this
  *         every main-loop iteration, no need to time it externally.
  */
void ethernetif_poll_link(struct netif *netif)
{
  static uint32_t last_check = 0;
  ETH_MACConfigTypeDef MACConf = {0};
  int32_t PHYLinkState;
  uint32_t speed = 0, duplex = 0, linkchanged = 0;

  /* Guard: if HAL_ETH_Init failed (e.g. RMII REF_CLK absent), EthHandle.gState
     is not READY and LAN8742.IO.ReadReg is NULL — calling GetLinkState would
     dereference NULL and trigger a UsageFault/HardFault. */
  if (EthHandle.gState != HAL_ETH_STATE_READY)
  {
    return;
  }

  if ((HAL_GetTick() - last_check) < 500U)
  {
    return;
  }
  last_check = HAL_GetTick();

  PHYLinkState = LAN8742_GetLinkState(&LAN8742);
  g_eth_last_phy_link_state = PHYLinkState;

  if (netif_is_link_up(netif) && (PHYLinkState <= LAN8742_STATUS_LINK_DOWN))
  {
    HAL_ETH_Stop_IT(&EthHandle);
    dhcp_stop(netif);
    netif_set_down(netif);
    netif_set_link_down(netif);
  }
  else if (!netif_is_link_up(netif) && (PHYLinkState > LAN8742_STATUS_LINK_DOWN))
  {
    switch (PHYLinkState)
    {
      case LAN8742_STATUS_100MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE; speed = ETH_SPEED_100M; linkchanged = 1; break;
      case LAN8742_STATUS_100MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE; speed = ETH_SPEED_100M; linkchanged = 1; break;
      case LAN8742_STATUS_10MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE; speed = ETH_SPEED_10M; linkchanged = 1; break;
      case LAN8742_STATUS_10MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE; speed = ETH_SPEED_10M; linkchanged = 1; break;
      default:
        break;
    }

    if (linkchanged)
    {
      HAL_ETH_GetMACConfig(&EthHandle, &MACConf);
      MACConf.DuplexMode = duplex;
      MACConf.Speed = speed;
      HAL_ETH_SetMACConfig(&EthHandle, &MACConf);
      HAL_ETH_Start_IT(&EthHandle);
      netif_set_up(netif);
      netif_set_link_up(netif);
      if (s_use_dhcp != 0U) { dhcp_start(netif); }
    }
  }
}

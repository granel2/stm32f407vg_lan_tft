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
#include "netif/ethernet.h"
#include "netif/etharp.h"
#include "lwip/stats.h"
#include "lwip/snmp.h"
#include "ethernetif.h"
#include "lan8742.h"
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
  /* configure ethernet peripheral (GPIOs/clocks done in HAL_ETH_MspInit, MAC+DMA here) */
  if (HAL_ETH_Init(&EthHandle) != HAL_OK)
  {
    g_eth_debug_marker = 100;
    /* DMABMR.SWR never cleared — RMII 50 MHz REF_CLK from LAN8720 absent.
       EthHandle.gState stays ERROR so ethernetif_poll_link skips PHY access. */
    eth_debug("[eth] HAL_ETH_Init FAILED (DMABMR.SWR timeout - no RMII REF_CLK?)\r\n");
    netif_set_link_down(netif);
    netif_set_down(netif);
    return;
  }
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

void ethernetif_reset(struct netif *netif)
{
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
  mem_init();
  memp_init();
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
  if (netif_is_link_up(netif))
  {
    dhcp_start(netif);
  }
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
      dhcp_start(netif);
    }
  }
}

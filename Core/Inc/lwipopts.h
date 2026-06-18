/**
  ******************************************************************************
  * @file    lwipopts.h
  * @brief   lwIP configuration for this project.
  *
  * NO_SYS=1 (bare-metal, no RTOS): the whole stack runs from the main loop via
  * ethernetif_input() + sys_check_timeouts(), see main.c.
  ******************************************************************************
  */
#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

/**
 * NO_SYS==1: bare-metal, no RTOS. The raw API is used directly from main();
 * there is no tcpip thread, no mailboxes, no semaphores.
 */
#define NO_SYS                  1
#define LWIP_NETCONN            0
#define LWIP_SOCKET             0
#define LWIP_NETIF_API          0

/* ---------- Memory options ---------- */
#define MEM_ALIGNMENT           4

/* MEM_SIZE: lwIP's own static heap (not newlib malloc) - used by mem_malloc()
   for PBUF_RAM allocations (e.g. outgoing TCP segments).
   TCP_SND_BUF = 4*1460 = 5840 bytes + lwIP overhead; 16KB gives comfortable
   headroom so tcp_write never returns ERR_MEM during normal operation. */
#define MEM_SIZE                (16 * 1024)

#define MEMP_NUM_PBUF           10
#define MEMP_NUM_UDP_PCB        4
#define MEMP_NUM_TCP_PCB        4
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define MEMP_NUM_TCP_SEG        TCP_SND_QUEUELEN
#define MEMP_NUM_SYS_TIMEOUT    10

/* ---------- Pbuf options ---------- */
#define PBUF_POOL_SIZE          8
#define PBUF_POOL_BUFSIZE       1524

/* ---------- TCP options ---------- */
#define LWIP_TCP                1
#define TCP_TTL                 255
#define TCP_QUEUE_OOSEQ         0
#define TCP_MSS                 (1500 - 40)
#define TCP_SND_BUF             (4 * TCP_MSS)
#define TCP_WND                 (2 * TCP_MSS)

/* TCP_OVERSIZE defaults to TCP_MSS (1460): every tcp_write() that starts a
   new unsent tail segment reserves that much spare heap capacity so later
   small writes can append without a fresh allocation. We always call
   tcp_output() right after tcp_write() (see tcp_echo_client.c tx_flush()),
   so every write starts a fresh tail anyway - we never benefit from the
   reuse, only pay for the reservation. Confirmed by instrumentation
   (PROJECT_GUIDE.md): lwip_stats.mem.used grew by ~1.4-1.5KB per message
   sent and never shrank back even after 20+ s idle, exhausting MEM_SIZE
   (16KB) after ~12 messages and making tcp_connect() fail with ERR_MEM on
   the next reconnect. 0 = allocate the exact size every time (officially
   supported value, see lwip/opt.h) - removes the spare capacity entirely. */
#define TCP_OVERSIZE             0

/* Retransmit limits: default TCP_MAXRTX=12 means ~7.5 minutes before lwIP
   calls on_err when the server goes silent (no RST/FIN, just dead).
   With 4 retransmits: 1+2+4+8 = 15 seconds to detect a dead server.   */
#define TCP_MAXRTX              4U
#define TCP_SYNMAXRTX           3U    /* SYN timeout: 1+2+4 = 7 s            */

/* ---------- TCP keepalive ------------------------------------------
   Send a keepalive probe after 10 s idle, then every 5 s, give up after
   3 missed probes (= 25 s total).  This lets the module detect a dead
   server connection on its own and trigger on_err → reconnect, rather
   than waiting for the server to time out and send RST.              */
#define LWIP_TCP_KEEPALIVE              1
#define TCP_KEEPIDLE_DEFAULT            10000UL   /* ms before first probe */
#define TCP_KEEPINTVL_DEFAULT           5000UL    /* ms between probes      */
#define TCP_KEEPCNT_DEFAULT             3U        /* probes before giving up */

/* ---------- ICMP options (so the board answers ping) ---------- */
#define LWIP_ICMP                       1

/* ---------- DHCP options ---------- */
#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     0

/* ---------- UDP options ---------- */
#define LWIP_UDP                1
#define UDP_TTL                 255

/* ---------- ARP options ---------- */
#define LWIP_ARP                 1

/* ---------- Statistics options ---------- */
/* TEMPORARY: 1 while chasing the "tcp_connect() fails with ERR_MEM forever
   after one disconnect" bug (see PROJECT_GUIDE.md) - lets tcp_echo_client.c
   read lwip_stats.mem / lwip_stats.memp[] to see exactly what's exhausted. */
#define LWIP_STATS               1
#define MEM_STATS                1
#define MEMP_STATS                1
/* LWIP_PROVIDE_ERRNO is already defined (unconditionally) in arch/cc.h - don't redefine it here. */

/* ---------- link callback options ---------- */
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_STATUS_CALLBACK      1

/*
   --------------------------------------
   ---------- Checksum options ----------
   --------------------------------------
   The STM32F4xx ETH MAC can compute/verify IP, UDP, TCP and ICMP checksums in
   hardware - let it, so the CPU doesn't have to.
*/
#define CHECKSUM_BY_HARDWARE

#ifdef CHECKSUM_BY_HARDWARE
  #define CHECKSUM_GEN_IP                 0
  #define CHECKSUM_GEN_UDP                0
  #define CHECKSUM_GEN_TCP                0
  #define CHECKSUM_CHECK_IP               0
  #define CHECKSUM_CHECK_UDP              0
  #define CHECKSUM_CHECK_TCP              0
  #define CHECKSUM_GEN_ICMP                0
#else
  #define CHECKSUM_GEN_IP                 1
  #define CHECKSUM_GEN_UDP                1
  #define CHECKSUM_GEN_TCP                1
  #define CHECKSUM_CHECK_IP               1
  #define CHECKSUM_CHECK_UDP              1
  #define CHECKSUM_CHECK_TCP              1
  #define CHECKSUM_GEN_ICMP                1
#endif

/* ---------- Debug options (disabled; flip LWIP_DEBUG to 1 to diagnose) ---------- */
#define LWIP_DEBUG               0

#endif /* __LWIPOPTS_H__ */

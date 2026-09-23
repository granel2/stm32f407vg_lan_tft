/**
  ******************************************************************************
  * @file    tcp_echo_client.h
  * @brief   Minimal TCP client used as the first ETH/lwIP smoke test:
  *          connects to a configurable server, sends a counter message every
  *          few seconds, and prints back over UART1 whatever the server echoes.
  ******************************************************************************
  */
#ifndef __TCP_ECHO_CLIENT_H__
#define __TCP_ECHO_CLIENT_H__

#include <stdint.h>
#include "lwip/netif.h"

/* Real test server on the LAN (was a placeholder, 10.0.1.18 - never actually
   listened on anything). Run e.g. `nc -lk 5000` on 10.0.1.16 to see this
   client's counter messages and have it echo something back. */
#define TCP_ECHO_SERVER_IP0   10
#define TCP_ECHO_SERVER_IP1   0
#define TCP_ECHO_SERVER_IP2   1
#define TCP_ECHO_SERVER_IP3   16
#define TCP_ECHO_SERVER_PORT  5000

/* Call once the netif is up and has a valid IP (see main.c main loop). */
void tcp_echo_client_init(void);

/* Call every iteration of the main loop. Handles (re)connecting and the
   periodic send; cheap to call when there's nothing to do. */
void tcp_echo_client_poll(void);

/* TEMPORARY diagnostic for the heartbeat line in main.c: one-letter state
   code, 'I' = idle (about to retry), 'C' = connecting (SYN sent, no
   response yet), 'E' = connected. See PROJECT_GUIDE.md. */
char tcp_echo_client_state_char(void);

/* TEMPORARY diagnostic: number of pbufs currently queued on the active
   pcb's send buffer (pcb->snd_queuelen, unsent+unacked combined), 0 if no
   pcb. If this climbs without bound while connected, sent data is not
   being freed on ACK - see PROJECT_GUIDE.md. */
uint16_t tcp_echo_client_sndqueuelen(void);

/* For the TFT status screen (main.c): copies whatever was most recently
   received from the server into `buf` (NUL-terminated, truncated to
   buf_size - 1), returns the byte count written, 0 if nothing new since
   the last call. Each received chunk is handed out exactly once - call
   this once per main-loop iteration and pass a non-zero result straight to
   TFT_App_ShowReceived(). Independent of the UART debug echo (both are
   fed from the same received data, neither affects the other). */
uint16_t tcp_echo_client_take_last_rx(char *buf, uint16_t buf_size);

#endif /* __TCP_ECHO_CLIENT_H__ */

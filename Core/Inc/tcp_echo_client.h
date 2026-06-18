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

/* TODO: point this at your actual test server (e.g. `nc -lk 5000` on a PC in
   the same LAN). Until you set a real address, the client will just keep
   retrying the connection every few seconds (harmless, visible over UART). */
#define TCP_ECHO_SERVER_IP0   10
#define TCP_ECHO_SERVER_IP1   0
#define TCP_ECHO_SERVER_IP2   1
#define TCP_ECHO_SERVER_IP3   18
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

#endif /* __TCP_ECHO_CLIENT_H__ */

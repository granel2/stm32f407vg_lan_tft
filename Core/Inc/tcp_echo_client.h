/**
  ******************************************************************************
  * @file    tcp_echo_client.h
  * @brief   Minimal TCP client used as the first ETH/lwIP smoke test:
  *          connects to a configurable server, sends a counter message every
  *          few seconds, and prints back over UART1 whatever the server echoes.
  *
  *          The target address/port used to be TCP_ECHO_SERVER_IP0..3/PORT,
  *          compile-time-only constants here - changing the server meant a
  *          full rebuild+reflash. Now read at connect time from
  *          Config/Inc/device_config.h's DeviceConfig.server_ip/server_port
  *          (persisted in Flash, settable at runtime over the network - see
  *          Config/Src/config_server.c), with the same values as compiled-in
  *          defaults for a first boot with blank Flash.
  ******************************************************************************
  */
#ifndef __TCP_ECHO_CLIENT_H__
#define __TCP_ECHO_CLIENT_H__

#include <stdint.h>
#include "lwip/netif.h"

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

/* Drop the current connection (if any) and reconnect using whatever server
   address device_config.h holds now - main.c calls this when a saved
   config changed server_ip/server_port. Idle: retries right away instead
   of waiting out RECONNECT_INTERVAL_MS. Mid-connect (SYN sent): left alone,
   the timeout/retry path already re-reads the address on its next attempt
   - aborting there would count as a failed connect toward the ETH-reset
   escalation for no real reason. */
void tcp_echo_client_restart(void);

#endif /* __TCP_ECHO_CLIENT_H__ */

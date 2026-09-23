/**
  ******************************************************************************
  * @file    config_server.h
  * @brief   Plain-text TCP listener (port CONFIG_SERVER_PORT) for setting
  *          device_config.h's persisted parameters over the LAN - no serial
  *          cable needed, just the same router/switch the module is
  *          already on. See Config/README.md for the protocol and example
  *          session (a bare `nc <module IP> 7000` works, same as the
  *          existing tcp_echo_client test server pattern - no special
  *          client tool needed).
  ******************************************************************************
  */
#ifndef CONFIG_SERVER_H
#define CONFIG_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Starts listening on CONFIG_SERVER_PORT. Call once, after
  *         MX_LWIP_Init() (needs the netif up first). Purely event-driven
  *         through lwIP raw-API callbacks (tcp_accept()/tcp_recv()), like
  *         tcp_echo_client's connection handling - no separate poll()
  *         function is needed in the main loop, ethernetif_input()/
  *         sys_check_timeouts() already drive it the same way they drive
  *         everything else in lwIP.
  */
void config_server_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_SERVER_H */

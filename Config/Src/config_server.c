/**
  ******************************************************************************
  * @file    config_server.c
  * @brief   See config_server.h. One line in, one reply out, lwIP raw API -
  *          modelled on tcp_echo_client.c's connection handling, just with
  *          the roles reversed (this module is the server/listener, that
  *          one the client).
  *
  *          Only one config session at a time: a second connection attempt
  *          while one is already open gets a one-line refusal and is
  *          closed immediately. Simpler and safer than juggling multiple
  *          concurrent line buffers for what is, in practice, a single
  *          technician typing commands during commissioning - not a
  *          service with concurrent users.
  ******************************************************************************
  */
#include "config_server.h"
#include "device_config.h"

#include "lwip/tcp.h"
#include "lwip/pbuf.h"

#include "main.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_SERVER_PORT  7000U
#define CONFIG_LINE_BUF     128U

static struct tcp_pcb *s_listen_pcb;
static struct tcp_pcb *s_active_pcb;      /* NULL when no session is open */
static char            s_line[CONFIG_LINE_BUF];
static uint16_t         s_line_pos;

static void send_str(struct tcp_pcb *tpcb, const char *s)
{
  /* Best-effort: TCP_WRITE_FLAG_COPY so `s` (often a local snprintf buffer)
     doesn't need to outlive the call. Replies here are short (well under
     tcp_sndbuf()'s size for a single unloaded connection), and this is a
     low-traffic admin path, so the ERR_MEM "retry later" case tcp_echo_client
     handles for its steady 2 s ping stream isn't worth replicating here -
     a dropped reply just means the technician retries the command. */
  tcp_write(tpcb, s, (u16_t)strlen(s), TCP_WRITE_FLAG_COPY);
  tcp_output(tpcb);
}

static uint8_t parse_ipv4(const char *s, uint8_t out[4])
{
  const char *p = s;
  char *end;

  for (uint8_t i = 0; i < 4U; i++)
  {
    unsigned long v = strtoul(p, &end, 10);
    if ((end == p) || (v > 255UL)) { return 0U; }
    out[i] = (uint8_t)v;
    p = end;
    if (i < 3U)
    {
      if (*p != '.') { return 0U; }
      p++;
    }
  }
  /* Whatever follows (space, NUL, ...) is the caller's to check - this
     just validates the four dotted octets themselves. */
  return 1U;
}

/**
  * @brief  Parses and applies one already-trimmed command line. Replies are
  *         sent directly on tpcb; nothing is returned. Recognised commands:
  *           SET SERVER <ip> <port>        - tcp_echo_client's target
  *           SET NAME <text>               - rest of the line, verbatim
  *           SET IP DHCP                   - takes effect after REBOOT
  *           SET IP STATIC <ip> <mask> <gw> - takes effect after REBOOT
  *           SAVE                          - persist to Flash (device_config_save())
  *           REBOOT                        - NVIC_SystemReset()
  *           GET                           - dump the current in-RAM config
  *         Anything else: one ERR line naming the problem.
  */
static void handle_command(struct tcp_pcb *tpcb, char *line)
{
  DeviceConfig *cfg = device_config_get();
  char reply[200];  /* worst case (23-char name, 255.255.255.255 everywhere,
                        port 65535): ~165 B - sized with real margin, this
                        one genuinely could have truncated, unlike the
                        SET NAME case below where truncation is intentional */

  if (strncmp(line, "SET SERVER ", 11) == 0)
  {
    char *ip_tok = strtok(line + 11, " ");
    char *port_tok = strtok(NULL, " ");
    uint8_t ip[4];
    unsigned long port;
    char *end;

    if ((ip_tok == NULL) || (port_tok == NULL) || (parse_ipv4(ip_tok, ip) == 0U))
    {
      send_str(tpcb, "ERR usage: SET SERVER <a.b.c.d> <port>\r\n");
      return;
    }
    port = strtoul(port_tok, &end, 10);
    if ((end == port_tok) || (port == 0UL) || (port > 65535UL))
    {
      send_str(tpcb, "ERR bad port (1-65535)\r\n");
      return;
    }
    memcpy(cfg->server_ip, ip, sizeof(ip));
    cfg->server_port = (uint16_t)port;
    send_str(tpcb, "OK (SAVE, then next reconnect attempt uses it - or REBOOT for immediately)\r\n");
  }
  else if (strncmp(line, "SET NAME ", 9) == 0)
  {
    /* GCC can't know at compile time that the rest of the line (up to
       CONFIG_LINE_BUF-10 = 118 bytes) fits cfg->name (24 bytes), so
       -Wformat-truncation flags this - but snprintf() truncating an
       over-long name to fit is exactly the intended, safe behaviour, not
       a bug (same pattern as Display/Src/tft_app.c's status_draw_field()). */
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(cfg->name, sizeof(cfg->name), "%s", line + 9);
    #pragma GCC diagnostic pop
    send_str(tpcb, "OK (SAVE to persist)\r\n");
  }
  else if (strcmp(line, "SET IP DHCP") == 0)
  {
    cfg->use_static_ip = 0U;
    send_str(tpcb, "OK (SAVE, then REBOOT to apply)\r\n");
  }
  else if (strncmp(line, "SET IP STATIC ", 14) == 0)
  {
    char *ip_tok   = strtok(line + 14, " ");
    char *mask_tok = strtok(NULL, " ");
    char *gw_tok   = strtok(NULL, " ");
    uint8_t ip[4], mask[4], gw[4];

    if ((ip_tok == NULL) || (mask_tok == NULL) || (gw_tok == NULL) ||
        (parse_ipv4(ip_tok, ip) == 0U) || (parse_ipv4(mask_tok, mask) == 0U) || (parse_ipv4(gw_tok, gw) == 0U))
    {
      send_str(tpcb, "ERR usage: SET IP STATIC <ip> <netmask> <gateway>\r\n");
      return;
    }
    cfg->use_static_ip = 1U;
    memcpy(cfg->static_ip,      ip,   sizeof(ip));
    memcpy(cfg->static_netmask, mask, sizeof(mask));
    memcpy(cfg->static_gw,      gw,   sizeof(gw));
    send_str(tpcb, "OK (SAVE, then REBOOT to apply)\r\n");
  }
  else if (strcmp(line, "SAVE") == 0)
  {
    send_str(tpcb, (device_config_save() != 0U) ? "OK\r\n" : "ERR flash write failed\r\n");
  }
  else if (strcmp(line, "REBOOT") == 0)
  {
    send_str(tpcb, "OK rebooting\r\n");
    tcp_output(tpcb);
    HAL_Delay(100);  /* give the TCP stack a moment to actually get the
                         reply and the ACK out over Ethernet before the
                         reset tears everything down */
    NVIC_SystemReset();
  }
  else if (strcmp(line, "GET") == 0)
  {
    snprintf(reply, sizeof(reply),
             "NAME=%s\r\nSERVER=%u.%u.%u.%u:%u\r\nIPMODE=%s\r\nSTATIC_IP=%u.%u.%u.%u\r\n"
             "STATIC_MASK=%u.%u.%u.%u\r\nSTATIC_GW=%u.%u.%u.%u\r\nOK\r\n",
             cfg->name,
             cfg->server_ip[0], cfg->server_ip[1], cfg->server_ip[2], cfg->server_ip[3], cfg->server_port,
             (cfg->use_static_ip != 0U) ? "STATIC" : "DHCP",
             cfg->static_ip[0], cfg->static_ip[1], cfg->static_ip[2], cfg->static_ip[3],
             cfg->static_netmask[0], cfg->static_netmask[1], cfg->static_netmask[2], cfg->static_netmask[3],
             cfg->static_gw[0], cfg->static_gw[1], cfg->static_gw[2], cfg->static_gw[3]);
    send_str(tpcb, reply);
  }
  else
  {
    send_str(tpcb, "ERR unknown command (SET SERVER/NAME/IP, SAVE, REBOOT, GET)\r\n");
  }
}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
  LWIP_UNUSED_ARG(arg);

  if (err != ERR_OK) { if (p != NULL) { pbuf_free(p); } return err; }

  if (p == NULL)
  {
    /* Peer closed. */
    tcp_arg(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_close(tpcb);
    if (tpcb == s_active_pcb) { s_active_pcb = NULL; }
    return ERR_OK;
  }

  for (struct pbuf *q = p; q != NULL; q = q->next)
  {
    const char *bytes = (const char *)q->payload;
    for (uint16_t i = 0; i < q->len; i++)
    {
      char b = bytes[i];

      if ((b == '\r') || (b == '\n'))
      {
        if (s_line_pos > 0U)
        {
          s_line[s_line_pos] = '\0';
          handle_command(tpcb, s_line);
          s_line_pos = 0U;
        }
      }
      else if (s_line_pos < (CONFIG_LINE_BUF - 1U))
      {
        s_line[s_line_pos++] = b;
      }
      /* Byte beyond CONFIG_LINE_BUF silently dropped - an over-long line
         is almost certainly a mistyped command anyway. */
    }
  }

  tcp_recved(tpcb, p->tot_len);
  pbuf_free(p);
  return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
  /* PCB already freed by lwIP when this fires - do NOT touch it. */
  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(err);
  s_active_pcb = NULL;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
  LWIP_UNUSED_ARG(arg);
  if (err != ERR_OK) { return err; }

  if (s_active_pcb != NULL)
  {
    /* Already busy with another session - refuse politely rather than
       interleaving two clients' bytes into one shared line buffer. */
    tcp_write(newpcb, "ERR busy, one config session at a time\r\n", 41U, TCP_WRITE_FLAG_COPY);
    tcp_output(newpcb);
    tcp_close(newpcb);
    return ERR_OK;
  }

  s_active_pcb = newpcb;
  s_line_pos   = 0U;
  tcp_arg(newpcb, NULL);
  tcp_err(newpcb, on_err);
  tcp_recv(newpcb, on_recv);
  send_str(newpcb, "READY (SET SERVER/NAME/IP, SAVE, REBOOT, GET)\r\n");
  return ERR_OK;
}

void config_server_init(void)
{
  struct tcp_pcb *pcb = tcp_new();

  if (pcb == NULL)
  {
    Debug_Print("[config] tcp_new() failed, config server not started\r\n");
    return;
  }
  if (tcp_bind(pcb, IP_ADDR_ANY, CONFIG_SERVER_PORT) != ERR_OK)
  {
    Debug_Print("[config] tcp_bind() failed, config server not started\r\n");
    tcp_close(pcb);
    return;
  }
  s_listen_pcb = tcp_listen(pcb);
  tcp_accept(s_listen_pcb, on_accept);
  Debug_Print("[config] listening on TCP port 7000\r\n");
}

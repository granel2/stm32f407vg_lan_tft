/**
  ******************************************************************************
  * @file    config_http.c
  * @brief   See config_http.h. Minimal HTTP/1.0 server on lwIP's raw API:
  *            GET /                 - settings form, pre-filled
  *            GET /save?name=...    - validate all fields, apply, persist
  *            GET /reboot           - "Rebooting..." page, then reset
  *            anything else         - 404
  *
  *          The form uses method="get", so everything the server needs is in
  *          the request line - no request body, no Content-Length handling,
  *          no per-connection state. The request line of a form submit is a
  *          few hundred bytes at most and always arrives in the first TCP
  *          segment (MSS 1460), so it's parsed straight from the first pbuf
  *          and the rest of the request (headers) is ignored. Every response
  *          is "Connection: close" - one request per connection, close right
  *          after, which is all a settings page needs.
  ******************************************************************************
  */
#include "config_http.h"
#include "device_config.h"

#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "main.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_HTTP_PORT     80U
#define HTTP_REQ_BUF         512U
#define HTTP_PAGE_BUF        3072U
#define HTTP_REBOOT_DELAY_MS 500U

static char     s_page[HTTP_PAGE_BUF];  /* shared: single-threaded (NO_SYS), and every
                                            response is tcp_write(COPY)'d immediately */
static uint16_t s_page_len;
static uint8_t  s_reboot_pending;
static uint32_t s_reboot_at;

/* ---------- small string helpers ---------------------------------------- */

static void page_reset(void)
{
  s_page_len = 0U;
  s_page[0] = '\0';
}

/* Appends to s_page; silently stops at the buffer end (a truncated page is
   still valid enough HTML to see that something went wrong, and
   HTTP_PAGE_BUF has ~1 KB of headroom over the real page size). */
static void page_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void page_add(const char *fmt, ...)
{
  va_list ap;
  int n;

  if (s_page_len >= (HTTP_PAGE_BUF - 1U)) { return; }
  va_start(ap, fmt);
  n = vsnprintf(s_page + s_page_len, HTTP_PAGE_BUF - s_page_len, fmt, ap);
  va_end(ap);
  if (n < 0) { return; }
  s_page_len = (uint16_t)(((uint32_t)s_page_len + (uint32_t)n < (HTTP_PAGE_BUF - 1U))
                          ? (s_page_len + (uint16_t)n) : (HTTP_PAGE_BUF - 1U));
}

/* '<', '>', '&', '"' -> entities, so a device name like  a"b<c  can't break
   the value="..." attribute it gets echoed into. */
static void html_escape(const char *in, char *out, size_t out_size)
{
  size_t o = 0;

  for (; (*in != '\0') && (o + 7U < out_size); in++)
  {
    const char *rep = NULL;
    switch (*in)
    {
      case '<': rep = "&lt;";   break;
      case '>': rep = "&gt;";   break;
      case '&': rep = "&amp;";  break;
      case '"': rep = "&quot;"; break;
      default:  break;
    }
    if (rep != NULL) { size_t l = strlen(rep); memcpy(out + o, rep, l); o += l; }
    else             { out[o++] = *in; }
  }
  out[o] = '\0';
}

static int hex_val(char c)
{
  if ((c >= '0') && (c <= '9')) { return c - '0'; }
  if ((c >= 'a') && (c <= 'f')) { return c - 'a' + 10; }
  if ((c >= 'A') && (c <= 'F')) { return c - 'A' + 10; }
  return -1;
}

/* application/x-www-form-urlencoded: '+' is a space, %XX is a byte. */
static void url_decode(char *s)
{
  char *w = s;

  for (; *s != '\0'; s++)
  {
    if (*s == '+') { *w++ = ' '; }
    else if ((*s == '%') && (hex_val(s[1]) >= 0) && (hex_val(s[2]) >= 0))
    {
      *w++ = (char)((hex_val(s[1]) << 4) | hex_val(s[2]));
      s += 2;
    }
    else { *w++ = *s; }
  }
  *w = '\0';
}

/* Finds key=value in a "k1=v1&k2=v2" query string, copies the url-decoded
   value into out. Returns 1 if the key was present (even if empty). */
static uint8_t get_param(const char *query, const char *key, char *out, size_t out_size)
{
  const size_t key_len = strlen(key);
  const char *p = query;

  while ((p != NULL) && (*p != '\0'))
  {
    if ((strncmp(p, key, key_len) == 0) && (p[key_len] == '='))
    {
      const char *v = p + key_len + 1;
      size_t n = 0;
      while ((v[n] != '\0') && (v[n] != '&') && (n + 1U < out_size)) { n++; }
      memcpy(out, v, n);
      out[n] = '\0';
      url_decode(out);
      return 1U;
    }
    p = strchr(p, '&');
    if (p != NULL) { p++; }
  }
  out[0] = '\0';
  return 0U;
}

/* Same validation as config_server.c's: four dotted 0-255 octets, nothing
   else. Duplicated rather than shared - ~20 lines, and keeps the two
   front ends independent of each other. */
static uint8_t parse_ipv4(const char *s, uint8_t out[4])
{
  const char *p = s;
  char *end;

  while (*p == ' ') { p++; }
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
  while (*p == ' ') { p++; }
  return (*p == '\0') ? 1U : 0U;
}

/* ---------- response plumbing -------------------------------------------- */

static err_t on_poll_close(void *arg, struct tcp_pcb *tpcb)
{
  LWIP_UNUSED_ARG(arg);
  if (tcp_close(tpcb) != ERR_OK)
  {
    tcp_abort(tpcb);
    return ERR_ABRT;
  }
  return ERR_OK;
}

static void send_and_close(struct tcp_pcb *tpcb, const char *status, const char *body, uint16_t body_len)
{
  char hdr[160];
  int  hl = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.0 %s\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: %u\r\n\r\n",
                     status, (unsigned)body_len);

  if (hl > 0)
  {
    tcp_write(tpcb, hdr, (u16_t)hl, TCP_WRITE_FLAG_COPY);
  }
  if (body_len > 0U)
  {
    tcp_write(tpcb, body, body_len, TCP_WRITE_FLAG_COPY);
  }
  tcp_output(tpcb);

  /* tcp_close() keeps already-queued data and sends it before the FIN, so
     closing straight away is fine. On the rare ERR_MEM, retry from a poll
     callback instead of leaking the pcb. */
  tcp_recv(tpcb, NULL);
  if (tcp_close(tpcb) != ERR_OK)
  {
    tcp_poll(tpcb, on_poll_close, 2U);
  }
}

/* ---------- pages -------------------------------------------------------- */

static void build_form(const char *notice, uint8_t notice_ok)
{
  const DeviceConfig *cfg = device_config_get();
  char name_esc[24 * 6];

  html_escape(cfg->name, name_esc, sizeof(name_esc));

  page_reset();
  page_add("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>%s</title><style>"
           "body{font-family:sans-serif;max-width:420px;margin:16px auto;padding:0 12px}"
           "label{display:block;margin-top:10px;font-size:14px;color:#555}"
           "input,select{width:100%%;padding:6px;font-size:16px;box-sizing:border-box}"
           "button{margin-top:16px;padding:8px 18px;font-size:16px}"
           ".ok{background:#d4f7d4;padding:8px}.err{background:#f9d0d0;padding:8px}"
           ".hint{font-size:12px;color:#888}"
           "</style></head><body><h2>%s</h2>",
           name_esc, name_esc);

  if (netif_default != NULL)
  {
    page_add("<p class=\"hint\">Текущий IP: %s (%s)</p>",
             ip4addr_ntoa(netif_ip4_addr(netif_default)),
             (cfg->use_static_ip != 0U) ? "статический" : "DHCP");
  }

  if (notice != NULL)
  {
    page_add("<p class=\"%s\">%s</p>", (notice_ok != 0U) ? "ok" : "err", notice);
  }

  page_add("<form action=\"/save\" method=\"get\">"
           "<label>Имя устройства<input name=\"name\" maxlength=\"23\" value=\"%s\"></label>"
           "<p class=\"hint\">Латиница/цифры - кириллицу экран модуля не отображает.</p>"
           "<label>IP TCP-сервера<input name=\"sip\" value=\"%u.%u.%u.%u\"></label>"
           "<label>Порт TCP-сервера<input name=\"sport\" value=\"%u\"></label>",
           name_esc,
           cfg->server_ip[0], cfg->server_ip[1], cfg->server_ip[2], cfg->server_ip[3],
           cfg->server_port);

  page_add("<label>Режим IP<select name=\"mode\">"
           "<option value=\"dhcp\"%s>DHCP (автоматически)</option>"
           "<option value=\"static\"%s>Статический</option></select></label>"
           "<label>Статический IP<input name=\"ip\" value=\"%u.%u.%u.%u\"></label>"
           "<label>Маска<input name=\"mask\" value=\"%u.%u.%u.%u\"></label>"
           "<label>Шлюз<input name=\"gw\" value=\"%u.%u.%u.%u\"></label>"
           "<p class=\"hint\">Статические поля используются только в режиме «Статический»."
           " Смена режима/адреса IP вступает в силу после перезагрузки.</p>"
           "<button type=\"submit\">Сохранить</button></form>"
           "<form action=\"/reboot\" method=\"get\"><button type=\"submit\">Перезагрузить</button></form>"
           "</body></html>",
           (cfg->use_static_ip == 0U) ? " selected" : "",
           (cfg->use_static_ip != 0U) ? " selected" : "",
           cfg->static_ip[0], cfg->static_ip[1], cfg->static_ip[2], cfg->static_ip[3],
           cfg->static_netmask[0], cfg->static_netmask[1], cfg->static_netmask[2], cfg->static_netmask[3],
           cfg->static_gw[0], cfg->static_gw[1], cfg->static_gw[2], cfg->static_gw[3]);
}

/* Validates *every* field before touching the live config - either the whole
   submit is applied and saved, or nothing is (and the form comes back with
   the reason, still showing the old values). */
static void handle_save(struct tcp_pcb *tpcb, const char *query)
{
  DeviceConfig *cfg = device_config_get();
  DeviceConfig  tmp = *cfg;
  char          val[64];
  const char   *error = NULL;
  uint8_t       reboot_needed;

  if (get_param(query, "name", val, sizeof(val)) != 0U)
  {
    if (val[0] == '\0') { error = "Имя не может быть пустым."; }
    else
    {
      #pragma GCC diagnostic push
      #pragma GCC diagnostic ignored "-Wformat-truncation"
      snprintf(tmp.name, sizeof(tmp.name), "%s", val);  /* >23 chars: truncated, by design */
      #pragma GCC diagnostic pop
    }
  }

  if ((error == NULL) && (get_param(query, "sip", val, sizeof(val)) != 0U) &&
      (parse_ipv4(val, tmp.server_ip) == 0U))
  {
    error = "Неверный IP TCP-сервера (нужно вида 10.0.1.16).";
  }

  if ((error == NULL) && (get_param(query, "sport", val, sizeof(val)) != 0U))
  {
    char *end;
    unsigned long port = strtoul(val, &end, 10);
    if ((end == val) || (*end != '\0') || (port == 0UL) || (port > 65535UL))
    {
      error = "Неверный порт TCP-сервера (1-65535).";
    }
    else
    {
      tmp.server_port = (uint16_t)port;
    }
  }

  if ((error == NULL) && (get_param(query, "mode", val, sizeof(val)) != 0U))
  {
    tmp.use_static_ip = (strcmp(val, "static") == 0) ? 1U : 0U;
  }

  if (error == NULL)
  {
    /* Static fields: required (and validated) in static mode; in DHCP mode
       they're kept if valid and silently left as-is if not, so switching
       modes back and forth doesn't lose what was typed. */
    uint8_t ip[4], mask[4], gw[4];
    uint8_t ip_ok   = (get_param(query, "ip",   val, sizeof(val)) != 0U) && (parse_ipv4(val, ip)   != 0U);
    uint8_t mask_ok = (get_param(query, "mask", val, sizeof(val)) != 0U) && (parse_ipv4(val, mask) != 0U);
    uint8_t gw_ok   = (get_param(query, "gw",   val, sizeof(val)) != 0U) && (parse_ipv4(val, gw)   != 0U);

    if (tmp.use_static_ip != 0U)
    {
      if (!ip_ok || ((ip[0] | ip[1] | ip[2] | ip[3]) == 0U)) { error = "Неверный статический IP."; }
      else if (!mask_ok || ((mask[0] | mask[1] | mask[2] | mask[3]) == 0U)) { error = "Неверная маска (например 255.255.255.0)."; }
      else if (!gw_ok)   { error = "Неверный шлюз."; }
    }
    if (error == NULL)
    {
      if (ip_ok)   { memcpy(tmp.static_ip,      ip,   4U); }
      if (mask_ok) { memcpy(tmp.static_netmask, mask, 4U); }
      if (gw_ok)   { memcpy(tmp.static_gw,      gw,   4U); }
    }
  }

  if (error != NULL)
  {
    build_form(error, 0U);
    send_and_close(tpcb, "200 OK", s_page, s_page_len);
    return;
  }

  /* IP-mode/address changes only take effect at boot (MX_LWIP_Init()); the
     server address and name apply straight away (main.c reacts to
     device_config_revision()). */
  reboot_needed = (uint8_t)((tmp.use_static_ip != cfg->use_static_ip) ||
                            ((tmp.use_static_ip != 0U) &&
                             ((memcmp(tmp.static_ip, cfg->static_ip, 4U) != 0) ||
                              (memcmp(tmp.static_netmask, cfg->static_netmask, 4U) != 0) ||
                              (memcmp(tmp.static_gw, cfg->static_gw, 4U) != 0))));

  *cfg = tmp;
  if (device_config_save() == 0U)
  {
    build_form("Ошибка записи во Flash - настройки не сохранены.", 0U);
  }
  else
  {
    Debug_Print("[http] settings saved from web page\r\n");
    build_form((reboot_needed != 0U)
               ? "Сохранено. Настройки IP вступят в силу после перезагрузки - нажмите «Перезагрузить»."
               : "Сохранено и применено.", 1U);
  }
  send_and_close(tpcb, "200 OK", s_page, s_page_len);
}

static void handle_reboot(struct tcp_pcb *tpcb)
{
  page_reset();
  page_add("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta http-equiv=\"refresh\" content=\"12;url=/\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"></head>"
           "<body style=\"font-family:sans-serif;max-width:420px;margin:16px auto\">"
           "<h3>Перезагрузка...</h3><p>Страница обновится сама через ~12 с (загрузка модуля"
           " занимает около 10 с). Если IP-адрес менялся - откройте новый адрес вручную.</p>"
           "</body></html>");
  send_and_close(tpcb, "200 OK", s_page, s_page_len);
  Debug_Print("[http] reboot requested from web page\r\n");
  s_reboot_at = HAL_GetTick() + HTTP_REBOOT_DELAY_MS;
  s_reboot_pending = 1U;
}

/* ---------- lwIP callbacks ------------------------------------------------ */

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
  char req[HTTP_REQ_BUF];
  char *path;
  char *query;
  char *sp;
  u16_t n;

  LWIP_UNUSED_ARG(arg);

  if (err != ERR_OK) { if (p != NULL) { pbuf_free(p); } return err; }
  if (p == NULL)
  {
    tcp_recv(tpcb, NULL);
    if (tcp_close(tpcb) != ERR_OK) { tcp_poll(tpcb, on_poll_close, 2U); }
    return ERR_OK;
  }

  n = pbuf_copy_partial(p, req, (u16_t)(sizeof(req) - 1U), 0U);
  req[n] = '\0';
  tcp_recved(tpcb, p->tot_len);
  pbuf_free(p);

  /* Request line only: "GET /path?query HTTP/1.1" */
  sp = strpbrk(req, "\r\n");
  if (sp != NULL) { *sp = '\0'; }

  if (strncmp(req, "GET ", 4) != 0)
  {
    static const char msg[] = "<h3>405 - only GET</h3>";
    send_and_close(tpcb, "405 Method Not Allowed", msg, (uint16_t)(sizeof(msg) - 1U));
    return ERR_OK;
  }

  path = req + 4;
  sp = strchr(path, ' ');
  if (sp != NULL) { *sp = '\0'; }
  query = strchr(path, '?');
  if (query != NULL) { *query++ = '\0'; } else { query = ""; }

  if (strcmp(path, "/") == 0)
  {
    build_form(NULL, 0U);
    send_and_close(tpcb, "200 OK", s_page, s_page_len);
  }
  else if (strcmp(path, "/save") == 0)
  {
    handle_save(tpcb, query);
  }
  else if (strcmp(path, "/reboot") == 0)
  {
    handle_reboot(tpcb);
  }
  else
  {
    /* Includes the browser's automatic /favicon.ico request. */
    static const char msg[] = "<h3>404</h3>";
    send_and_close(tpcb, "404 Not Found", msg, (uint16_t)(sizeof(msg) - 1U));
  }
  return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
  LWIP_UNUSED_ARG(arg);
  if ((err != ERR_OK) || (newpcb == NULL)) { return ERR_VAL; }
  tcp_recv(newpcb, on_recv);
  return ERR_OK;
}

void config_http_init(void)
{
  struct tcp_pcb *pcb = tcp_new();

  if (pcb == NULL)
  {
    Debug_Print("[http] tcp_new() failed, web config not started\r\n");
    return;
  }
  if (tcp_bind(pcb, IP_ADDR_ANY, CONFIG_HTTP_PORT) != ERR_OK)
  {
    Debug_Print("[http] tcp_bind() failed, web config not started\r\n");
    tcp_close(pcb);
    return;
  }
  pcb = tcp_listen(pcb);
  if (pcb == NULL)
  {
    Debug_Print("[http] tcp_listen() failed (MEMP_NUM_TCP_PCB_LISTEN?), web config not started\r\n");
    return;
  }
  tcp_accept(pcb, on_accept);
  Debug_Print("[http] web config on TCP port 80\r\n");
}

void config_http_poll(void)
{
  if ((s_reboot_pending != 0U) && ((int32_t)(HAL_GetTick() - s_reboot_at) >= 0))
  {
    NVIC_SystemReset();
  }
}

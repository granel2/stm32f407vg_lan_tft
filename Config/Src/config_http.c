/**
  ******************************************************************************
  * @file    config_http.c
  * @brief   See config_http.h. Minimal HTTP/1.0 server on lwIP's raw API:
  *            GET /                 - settings table: parameter | example |
  *                                    current value | new value (input)
  *            GET /apply?act=save   - validate the non-empty inputs, write
  *                                    to Flash; active after next restart
  *            GET /apply?act=cancel - nothing written, table re-shown
  *            GET /apply?act=restart- as save, then reboot the module
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
#include "lwip/dhcp.h"

#include "main.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_HTTP_PORT     80U
#define HTTP_REQ_BUF         512U
#define HTTP_PAGE_BUF        6144U
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
   HTTP_PAGE_BUF has ~2 KB of headroom over the real page size). */
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

/* ---------- settings table ------------------------------------------------ */

/* One row of the table. Drives rendering (label/example/current value) and
   parsing (key = the input's name= in the query string) from one place, so
   adding a setting means one line here plus a case in field_format() and
   field_parse(). */
typedef enum
{
  FIELD_NAME = 0,
  FIELD_SERVER_IP,
  FIELD_SERVER_PORT,
  FIELD_IP_MODE,
  FIELD_STATIC_IP,
  FIELD_NETMASK,
  FIELD_GATEWAY,
  FIELD_BL_LEVEL,      /* backlight fields: applied at once on Save, no restart */
  FIELD_BL_DIM_LEVEL,
  FIELD_BL_DIM_MIN,
  FIELD_COUNT
} FieldId;

#define FIELD_IS_LIVE(f)  ((f) >= FIELD_BL_LEVEL)

typedef struct
{
  const char *key;
  const char *label;
  const char *example;
} FieldDesc;

static const FieldDesc k_fields[FIELD_COUNT] =
{
  [FIELD_NAME]        = { "name",  "Имя устройства",                "room-3" },
  [FIELD_SERVER_IP]   = { "sip",   "IP TCP-сервера",                "10.0.1.16" },
  [FIELD_SERVER_PORT] = { "sport", "Порт TCP-сервера",              "5000" },
  [FIELD_IP_MODE]     = { "mode",  "Режим IP",                      "DHCP / STATIC" },
  [FIELD_STATIC_IP]   = { "ip",    "Статический IP / IP по умолчанию", "192.168.1.100" },
  [FIELD_NETMASK]     = { "mask",  "Маска подсети",                 "255.255.255.0" },
  [FIELD_GATEWAY]     = { "gw",    "Шлюз",                          "192.168.1.1" },
  [FIELD_BL_LEVEL]     = { "bl",    "Яркость экрана, %",             "100" },
  [FIELD_BL_DIM_LEVEL] = { "bldim", "Яркость в покое, %",            "20" },
  [FIELD_BL_DIM_MIN]   = { "blmin", "Приглушать через, мин (0 = нет)", "5" },
};

/* Whole number in [lo, hi], nothing else. */
static uint8_t parse_uint(const char *s, unsigned long lo, unsigned long hi, unsigned long *out)
{
  char *end;
  unsigned long v = strtoul(s, &end, 10);

  if ((end == s) || (*end != '\0') || (v < lo) || (v > hi)) { return 0U; }
  *out = v;
  return 1U;
}

/* Value of one field as shown in the "current" column (not HTML-escaped). */
static void field_format(const DeviceConfig *cfg, FieldId f, char *out, size_t out_size)
{
  const uint8_t *ip = NULL;

  switch (f)
  {
    case FIELD_NAME:        snprintf(out, out_size, "%s", cfg->name); return;
    case FIELD_SERVER_PORT: snprintf(out, out_size, "%u", cfg->server_port); return;
    case FIELD_IP_MODE:     snprintf(out, out_size, "%s", (cfg->use_static_ip != 0U) ? "STATIC" : "DHCP"); return;
    case FIELD_BL_LEVEL:     snprintf(out, out_size, "%u", cfg->bl_level); return;
    case FIELD_BL_DIM_LEVEL: snprintf(out, out_size, "%u", cfg->bl_dim_level); return;
    case FIELD_BL_DIM_MIN:   snprintf(out, out_size, "%u", cfg->bl_dim_min); return;
    case FIELD_SERVER_IP:   ip = cfg->server_ip;      break;
    case FIELD_STATIC_IP:   ip = cfg->static_ip;      break;
    case FIELD_NETMASK:     ip = cfg->static_netmask; break;
    case FIELD_GATEWAY:     ip = cfg->static_gw;      break;
    default:                out[0] = '\0'; return;
  }
  snprintf(out, out_size, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

/* Parses one non-empty input into cfg. Returns NULL or the error text. */
static const char *field_parse(DeviceConfig *cfg, FieldId f, const char *val)
{
  switch (f)
  {
    case FIELD_NAME:
      if (strlen(val) >= sizeof(cfg->name)) { return "Имя устройства: не длиннее 23 символов (латиница/цифры)."; }
      snprintf(cfg->name, sizeof(cfg->name), "%s", val);
      return NULL;

    case FIELD_SERVER_PORT:
    {
      char *end;
      unsigned long port = strtoul(val, &end, 10);
      if ((end == val) || (*end != '\0') || (port == 0UL) || (port > 65535UL))
      {
        return "Порт TCP-сервера: число 1-65535.";
      }
      cfg->server_port = (uint16_t)port;
      return NULL;
    }

    case FIELD_IP_MODE:
      if      (strcmp(val, "dhcp")   == 0) { cfg->use_static_ip = 0U; }
      else if (strcmp(val, "static") == 0) { cfg->use_static_ip = 1U; }
      else { return "Режим IP: DHCP или STATIC."; }
      return NULL;

    case FIELD_SERVER_IP: return (parse_ipv4(val, cfg->server_ip)      != 0U) ? NULL : "IP TCP-сервера: нужен вид 10.0.1.16.";
    case FIELD_STATIC_IP: return (parse_ipv4(val, cfg->static_ip)      != 0U) ? NULL : "Статический IP: нужен вид 192.168.1.100.";
    case FIELD_NETMASK:   return (parse_ipv4(val, cfg->static_netmask) != 0U) ? NULL : "Маска: нужен вид 255.255.255.0.";
    case FIELD_GATEWAY:   return (parse_ipv4(val, cfg->static_gw)      != 0U) ? NULL : "Шлюз: нужен вид 192.168.1.1.";

    case FIELD_BL_LEVEL:
    case FIELD_BL_DIM_LEVEL:
    {
      unsigned long v;
      if (!parse_uint(val, 1UL, 100UL, &v))
      {
        return (f == FIELD_BL_LEVEL) ? "Яркость экрана: число 1-100."
                                     : "Яркость в покое: число 1-100 (0 нельзя - экран должен оставаться видимым).";
      }
      if (f == FIELD_BL_LEVEL) { cfg->bl_level = (uint8_t)v; } else { cfg->bl_dim_level = (uint8_t)v; }
      return NULL;
    }

    case FIELD_BL_DIM_MIN:
    {
      unsigned long v;
      if (!parse_uint(val, 0UL, 1440UL, &v)) { return "Приглушать через: число минут 0-1440 (0 = не приглушать)."; }
      cfg->bl_dim_min = (uint16_t)v;
      return NULL;
    }

    default:              return NULL;
  }
}

/* What the next boot will run with: the Flash copy (it may already hold
   changes saved earlier and still waiting for a restart), or the running
   config if Flash has never been written. New inputs are applied on top
   of this, so a second Save doesn't lose the first one's changes. */
static void next_boot_config(DeviceConfig *out)
{
  const DeviceConfig *stored = device_config_stored();
  *out = (stored != NULL) ? *stored : *device_config_get();
}

/* Applies every non-empty input over `cfg`. All-or-nothing from the
   caller's point of view: on error the caller just discards `cfg`.
   *changed = number of fields whose value actually differs afterwards. */
static const char *apply_inputs(const char *query, DeviceConfig *cfg, uint8_t *changed)
{
  const DeviceConfig before = *cfg;
  char val[64];

  *changed = 0U;
  for (uint8_t f = 0U; f < (uint8_t)FIELD_COUNT; f++)
  {
    char *v = val;
    char *e;
    const char *err;

    (void)get_param(query, k_fields[f].key, val, sizeof(val));
    while (*v == ' ') { v++; }                       /* trim - pasted values often */
    e = v + strlen(v);                                /* carry stray spaces         */
    while ((e > v) && (e[-1] == ' ')) { *--e = '\0'; }
    if (*v == '\0') { continue; }                     /* empty = keep current value */

    err = field_parse(cfg, (FieldId)f, v);
    if (err != NULL) { return err; }
  }

  /* Whole-config check: static mode needs a usable address. (In DHCP mode
     static_* is only the fallback address - still worth rejecting 0.0.0.0.) */
  if (((cfg->static_ip[0] | cfg->static_ip[1] | cfg->static_ip[2] | cfg->static_ip[3]) == 0U) &&
      (cfg->use_static_ip != 0U))
  {
    return "Режим STATIC: задайте статический IP.";
  }
  if (((cfg->static_netmask[0] | cfg->static_netmask[1] | cfg->static_netmask[2] | cfg->static_netmask[3]) == 0U) &&
      (cfg->use_static_ip != 0U))
  {
    return "Режим STATIC: задайте маску подсети.";
  }

  for (uint8_t f = 0U; f < (uint8_t)FIELD_COUNT; f++)
  {
    char a[32], b[32];
    field_format(&before, (FieldId)f, a, sizeof(a));
    field_format(cfg,     (FieldId)f, b, sizeof(b));
    if (strcmp(a, b) != 0) { (*changed)++; }
  }
  return NULL;
}

/* ---------- pages -------------------------------------------------------- */

static const char k_css[] =
  "body{font-family:sans-serif;max-width:900px;margin:16px auto;padding:0 12px;color:#222}"
  "h2{margin:0 0 4px}.info{color:#555;font-size:14px;margin:0 0 12px}"
  ".wrap{overflow-x:auto}table{border-collapse:collapse;width:100%}"
  "th,td{border:1px solid #ccc;padding:6px 8px;text-align:left;vertical-align:middle;font-size:14px}"
  "th{background:#eee}tr:nth-child(even) td{background:#fafafa}"
  "td.ex{color:#888;font-family:monospace}td.cur{font-family:monospace;font-weight:bold}"
  ".pend{color:#b36b00;font-weight:normal;font-size:12px}"
  "input,select{width:100%;min-width:130px;padding:5px;font-size:15px;box-sizing:border-box}"
  ".btns{margin-top:16px;display:flex;gap:12px;flex-wrap:wrap}"
  "button{padding:9px 26px;font-size:16px;cursor:pointer}"
  ".ok{background:#d4f7d4;padding:8px}.err{background:#f9d0d0;padding:8px}"
  ".hint{font-size:12px;color:#888}";

static void page_head(const char *title_esc)
{
  page_reset();
  page_add("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>%s</title><style>%s</style></head><body>", title_esc, k_css);
}

static void build_table(const char *notice, uint8_t notice_ok)
{
  const DeviceConfig *run = device_config_get();
  DeviceConfig        next;
  uint8_t             pending = 0U;
  char                name_esc[24 * 6];

  next_boot_config(&next);
  html_escape(run->name, name_esc, sizeof(name_esc));

  page_head(name_esc);
  page_add("<h2>Настройки модуля «%s»</h2>", name_esc);

  if (netif_default != NULL)
  {
    const char *src = (run->use_static_ip != 0U)             ? "статический" :
                      dhcp_supplied_address(netif_default)   ? "получен по DHCP" :
                      "IP по умолчанию - DHCP-сервер не ответил";
    page_add("<p class=\"info\">IP модуля сейчас: <b>%s</b> (%s)</p>",
             ip4addr_ntoa(netif_ip4_addr(netif_default)), src);
  }

  if (notice != NULL)
  {
    page_add("<p class=\"%s\">%s</p>", (notice_ok != 0U) ? "ok" : "err", notice);
  }

  page_add("<form action=\"/apply\" method=\"get\"><div class=\"wrap\"><table>"
           "<tr><th>Параметр</th><th>Образец записи</th><th>Текущее значение</th><th>Новое значение</th></tr>");

  for (uint8_t f = 0U; f < (uint8_t)FIELD_COUNT; f++)
  {
    char cur[32], nxt[32];
    char cur_esc[32 * 6];

    field_format(run,   (FieldId)f, cur, sizeof(cur));
    field_format(&next, (FieldId)f, nxt, sizeof(nxt));
    html_escape(cur, cur_esc, sizeof(cur_esc));

    page_add("<tr><td>%s</td><td class=\"ex\">%s</td><td class=\"cur\">%s",
             k_fields[f].label, k_fields[f].example, cur_esc);
    if (strcmp(cur, nxt) != 0)
    {
      char nxt_esc[32 * 6];
      html_escape(nxt, nxt_esc, sizeof(nxt_esc));
      page_add("<br><span class=\"pend\">после рестарта: %s</span>", nxt_esc);
      pending = 1U;
    }
    page_add("</td><td>");

    if (f == (uint8_t)FIELD_IP_MODE)
    {
      page_add("<select name=\"mode\"><option value=\"\">- без изменений -</option>"
               "<option value=\"dhcp\">DHCP</option><option value=\"static\">STATIC</option></select>");
    }
    else
    {
      page_add("<input name=\"%s\"%s autocomplete=\"off\">", k_fields[f].key,
               (f == (uint8_t)FIELD_NAME) ? " maxlength=\"23\"" :
               ((f == (uint8_t)FIELD_SERVER_PORT) || FIELD_IS_LIVE(f)) ? " inputmode=\"numeric\"" : "");
    }
    page_add("</td></tr>");
  }

  page_add("</table></div>"
           "<p class=\"hint\">Пустое поле - значение не меняется. "
           "Статический IP в режиме DHCP используется как IP по умолчанию, "
           "если DHCP-сервер не ответил за 10 с. Имя - латиница/цифры (экран кириллицу не показывает). "
           "Яркость применяется сразу по Save, без рестарта; «в покое» - после указанных минут "
           "без нажатия кнопки страниц, нажатие возвращает обычную яркость.</p>");
  if (pending != 0U)
  {
    page_add("<p class=\"pend\">Есть сохранённые изменения - они вступят в силу после рестарта.</p>");
  }
  page_add("<div class=\"btns\">"
           "<button type=\"submit\" name=\"act\" value=\"save\">Save</button>"
           "<button type=\"submit\" name=\"act\" value=\"cancel\">Cancel</button>"
           "<button type=\"submit\" name=\"act\" value=\"restart\""
           " onclick=\"return confirm('Записать новые значения и перезапустить модуль?')\">Restart</button>"
           "</div>"
           "<p class=\"hint\">Save - записать в модуль, применить после следующего рестарта (яркость - сразу). "
           "Cancel - выйти без изменений. Restart - записать и сразу перезапустить.</p>"
           "</form>");
  /* After /apply?... put "/" back in the address bar, so F5 just reloads
     the table instead of re-submitting Save/Restart. */
  if (notice != NULL)
  {
    page_add("<script>history.replaceState(null,'','/')</script>");
  }
  page_add("</body></html>");
}

static void send_restart_page(struct tcp_pcb *tpcb, const DeviceConfig *next)
{
  char url[32] = "/";

  /* Static mode after the restart: the address is known, send the browser
     there. DHCP: most routers hand the same lease back, so stay on "/". */
  if (next->use_static_ip != 0U)
  {
    snprintf(url, sizeof(url), "http://%u.%u.%u.%u/",
             next->static_ip[0], next->static_ip[1], next->static_ip[2], next->static_ip[3]);
  }

  page_head("Restart");
  page_add("<meta http-equiv=\"refresh\" content=\"12;url=%s\">"
           "<h3>Перезапуск модуля...</h3>"
           "<p>Страница откроется сама через ~12 с: <a href=\"%s\">%s</a>.</p>"
           "<p class=\"hint\">Если адрес не открылся - посмотрите IP на экране модуля "
           "(строка IP:). Без DHCP-сервера модуль через 10 с берёт IP по умолчанию.</p>"
           "</body></html>", url, url, url);
  send_and_close(tpcb, "200 OK", s_page, s_page_len);
  Debug_Print("[http] restart requested from web page\r\n");
  s_reboot_at = HAL_GetTick() + HTTP_REBOOT_DELAY_MS;
  s_reboot_pending = 1U;
}

/* /apply?act=save|cancel|restart&name=...&sip=...  (one form, three buttons)
     save    - validate, write to Flash, running config untouched: takes
               effect at the next restart
     cancel  - nothing written, table re-shown
     restart - as save, then reboot (with no new values: just reboot) */
static void handle_apply(struct tcp_pcb *tpcb, const char *query)
{
  DeviceConfig next;
  char         act[16];
  const char  *error;
  uint8_t      changed = 0U;

  (void)get_param(query, "act", act, sizeof(act));

  if (strcmp(act, "cancel") == 0)
  {
    build_table("Отменено - ничего не изменено.", 1U);
    send_and_close(tpcb, "200 OK", s_page, s_page_len);
    return;
  }

  next_boot_config(&next);
  error = apply_inputs(query, &next, &changed);
  if (error != NULL)
  {
    build_table(error, 0U);   /* nothing written, nothing restarted */
    send_and_close(tpcb, "200 OK", s_page, s_page_len);
    return;
  }

  if ((changed != 0U) && (device_config_store(&next) == 0U))
  {
    build_table("Ошибка записи во Flash - настройки не сохранены.", 0U);
    send_and_close(tpcb, "200 OK", s_page, s_page_len);
    return;
  }
  if (changed != 0U)
  {
    /* Backlight settings need no restart: copy them into the running config
       as well - the TFT picks them up on its next poll. */
    DeviceConfig *run = device_config_get();
    run->bl_level     = next.bl_level;
    run->bl_dim_level = next.bl_dim_level;
    run->bl_dim_min   = next.bl_dim_min;
    Debug_Print("[http] settings saved from web page\r\n");
  }

  if (strcmp(act, "restart") == 0)
  {
    send_restart_page(tpcb, &next);
    return;
  }

  build_table((changed != 0U)
              ? "Сохранено в модуле. Новые значения начнут действовать после рестарта (кнопка Restart)."
              : "Новых значений не введено - ничего не записано.", 1U);
  send_and_close(tpcb, "200 OK", s_page, s_page_len);
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
    build_table(NULL, 0U);
    send_and_close(tpcb, "200 OK", s_page, s_page_len);
  }
  else if (strcmp(path, "/apply") == 0)
  {
    handle_apply(tpcb, query);
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
  device_config_touch();
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

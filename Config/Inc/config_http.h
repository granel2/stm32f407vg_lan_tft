/**
  ******************************************************************************
  * @file    config_http.h
  * @brief   Browser-based configuration page (HTTP, port 80): open
  *          http://<module IP>/ (IP is on the module's screen) on any PC/
  *          phone on the same network. A table - parameter, example,
  *          current value, new value - and three buttons: Save (write to
  *          Flash, active after the next restart), Cancel (leave without
  *          changes), Restart (write, then reboot). Same settings as
  *          config_server.c's plain-text port 7000 (both use
  *          device_config.h's DeviceConfig).
  ******************************************************************************
  */
#ifndef CONFIG_HTTP_H
#define CONFIG_HTTP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Starts listening on port 80. Call once, after MX_LWIP_Init().
  */
void config_http_init(void);

/**
  * @brief  Call every main-loop iteration. Only job: carry out a reboot
  *         requested from the page a short while *after* the "Restarting..."
  *         reply has actually gone out - resetting from inside the lwIP
  *         callback would kill the reply before the browser ever saw it.
  */
void config_http_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_HTTP_H */

/**
  ******************************************************************************
  * @file    config_http.h
  * @brief   Browser-based configuration page (HTTP, port 80): open
  *          http://<module IP>/ on any PC/phone on the same network, edit
  *          the fields, press Save. Same settings as config_server.c's
  *          plain-text port 7000 (both write device_config.h's DeviceConfig)
  *          - this is just the no-tools-needed front end for it.
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
  *         requested from the page a short while *after* the "Rebooting..."
  *         reply has actually gone out - resetting from inside the lwIP
  *         callback would kill the reply before the browser ever saw it.
  */
void config_http_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_HTTP_H */

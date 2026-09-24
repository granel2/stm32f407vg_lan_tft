/**
  ******************************************************************************
  * @file    panel.h
  * @brief   Instrument pages for the TFT: buttons/lamps, digital readouts,
  *          analog (needle) gauges, sliders and bar meters, laid out from
  *          static widget tables (Panel/Src/panel_pages.c).
  *
  *          Two pages:
  *            PANEL_PAGE_REMOTE - values sent by the remote server
  *            PANEL_PAGE_LOCAL  - the module's own inputs and controls
  *
  *          Every widget shows one *channel* (0..PANEL_CHANNELS-1). A channel
  *          is a fixed-point number in tenths (237 = 23.7). Whoever owns the
  *          data calls Panel_SetChannel(); the widget redraws itself on the
  *          next Panel_Poll(). Until a channel has been set from outside, the
  *          built-in demo (PANEL_DEMO) animates it so the pages show
  *          something - the first real Panel_SetChannel() on a channel stops
  *          the demo for that channel only.
  *
  *          Drawing: Panel_DrawPage() draws the whole page once (static
  *          parts + current values); after that Panel_Poll() redraws only
  *          widgets whose value changed, and at most ONE widget per call, so
  *          a busy page never blocks the main loop (Ethernet) for more than a
  *          few milliseconds at a time.
  *
  *          Depends on Display/ (st7796s.h) for pixels only; the page
  *          switching, title bar and heartbeat stay in Display/Src/tft_app.c.
  ******************************************************************************
  */
#ifndef PANEL_H
#define PANEL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define PANEL_CHANNELS  32U

/* 1 = animate every channel nobody has set yet (example pictures). */
#ifndef PANEL_DEMO
#define PANEL_DEMO      1
#endif

/* Top of the area the panel may draw in (below tft_app's title row). The
   40x40 heartbeat square at (20, 434) stays free - see panel_pages.c. */
#define PANEL_TOP_Y     50

typedef enum
{
  PANEL_PAGE_REMOTE = 0,
  PANEL_PAGE_LOCAL,
  PANEL_PAGE_COUNT
} PanelPage;

/* Draws the whole page (the caller has already cleared the screen and drawn
   its title). Blocking, ~0.2-0.4 s - only on a page switch. */
void    Panel_DrawPage(PanelPage page);

/* Call every main-loop iteration while `page` is on screen. */
void    Panel_Poll(PanelPage page);

/* value in tenths: 237 = 23.7; switches that channel from demo to real. */
void    Panel_SetChannel(uint8_t channel, int32_t value);
int32_t Panel_GetChannel(uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif /* PANEL_H */

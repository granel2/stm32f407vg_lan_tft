/**
  ******************************************************************************
  * @file    panel_internal.h
  * @brief   Widget description shared by panel.c (drawing engine) and
  *          panel_pages.c (page layouts + demo data). Not a public API.
  ******************************************************************************
  */
#ifndef PANEL_INTERNAL_H
#define PANEL_INTERNAL_H

#include <stdint.h>
#include "panel.h"

typedef enum
{
  W_GAUGE = 0,  /* analog: needle on a 240-degree scale with colour zones  */
  W_DIGITAL,    /* digital readout: big number + label + unit              */
  W_SLIDER,     /* horizontal slider: track + knob + value                 */
  W_VBAR,       /* vertical bar meter                                       */
  W_LAMP,       /* indicator lamp: LED circle + label, on when value != 0  */
  W_BUTTON      /* push/toggle button: filled when value != 0              */
} WidgetType;

/* All values (min/max/warn/alarm) are tenths, like the channels. */
typedef struct
{
  WidgetType  type;
  int16_t     x, y, w, h;
  const char *label;
  const char *unit;
  int32_t     min, max;
  int32_t     warn, alarm;  /* >= warn: yellow, >= alarm: red; set above max to disable */
  uint8_t     decimals;     /* digits shown after the point: 0 or 1 */
  uint8_t     channel;
  uint16_t    color;        /* bar/slider fill, lamp/button "on" colour */
} Widget;

typedef struct
{
  const Widget *widgets;
  uint8_t       count;
  const char   *footer;     /* small text at the bottom right, may be NULL */
} PageDef;

/* panel_pages.c */
extern const PageDef k_panel_pages[PANEL_PAGE_COUNT];
void panel_demo_update(int32_t *channels, uint32_t external_mask);     /* PANEL_DEMO only */
void panel_sources_update(int32_t *channels, uint32_t external_mask);  /* real local data, always */

#endif /* PANEL_INTERNAL_H */

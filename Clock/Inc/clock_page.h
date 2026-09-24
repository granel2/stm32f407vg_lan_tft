/**
  ******************************************************************************
  * @file    clock_page.h
  * @brief   TFT "CLOCK" page: HH:MM:SS in large 7-segment digits (24 h),
  *          date DD.MM.YYYY, day of the week, time zone and sync status.
  *
  *          Same contract as Panel/: Clock_PageDraw() paints the whole page
  *          once after tft_app.c has drawn the title bar; Clock_PagePoll()
  *          (every main-loop pass while the page is showing) redraws only
  *          what changed, one item per call, so the loop is never held for
  *          more than a few milliseconds.
  ******************************************************************************
  */
#ifndef CLOCK_PAGE_H
#define CLOCK_PAGE_H

#ifdef __cplusplus
extern "C" {
#endif

void Clock_PageDraw(void);
void Clock_PagePoll(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOCK_PAGE_H */

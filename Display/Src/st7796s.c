/**
  ******************************************************************************
  * @file    st7796s.c
  * @brief   Minimal ST7796S driver over 4-wire SPI.
  *
  * Reset + init sequence, rotation, rectangle fills, test pattern, 7-segment
  * digits and a 5x7 font. Every pixel goes out through ST7796S_FillRect().
  *
  * Commands and small fills are blocking HAL transfers. Fills of at least
  * DMA_MIN_PIXELS go out by DMA (DMA1 Stream5, see HAL_SPI_MspInit()): the
  * SPI is switched to 16-bit frames and the DMA re-sends one colour word
  * with memory increment off, so no pixel buffer is needed. FillRect()
  * returns as soon as the transfer is started; the next driver call waits
  * for it (dma_wait()), so callers never see the difference - except that
  * the CPU is free in between. Use ST7796S_WaitIdle() when the frame has to
  * be on the glass before going on (timing, reset).
  *
  * Text goes the same way: DrawString()/DrawChar() set the RAM window, then
  * the string's pixel lines are streamed by DMA from two line buffers - the
  * SPI-complete interrupt starts the next line and renders the one after it
  * into the other buffer while that line is on the wire. The call returns
  * after ~20 us instead of the ~2-4 ms a line of text takes on the bus;
  * ST7796S_Busy() lets a caller skip a pass instead of waiting.
  ******************************************************************************
  */

#include "st7796s.h"
#include <string.h>

/* ST7796S command set (subset) */
#define CMD_SWRESET   0x01U
#define CMD_RDID4     0xD3U
#define CMD_SLPOUT    0x11U
#define CMD_INVOFF    0x20U
#define CMD_INVON     0x21U
#define CMD_DISPON    0x29U
#define CMD_CASET     0x2AU
#define CMD_RASET     0x2BU
#define CMD_RAMWR     0x2CU
#define CMD_MADCTL    0x36U
#define CMD_COLMOD    0x3AU
#define CMD_DIC       0xB4U  /* display inversion control */
#define CMD_DFC       0xB6U  /* display function control */
#define CMD_PWR2      0xC1U
#define CMD_PWR3      0xC2U
#define CMD_VCMPCTL   0xC5U
#define CMD_DOCA      0xE8U  /* display output ctrl adjust */
#define CMD_PGC       0xE0U  /* positive gamma */
#define CMD_NGC       0xE1U  /* negative gamma */
#define CMD_CSCON     0xF0U  /* command set control */

/* MADCTL bits */
#define MADCTL_MY   0x80U
#define MADCTL_MX   0x40U
#define MADCTL_MV   0x20U
#define MADCTL_BGR  0x08U

/* Pixel scratch buffer for fills: 480 px * 2 B = one full row in landscape */
#define FILL_BUF_PIXELS 480U

/* Fills smaller than this stay blocking: at 20 MHz 256 px take ~26 us,
   about what starting and finishing a DMA transfer costs anyway. */
#define DMA_MIN_PIXELS  256U
/* DMA NDTR is 16 bits: longer fills are sent as several chunks */
#define DMA_MAX_CHUNK   0xFFFFU

static SPI_HandleTypeDef *tft_spi;
static uint16_t tft_width  = ST7796S_WIDTH;
static uint16_t tft_height = ST7796S_HEIGHT;
static uint8_t  fill_buf[FILL_BUF_PIXELS * 2U];

/* DMA fill state, shared with HAL_SPI_TxCpltCallback() (ISR context).
   dma_color must stay static: the DMA keeps reading it after FillRect()
   has returned. */
static volatile uint8_t  dma_busy;
static volatile uint32_t dma_left;   /* pixels not yet handed to the DMA */
static uint16_t          dma_color;

/* DMA text job (blit_text() -> text_next() in HAL_SPI_TxCpltCallback()).
   The string is copied and its glyphs looked up at start, so the caller's
   buffer may go away and the interrupt never searches the font table. */
#define TEXT_MAX_CHARS  (FILL_BUF_PIXELS / 6U)    /* 80 chars at scale 1 */
static volatile uint8_t  txt_active;
static const uint8_t    *txt_glyph[TEXT_MAX_CHARS];  /* NULL = blank cell */
static uint16_t          txt_len;
static uint16_t          txt_w;         /* pixels per line */
static uint16_t          txt_fg, txt_bg;
static uint8_t           txt_scale;
static uint8_t           txt_row;       /* glyph row being sent, 0..6 */
static uint8_t           txt_rep;       /* times this row was sent (scale) */
static uint8_t           txt_cur;       /* line_buf[] index on the wire */
static uint16_t          line_buf[2][FILL_BUF_PIXELS];

/* ---- low-level helpers ------------------------------------------------- */

static inline void cs_low(void)  { HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET); }
static inline void cs_high(void) { HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET); }
static inline void dc_cmd(void)  { HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_RESET); }
static inline void dc_data(void) { HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_SET); }

static void spi_tx(const uint8_t *buf, uint32_t len)
{
  while (len > 0U)
  {
    uint16_t chunk = (len > 0xFFFFU) ? 0xFFFFU : (uint16_t)len;
    HAL_SPI_Transmit(tft_spi, (uint8_t *)buf, chunk, HAL_MAX_DELAY);
    buf += chunk;
    len -= chunk;
  }
}

/* SPI frame size. DFF may only change while the SPI is disabled; HAL
   re-enables it on the next transfer. Init.DataSize is kept in step
   because HAL's polling transmit reads it to pick 8- or 16-bit writes. */
static void spi_frame16(uint8_t on)
{
  __HAL_SPI_DISABLE(tft_spi);
  if (on != 0U)
  {
    SET_BIT(tft_spi->Instance->CR1, SPI_CR1_DFF);
    tft_spi->Init.DataSize = SPI_DATASIZE_16BIT;
  }
  else
  {
    CLEAR_BIT(tft_spi->Instance->CR1, SPI_CR1_DFF);
    tft_spi->Init.DataSize = SPI_DATASIZE_8BIT;
  }
}

/* Hand the next <= 64K pixels of the current fill to the DMA. */
static void dma_next_chunk(void)
{
  uint32_t n = (dma_left > DMA_MAX_CHUNK) ? DMA_MAX_CHUNK : dma_left;

  dma_left -= n;
  if (HAL_SPI_Transmit_DMA(tft_spi, (uint8_t *)&dma_color, (uint16_t)n) != HAL_OK)
  {
    /* Should not happen (SPI was idle). Drop the rest of the fill rather
       than hang every later draw call in dma_wait(). */
    dma_left = 0U;
    spi_frame16(0U);
    cs_high();
    dma_busy = 0U;
  }
}

/* Fills re-send one colour word (memory increment off); text streams a
   line buffer (on). The stream is idle between transfers, so MINC can be
   flipped directly in its CR. */
static void dma_mem_inc(uint8_t on)
{
  if (on) { tft_spi->hdmatx->Instance->CR |=  DMA_SxCR_MINC; }
  else    { tft_spi->hdmatx->Instance->CR &= ~DMA_SxCR_MINC; }
}

/* Render glyph row `row` of the current text job into line_buf[b] */
static void text_render(uint8_t b, uint8_t row)
{
  uint16_t *p = line_buf[b];

  for (uint16_t i = 0; i < txt_len; i++)
  {
    const uint8_t bits  = (txt_glyph[i] != NULL) ? txt_glyph[i][row] : 0U;
    const uint8_t ncols = (i == (uint16_t)(txt_len - 1U)) ? 5U : 6U;  /* no gap after the last one */

    for (uint8_t col = 0; col < ncols; col++)
    {
      const uint16_t c = (col < 5U) && (((bits >> (4U - col)) & 1U) != 0U) ? txt_fg : txt_bg;

      for (uint8_t k = 0; k < txt_scale; k++) { *p++ = c; }
    }
  }
}

static void text_finish(void)
{
  txt_active = 0U;
  dma_mem_inc(0U);
  spi_frame16(0U);
  cs_high();
  dma_busy = 0U;
}

static void text_send(void)
{
  if (HAL_SPI_Transmit_DMA(tft_spi, (uint8_t *)line_buf[txt_cur], txt_w) != HAL_OK)
  {
    text_finish();  /* give the rest up rather than hang dma_wait() */
  }
}

/* Interrupt: one pixel line of the text job is out - send the next one */
static void text_next(void)
{
  if (++txt_rep < txt_scale)
  {
    text_send();        /* same line again (scale > 1) */
    return;
  }
  txt_rep = 0U;
  if (++txt_row >= 7U)
  {
    text_finish();
    return;
  }
  txt_cur ^= 1U;        /* already holds txt_row */
  text_send();
  if (txt_row < 6U)
  {
    text_render((uint8_t)(txt_cur ^ 1U), (uint8_t)(txt_row + 1U));  /* while this one is on the wire */
  }
}

/* Block until a DMA fill started by FillRect() is on the wire and CS is
   released. Every function that touches the bus calls this first. */
static void dma_wait(void)
{
  while (dma_busy != 0U)
  {
  }
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
  if ((tft_spi == NULL) || (hspi != tft_spi))
  {
    return;
  }
  if (txt_active)
  {
    text_next();
    return;
  }
  if (dma_left > 0U)
  {
    dma_next_chunk();
    return;
  }
  /* HAL has already waited for BSY=0, so the last pixel is out */
  spi_frame16(0U);
  cs_high();
  dma_busy = 0U;
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
  if ((tft_spi == NULL) || (hspi != tft_spi))
  {
    return;
  }
  /* Only reachable through a DMA error (SPI3_IRQn is left off, see
     HAL_SPI_MspInit()); HAL has already stopped the stream. */
  if (txt_active)
  {
    text_finish();
    return;
  }
  dma_left = 0U;
  spi_frame16(0U);
  cs_high();
  dma_busy = 0U;
}

/* Command with optional parameter bytes, framed by CS. */
static void write_cmd(uint8_t cmd, const uint8_t *params, uint8_t nparams)
{
  dma_wait();
  cs_low();
  dc_cmd();
  spi_tx(&cmd, 1U);
  if (nparams > 0U)
  {
    dc_data();
    spi_tx(params, nparams);
  }
  cs_high();
}

static void write_cmd0(uint8_t cmd)
{
  write_cmd(cmd, NULL, 0U);
}

/* Select the RAM window; the next RAMWR stream fills it row by row. */
static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
  uint8_t col[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
  uint8_t row[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
  write_cmd(CMD_CASET, col, 4U);
  write_cmd(CMD_RASET, row, 4U);
}

/* ---- public API -------------------------------------------------------- */

void ST7796S_Backlight(uint8_t on)
{
  HAL_GPIO_WritePin(TFT_BL_GPIO_Port, TFT_BL_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

uint16_t ST7796S_Width(void)  { return tft_width; }
uint16_t ST7796S_Height(void) { return tft_height; }

void ST7796S_SetRotation(uint8_t rotation)
{
  uint8_t madctl;

  switch (rotation & 3U)
  {
    default:
    case 0: madctl = MADCTL_MX | MADCTL_BGR;                        break;
    case 1: madctl = MADCTL_MV | MADCTL_BGR;                        break;
    case 2: madctl = MADCTL_MY | MADCTL_BGR;                        break;
    case 3: madctl = MADCTL_MX | MADCTL_MY | MADCTL_MV | MADCTL_BGR; break;
  }

  if ((rotation & 1U) != 0U)
  {
    tft_width  = ST7796S_HEIGHT;
    tft_height = ST7796S_WIDTH;
  }
  else
  {
    tft_width  = ST7796S_WIDTH;
    tft_height = ST7796S_HEIGHT;
  }

  write_cmd(CMD_MADCTL, &madctl, 1U);
}

void ST7796S_Init(SPI_HandleTypeDef *hspi)
{
  tft_spi = hspi;

  cs_high();
  ST7796S_Backlight(0U);

  /* Hardware reset: datasheet asks >= 10 us low, then up to 120 ms before
     the controller accepts commands again. */
  HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_SET);
  HAL_Delay(10);
  HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_RESET);
  HAL_Delay(20);
  HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_SET);
  HAL_Delay(120);

  write_cmd0(CMD_SWRESET);
  HAL_Delay(120);
  write_cmd0(CMD_SLPOUT);
  HAL_Delay(120);

  /* Unlock manufacturer command set (part 1 + part 2) */
  { uint8_t p[] = { 0xC3 }; write_cmd(CMD_CSCON, p, 1U); }
  { uint8_t p[] = { 0x96 }; write_cmd(CMD_CSCON, p, 1U); }

  ST7796S_SetRotation(0U);

  /* 16 bit/pixel RGB565 over SPI (unlike ILI9488, ST7796S accepts it) */
  { uint8_t p[] = { 0x55 }; write_cmd(CMD_COLMOD, p, 1U); }

  { uint8_t p[] = { 0x01 };             write_cmd(CMD_DIC, p, 1U); }   /* 1-dot inversion */
  { uint8_t p[] = { 0x80, 0x02, 0x3B }; write_cmd(CMD_DFC, p, 3U); }
  { uint8_t p[] = { 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33 }; write_cmd(CMD_DOCA, p, 8U); }
  { uint8_t p[] = { 0x06 }; write_cmd(CMD_PWR2, p, 1U); }
  { uint8_t p[] = { 0xA7 }; write_cmd(CMD_PWR3, p, 1U); }
  { uint8_t p[] = { 0x18 }; write_cmd(CMD_VCMPCTL, p, 1U); }
  HAL_Delay(120);

  { uint8_t p[] = { 0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x0F, 0x0F, 0x18, 0x1B };
    write_cmd(CMD_PGC, p, 14U); }
  { uint8_t p[] = { 0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B };
    write_cmd(CMD_NGC, p, 14U); }
  HAL_Delay(120);

  /* Lock manufacturer command set again */
  { uint8_t p[] = { 0x3C }; write_cmd(CMD_CSCON, p, 1U); }
  { uint8_t p[] = { 0x69 }; write_cmd(CMD_CSCON, p, 1U); }
  HAL_Delay(120);

  /* Confirmed on real hardware (photo of the status screen): every colour
     came out as the exact bitwise inverse of what was requested (yellow
     <-> blue, cyan <-> red, white <-> black, background white instead of
     black) - this panel needs INVOFF, not INVON. Was backwards from the
     start; a solid-fill smoke test doesn't make an inverted colour as
     obvious as text/labels do, so it went unnoticed until now. */
  write_cmd0(CMD_INVOFF);

  /* Clear GRAM (contains garbage after power-up) before showing the panel */
  ST7796S_FillScreen(ST7796S_BLACK);
  write_cmd0(CMD_DISPON);
  HAL_Delay(20);
  ST7796S_Backlight(1U);
}

void ST7796S_ReadID(uint8_t out[4])
{
  uint8_t cmd = CMD_RDID4;

  memset(out, 0, 4);
  dma_wait();
  cs_low();
  dc_cmd();
  spi_tx(&cmd, 1U);
  dc_data();
  HAL_SPI_Receive(tft_spi, out, 4U, 100U);
  cs_high();
}

void ST7796S_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
  uint32_t remaining;
  uint32_t n;

  dma_wait();
  if ((x >= tft_width) || (y >= tft_height) || (w == 0U) || (h == 0U))
  {
    return;
  }
  if ((uint32_t)x + w > tft_width)  { w = (uint16_t)(tft_width - x); }
  if ((uint32_t)y + h > tft_height) { h = (uint16_t)(tft_height - y); }

  remaining = (uint32_t)w * h;

  set_window(x, y, (uint16_t)(x + w - 1U), (uint16_t)(y + h - 1U));

  cs_low();
  dc_cmd();
  { uint8_t c = CMD_RAMWR; spi_tx(&c, 1U); }
  dc_data();

  if (remaining >= DMA_MIN_PIXELS)
  {
    /* 16-bit frames go out MSB first - same byte order as fill_buf below.
       CS stays low; HAL_SPI_TxCpltCallback() raises it when done. */
    dma_color = color;
    dma_left  = remaining;
    dma_busy  = 1U;
    spi_frame16(1U);
    dma_mem_inc(0U);
    dma_next_chunk();
    return;
  }

  n = (remaining < FILL_BUF_PIXELS) ? remaining : FILL_BUF_PIXELS;
  for (uint32_t i = 0; i < n; i++)
  {
    fill_buf[2U * i]      = (uint8_t)(color >> 8);
    fill_buf[2U * i + 1U] = (uint8_t)color;
  }
  while (remaining > 0U)
  {
    n = (remaining < FILL_BUF_PIXELS) ? remaining : FILL_BUF_PIXELS;
    spi_tx(fill_buf, n * 2U);
    remaining -= n;
  }
  cs_high();
}

void ST7796S_WaitIdle(void)
{
  dma_wait();
}

uint8_t ST7796S_Busy(void)
{
  return dma_busy;
}

void ST7796S_FillScreen(uint16_t color)
{
  ST7796S_FillRect(0U, 0U, tft_width, tft_height, color);
}

void ST7796S_DrawTestPattern(void)
{
  static const uint16_t bars[8] =
  {
    ST7796S_WHITE, ST7796S_YELLOW, ST7796S_CYAN, ST7796S_GREEN,
    ST7796S_MAGENTA, ST7796S_RED, ST7796S_BLUE, ST7796S_BLACK,
  };
  const uint16_t w = tft_width;
  const uint16_t h = tft_height;
  const uint16_t bar_w = (uint16_t)(w / 8U);
  const uint16_t bar_h = (uint16_t)(h / 2U);

  ST7796S_FillScreen(ST7796S_BLACK);

  /* Top half: 8 vertical colour bars, left to right white..black.
     Red must be leftmost of the last three - if it shows blue, the BGR bit
     in ST7796S_SetRotation() is wrong for this panel. */
  for (uint16_t i = 0; i < 8U; i++)
  {
    ST7796S_FillRect((uint16_t)(i * bar_w), 0U, bar_w, bar_h, bars[i]);
  }

  /* Bottom-left: 8x8 checkerboard of 20 px cells - shows pixel alignment */
  for (uint16_t cy = 0; cy < 8U; cy++)
  {
    for (uint16_t cx = 0; cx < 8U; cx++)
    {
      uint16_t c = (((cx + cy) & 1U) != 0U) ? ST7796S_WHITE : ST7796S_BLACK;
      ST7796S_FillRect((uint16_t)(cx * 20U), (uint16_t)(bar_h + cy * 20U), 20U, 20U, c);
    }
  }

  /* Bottom-right: solid grey block with a red square at its top-left corner.
     The red square marks logical (x,y) origin side after rotation. */
  ST7796S_FillRect((uint16_t)(w / 2U), bar_h, (uint16_t)(w / 2U), (uint16_t)(h - bar_h), ST7796S_RGB(64, 64, 64));
  ST7796S_FillRect((uint16_t)(w / 2U + 10U), (uint16_t)(bar_h + 10U), 40U, 40U, ST7796S_RED);

  /* 4 px white frame around the whole panel - clipped edges mean the window
     or MADCTL is off by a few pixels */
  ST7796S_FillRect(0U, 0U, w, 4U, ST7796S_WHITE);
  ST7796S_FillRect(0U, (uint16_t)(h - 4U), w, 4U, ST7796S_WHITE);
  ST7796S_FillRect(0U, 0U, 4U, h, ST7796S_WHITE);
  ST7796S_FillRect((uint16_t)(w - 4U), 0U, 4U, h, ST7796S_WHITE);
}

void ST7796S_DrawDigit7(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        uint8_t digit, uint16_t color, uint16_t bg)
{
  /* Segment bits: a=top, b=top-right, c=bottom-right, d=bottom,
     e=bottom-left, f=top-left, g=middle */
  static const uint8_t seg[10] =
  {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
  };
  const uint16_t t  = (uint16_t)((w / 5U) == 0U ? 1U : (w / 5U)); /* stroke */
  const uint16_t hv = (uint16_t)((h - 3U * t) / 2U);               /* vertical segment length */
  const uint8_t  m  = (digit < 10U) ? seg[digit] : 0U;             /* >9 = blank */

  ST7796S_FillRect(x, y, w, h, bg);
  if (m & 0x01U) { ST7796S_FillRect((uint16_t)(x + t), y, (uint16_t)(w - 2U * t), t, color); }                                   /* a */
  if (m & 0x02U) { ST7796S_FillRect((uint16_t)(x + w - t), (uint16_t)(y + t), t, hv, color); }                                   /* b */
  if (m & 0x04U) { ST7796S_FillRect((uint16_t)(x + w - t), (uint16_t)(y + 2U * t + hv), t, hv, color); }                         /* c */
  if (m & 0x08U) { ST7796S_FillRect((uint16_t)(x + t), (uint16_t)(y + h - t), (uint16_t)(w - 2U * t), t, color); }               /* d */
  if (m & 0x10U) { ST7796S_FillRect(x, (uint16_t)(y + 2U * t + hv), t, hv, color); }                                             /* e */
  if (m & 0x20U) { ST7796S_FillRect(x, (uint16_t)(y + t), t, hv, color); }                                                       /* f */
  if (m & 0x40U) { ST7796S_FillRect((uint16_t)(x + t), (uint16_t)(y + t + hv), (uint16_t)(w - 2U * t), t, color); }             /* g */
}

void ST7796S_DrawNumber7(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         uint32_t value, uint8_t digits, uint16_t color, uint16_t bg)
{
  const uint16_t pitch = (uint16_t)(w + w / 4U);

  for (uint8_t i = 0; i < digits; i++)
  {
    uint16_t cx = (uint16_t)(x + (digits - 1U - i) * pitch);
    uint8_t  d  = (uint8_t)(value % 10U);

    /* blank leading zeros, but always show the units digit */
    if ((value == 0U) && (i > 0U)) { d = 0xFFU; }
    ST7796S_DrawDigit7(cx, y, w, h, d, color, bg);
    value /= 10U;
  }
}

/* 5x7 bitmap font: one row per byte, bit4 = leftmost column ... bit0 =
   rightmost column (B5 lays the 5 args out left-to-right so the source
   reads like the glyph itself). Space, '.', ':', '-', '/', '!', '#', '%', '+', '=', '<', '>',
   digits, and both upper- and lowercase letters - covers plain ASCII text
   arriving over the wire (e.g. TFT_App_ShowReceived()), not just the
   status screen's own fixed vocabulary. Add more rows here (and to the
   table below) if something else is ever needed; ST7796S_DrawChar()
   already falls back to a blank cell for anything not listed, so an
   incomplete table fails safe rather than drawing garbage. */
#define B5(a, b, c, d, e) (uint8_t)(((a) << 4) | ((b) << 3) | ((c) << 2) | ((d) << 1) | (e))

typedef struct
{
  char    ch;
  uint8_t rows[7];
} Font5x7Glyph;

static const Font5x7Glyph font5x7[] =
{
  { ' ', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,0,0,0,0) } },
  { '.', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,0,1,0,0) } },
  { ':', { B5(0,0,0,0,0), B5(0,0,1,0,0), B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,0,1,0,0), B5(0,0,0,0,0), B5(0,0,0,0,0) } },
  { '-', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,0,0,0,0), B5(1,1,1,1,1), B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,0,0,0,0) } },
  { '!', { B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,0,0,0), B5(0,0,1,0,0) } },
  { '#', { B5(0,1,0,1,0), B5(0,1,0,1,0), B5(1,1,1,1,1), B5(0,1,0,1,0), B5(1,1,1,1,1), B5(0,1,0,1,0), B5(0,1,0,1,0) } },
  { '%', { B5(1,1,0,0,0), B5(1,1,0,0,1), B5(0,0,0,1,0), B5(0,0,1,0,0), B5(0,1,0,0,0), B5(1,0,0,1,1), B5(0,0,0,1,1) } },
  { '+', { B5(0,0,0,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(1,1,1,1,1), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,0,0,0) } },
  { '=', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(1,1,1,1,1), B5(0,0,0,0,0), B5(1,1,1,1,1), B5(0,0,0,0,0), B5(0,0,0,0,0) } },
  { '<', { B5(0,0,0,1,0), B5(0,0,1,0,0), B5(0,1,0,0,0), B5(1,0,0,0,0), B5(0,1,0,0,0), B5(0,0,1,0,0), B5(0,0,0,1,0) } },
  { '>', { B5(0,1,0,0,0), B5(0,0,1,0,0), B5(0,0,0,1,0), B5(0,0,0,0,1), B5(0,0,0,1,0), B5(0,0,1,0,0), B5(0,1,0,0,0) } },
  { '/', { B5(0,0,0,0,1), B5(0,0,0,1,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,1,0,0,0), B5(1,0,0,0,0), B5(0,0,0,0,0) } },
  { '0', { B5(0,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,1,1), B5(1,0,1,0,1), B5(1,1,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,0) } },
  { '1', { B5(0,0,1,0,0), B5(0,1,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,1,1,1,0) } },
  { '2', { B5(0,1,1,1,0), B5(1,0,0,0,1), B5(0,0,0,0,1), B5(0,0,0,1,0), B5(0,0,1,0,0), B5(0,1,0,0,0), B5(1,1,1,1,1) } },
  { '3', { B5(0,1,1,1,0), B5(1,0,0,0,1), B5(0,0,0,0,1), B5(0,0,1,1,0), B5(0,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,0) } },
  { '4', { B5(0,0,0,1,0), B5(0,0,1,1,0), B5(0,1,0,1,0), B5(1,0,0,1,0), B5(1,1,1,1,1), B5(0,0,0,1,0), B5(0,0,0,1,0) } },
  { '5', { B5(1,1,1,1,1), B5(1,0,0,0,0), B5(1,1,1,1,0), B5(0,0,0,0,1), B5(0,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,0) } },
  { '6', { B5(0,0,1,1,0), B5(0,1,0,0,0), B5(1,0,0,0,0), B5(1,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,0) } },
  { '7', { B5(1,1,1,1,1), B5(0,0,0,0,1), B5(0,0,0,1,0), B5(0,0,1,0,0), B5(0,1,0,0,0), B5(0,1,0,0,0), B5(0,1,0,0,0) } },
  { '8', { B5(0,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,0) } },
  { '9', { B5(0,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,1), B5(0,0,0,0,1), B5(0,0,0,1,0), B5(0,1,1,0,0) } },
  { 'A', { B5(0,0,1,0,0), B5(0,1,0,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,1,1,1,1), B5(1,0,0,0,1), B5(1,0,0,0,1) } },
  { 'B', { B5(1,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,1,1,1,0) } },
  { 'C', { B5(0,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,0,0,0,1), B5(0,1,1,1,0) } },
  { 'D', { B5(1,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,1,1,1,0) } },
  { 'E', { B5(1,1,1,1,1), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,1,1,1,0), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,1,1,1,1) } },
  { 'F', { B5(1,1,1,1,1), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,1,1,1,0), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,0,0,0,0) } },
  { 'G', { B5(0,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,0), B5(1,0,1,1,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,1) } },
  { 'H', { B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,1,1,1,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1) } },
  { 'I', { B5(0,1,1,1,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,1,1,1,0) } },
  { 'J', { B5(0,0,1,1,1), B5(0,0,0,1,0), B5(0,0,0,1,0), B5(0,0,0,1,0), B5(0,0,0,1,0), B5(1,0,0,1,0), B5(0,1,1,0,0) } },
  { 'K', { B5(1,0,0,0,1), B5(1,0,0,1,0), B5(1,0,1,0,0), B5(1,1,0,0,0), B5(1,0,1,0,0), B5(1,0,0,1,0), B5(1,0,0,0,1) } },
  { 'L', { B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,1,1,1,1) } },
  { 'M', { B5(1,0,0,0,1), B5(1,1,0,1,1), B5(1,0,1,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1) } },
  { 'N', { B5(1,0,0,0,1), B5(1,1,0,0,1), B5(1,0,1,0,1), B5(1,0,0,1,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1) } },
  { 'O', { B5(0,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,0) } },
  { 'P', { B5(1,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,1,1,1,0), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,0,0,0,0) } },
  { 'Q', { B5(0,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,1,0,1), B5(1,0,0,1,0), B5(0,1,1,0,1) } },
  { 'R', { B5(1,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,1,1,1,0), B5(1,0,1,0,0), B5(1,0,0,1,0), B5(1,0,0,0,1) } },
  { 'S', { B5(0,1,1,1,1), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(0,1,1,1,0), B5(0,0,0,0,1), B5(0,0,0,0,1), B5(1,1,1,1,0) } },
  { 'T', { B5(1,1,1,1,1), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0) } },
  { 'U', { B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,0) } },
  { 'V', { B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,0,1,0), B5(0,0,1,0,0) } },
  { 'W', { B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,1,0,1), B5(1,0,1,0,1), B5(1,1,0,1,1), B5(1,0,0,0,1) } },
  { 'X', { B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,0,1,0), B5(0,0,1,0,0), B5(0,1,0,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1) } },
  { 'Y', { B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,0,1,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0) } },
  { 'Z', { B5(1,1,1,1,1), B5(0,0,0,0,1), B5(0,0,0,1,0), B5(0,0,1,0,0), B5(0,1,0,0,0), B5(1,0,0,0,0), B5(1,1,1,1,1) } },
  { 'a', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,1,1,1,0), B5(0,0,0,0,1), B5(0,1,1,1,1), B5(1,0,0,0,1), B5(0,1,1,1,1) } },
  { 'b', { B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,1,1,1,0) } },
  { 'c', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,1,1,1,1), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(0,1,1,1,1) } },
  { 'd', { B5(0,0,0,0,1), B5(0,0,0,0,1), B5(0,1,1,1,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,1) } },
  { 'e', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,1,1,1,0), B5(1,0,0,0,1), B5(1,1,1,1,1), B5(1,0,0,0,0), B5(0,1,1,1,1) } },
  { 'f', { B5(0,0,1,1,0), B5(0,1,0,0,1), B5(0,1,0,0,0), B5(1,1,1,1,0), B5(0,1,0,0,0), B5(0,1,0,0,0), B5(0,1,0,0,0) } },
  { 'g', { B5(0,0,0,0,0), B5(0,1,1,1,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,1), B5(0,0,0,0,1), B5(0,1,1,1,0) } },
  { 'h', { B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1) } },
  { 'i', { B5(0,0,1,0,0), B5(0,0,0,0,0), B5(0,1,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,1,1,1,0) } },
  { 'j', { B5(0,0,0,1,0), B5(0,0,0,0,0), B5(0,0,1,1,0), B5(0,0,0,1,0), B5(0,0,0,1,0), B5(1,0,0,1,0), B5(0,1,1,0,0) } },
  { 'k', { B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,0,0,1,0), B5(1,0,1,0,0), B5(1,1,0,0,0), B5(1,0,1,0,0), B5(1,0,0,1,0) } },
  { 'l', { B5(0,1,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,1,1,1,0) } },
  { 'm', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(1,1,0,1,0), B5(1,0,1,0,1), B5(1,0,1,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1) } },
  { 'n', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(1,0,1,1,0), B5(1,1,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1) } },
  { 'o', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,1,1,1,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,0) } },
  { 'p', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(1,1,1,1,0), B5(1,0,0,0,1), B5(1,1,1,1,0), B5(1,0,0,0,0), B5(1,0,0,0,0) } },
  { 'q', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,1,1,1,1), B5(1,0,0,0,1), B5(0,1,1,1,1), B5(0,0,0,0,1), B5(0,0,0,0,1) } },
  { 'r', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(1,0,1,1,0), B5(1,1,0,0,1), B5(1,0,0,0,0), B5(1,0,0,0,0), B5(1,0,0,0,0) } },
  { 's', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(0,1,1,1,1), B5(1,0,0,0,0), B5(0,1,1,1,0), B5(0,0,0,0,1), B5(1,1,1,1,0) } },
  { 't', { B5(0,0,1,0,0), B5(0,1,1,1,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,1,0,0), B5(0,0,0,1,0) } },
  { 'u', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,1) } },
  { 'v', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,0,1,0), B5(0,0,1,0,0) } },
  { 'w', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(1,0,1,0,1), B5(1,0,1,0,1), B5(0,1,0,1,0) } },
  { 'x', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(1,0,0,0,1), B5(0,1,0,1,0), B5(0,0,1,0,0), B5(0,1,0,1,0), B5(1,0,0,0,1) } },
  { 'y', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(1,0,0,0,1), B5(1,0,0,0,1), B5(0,1,1,1,1), B5(0,0,0,0,1), B5(0,1,1,1,0) } },
  { 'z', { B5(0,0,0,0,0), B5(0,0,0,0,0), B5(1,1,1,1,1), B5(0,0,0,1,0), B5(0,0,1,0,0), B5(0,1,0,0,0), B5(1,1,1,1,1) } },
};
#define FONT5X7_COUNT (sizeof(font5x7) / sizeof(font5x7[0]))

uint16_t ST7796S_CharPitch(uint8_t scale)
{
  return (uint16_t)(6U * ((scale == 0U) ? 1U : scale));  /* 5 px glyph + 1 px gap */
}

static const uint8_t *glyph_rows(char c)
{
  for (uint32_t i = 0; i < FONT5X7_COUNT; i++)
  {
    if (font5x7[i].ch == c) { return font5x7[i].rows; }
  }
  return NULL;
}

/* Text fast path: the whole string as ONE RAM window, its pixel lines
   streamed by DMA (see the file header). Also paints the 1-px (x scale)
   gaps between characters with bg, but not the one after the last
   character, so nothing outside the old footprint is touched. Returns as
   soon as the first line is on its way. Caller guarantees the box is on
   screen and one pixel line fits a line buffer (text_fits()), and that
   the bus is idle (dma_wait()). Unknown characters come out blank. */
static void blit_text(uint16_t x, uint16_t y, const char *str, uint16_t len,
                      uint16_t color, uint16_t bg, uint8_t s)
{
  txt_len   = len;
  txt_w     = (uint16_t)(len * 6U * s - s);
  txt_fg    = color;
  txt_bg    = bg;
  txt_scale = s;
  for (uint16_t i = 0; i < len; i++)
  {
    txt_glyph[i] = glyph_rows(str[i]);
  }
  text_render(0U, 0U);
  text_render(1U, 1U);
  txt_row = 0U;
  txt_rep = 0U;
  txt_cur = 0U;

  set_window(x, y, (uint16_t)(x + txt_w - 1U), (uint16_t)(y + 7U * s - 1U));

  cs_low();
  dc_cmd();
  { uint8_t c = CMD_RAMWR; spi_tx(&c, 1U); }
  dc_data();

  /* 16-bit frames go out MSB first = RGB565 as stored in line_buf */
  dma_busy   = 1U;
  txt_active = 1U;
  spi_frame16(1U);
  dma_mem_inc(1U);
  text_send();
}

/* True when a len-character string at (x,y) can go through blit_text() */
static uint8_t text_fits(uint16_t x, uint16_t y, uint32_t len, uint8_t s)
{
  const uint32_t w = len * 6U * s - s;

  return (uint8_t)((len > 0U) && (len <= TEXT_MAX_CHARS) && (w <= FILL_BUF_PIXELS) &&
                   ((uint32_t)x + w <= tft_width) && ((uint32_t)y + 7U * s <= tft_height));
}

void ST7796S_DrawChar(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t scale)
{
  const uint8_t *rows;
  const uint8_t  s = (scale == 0U) ? 1U : scale;

  dma_wait();
  if (text_fits(x, y, 1U, s))
  {
    blit_text(x, y, &c, 1U, color, bg, s);
    return;
  }

  /* Partly off screen: per-run fills below, FillRect() clips each one */
  rows = glyph_rows(c);
  if (rows == NULL)
  {
    /* Unsupported character: leave a blank (background) cell rather than
       drawing garbage - a typo in a status string just shows a gap. */
    ST7796S_FillRect(x, y, (uint16_t)(5U * s), (uint16_t)(7U * s), bg);
    return;
  }

  for (uint8_t row = 0; row < 7U; row++)
  {
    const uint8_t bits = rows[row];
    uint8_t col = 0;

    /* Run-length the row into on/off spans so a mostly-solid or
       mostly-blank row costs one SPI fill instead of five 1px-wide ones. */
    while (col < 5U)
    {
      const uint8_t bit = (uint8_t)((bits >> (4U - col)) & 1U);
      const uint8_t run_start = col;

      while ((col < 5U) && (((bits >> (4U - col)) & 1U) == bit)) { col++; }
      ST7796S_FillRect((uint16_t)(x + (uint16_t)run_start * s), (uint16_t)(y + (uint16_t)row * s),
                       (uint16_t)((col - run_start) * s), s, bit ? color : bg);
    }
  }
}

uint16_t ST7796S_DrawString(uint16_t x, uint16_t y, const char *s, uint16_t color, uint16_t bg, uint8_t scale)
{
  const uint16_t pitch = ST7796S_CharPitch(scale);
  const uint8_t  sc    = (scale == 0U) ? 1U : scale;
  const size_t   len   = strlen(s);
  uint16_t cx = x;

  dma_wait();
  if (text_fits(x, y, (uint32_t)len, sc))
  {
    blit_text(x, y, s, (uint16_t)len, color, bg, sc);
    return (uint16_t)(x + len * pitch);
  }

  /* Wider than one fill_buf line, or partly off screen: per character */
  while (*s != '\0')
  {
    ST7796S_DrawChar(cx, y, *s, color, bg, scale);
    cx = (uint16_t)(cx + pitch);
    s++;
  }
  return cx;
}

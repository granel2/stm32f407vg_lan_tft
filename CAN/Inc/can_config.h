/**
  ******************************************************************************
  * @file    can_config.h
  * @brief   Settings for the CAN1 driver (CAN/). Every value is #ifndef'd, so
  *          it can also be overridden from the compiler command line.
  *
  *          Hardware: CAN1 on PA11 (RX) / PA12 (TX), AF9 -> TCAN1044V (U3),
  *          STB tied low (always active), connector J3 (GND, CANL, CANH,
  *          +5V). A 120 ohm split terminator (R2/R30) is fitted permanently,
  *          so this node must sit at one END of the bus.
  ******************************************************************************
  */
#ifndef CAN_CONFIG_H
#define CAN_CONFIG_H

/* Bit rate in kbit/s. The prescaler is derived from the real APB1 clock at
   init (40 MHz here); 10, 20, 50, 100, 125, 250, 500, 800 and 1000 all
   divide evenly. Sample point ~87.5 % (CiA recommendation). */
#ifndef CAN_BITRATE_KBPS
#define CAN_BITRATE_KBPS      500U
#endif

/* 1 = listen-only (bxCAN silent mode): the node never drives the bus - no
   ACK, no error frames, no transmit. Safe for sniffing a live bus.
   0 = normal: receives and ACKs frames, can transmit. */
#ifndef CAN_SILENT
#define CAN_SILENT            0
#endif

/* Received frames printed to USART1, at most this many per second (the
   UART print is blocking - a busy bus would otherwise stall the main loop).
   0 = print no frames, only the periodic summary. */
#ifndef CAN_LOG_MAX_PER_S
#define CAN_LOG_MAX_PER_S     20U
#endif

/* Summary line ("[can] 10 s: rx ... tx ... state ...") every N seconds;
   0 = never. Only printed if something happened or the state changed. */
#ifndef CAN_STATS_PERIOD_S
#define CAN_STATS_PERIOD_S    10U
#endif

/* Receive queue between the RX interrupt and can_bus_poll() (frames) */
#ifndef CAN_RX_QUEUE_LEN
#define CAN_RX_QUEUE_LEN      32U
#endif

#endif /* CAN_CONFIG_H */

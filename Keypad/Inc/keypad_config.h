/**
  ******************************************************************************
  * @file    keypad_config.h
  * @brief   Settings for the keypad receiver (Keypad/). Every value is
  *          #ifndef'd, so it can also be overridden from the compiler command
  *          line.
  *
  *          Hardware: 4x4 keypad module (ATmega328P, project atm328p_keyboard)
  *          on connector SV5: 1 GND, 2 +5V, 3 SDA PB7, 4 SCL PB6 (I2C1, AF4).
  *          The module is the bus MASTER and writes packets here; this board is
  *          a slave at KBD_HOST_ADDR (kbd_protocol.h). Pull-ups 4.7k-10k to +5V sit
  *          on the module - none on this board, internal pull-ups stay off
  *          (PB6/PB7 are 5 V tolerant).
  ******************************************************************************
  */
#ifndef KEYPAD_CONFIG_H
#define KEYPAD_CONFIG_H

/* NVIC priority of I2C1_EV/I2C1_ER. Must be HIGH (numerically low): after the
   CRC byte arrives the ISR has only the 8 bits of the tail byte (~80 us at
   100 kHz) to decide ACK/NACK for the tail - F4 stretches SCL only after the
   next byte's ACK pulse, so a late decision cannot be held off. The ISR is a
   few dozen instructions. ETH is 5, CAN/TFT DMA 6, debug UART DMA 7. */
#ifndef KEYPAD_IRQ_PRIORITY
#define KEYPAD_IRQ_PRIORITY   1U
#endif

/* Print received input to USART1. 1 = the typed characters as they are
   (bench), 0 = only their count ("GROUP enter, 4 chars") - set 0 once the
   keypad carries real passwords. */
#ifndef KEYPAD_LOG_TEXT
#define KEYPAD_LOG_TEXT       1
#endif

/* Packets between the I2C interrupt and keypad_poll() */
#ifndef KEYPAD_QUEUE_LEN
#define KEYPAD_QUEUE_LEN      8U
#endif

#endif /* KEYPAD_CONFIG_H */

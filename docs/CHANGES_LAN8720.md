# Все исправления — STM32F407VGTx + LAN8720B Ethernet

## Суть проблемы

LAN8720B пад 14 (NINT/REFCLKO) выводил +3.3В DC вместо 50 МГц → DMABMR.SWR не очищался → HAL_ETH_Init FAILED.

---

## 1. Аппаратное исправление (обязательное)

### Проблема
Пин LED2/nINTSEL (пад 2 LAN8720) имеет **внутренний pull-up**. По умолчанию и с R37 (10 кОм к +3V3) — nINTSEL = HIGH = 1 → **NINT-режим**: пад 14 = сигнал прерывания (static HIGH ~3.3В), а не 50 МГц.

Для REFCLKO-режима нужен nINTSEL = LOW = 0 во время сброса.

R37 (10 кОм) **тянет пад 2 к +3V3** — это ошибка разводки платы.

### Исправление
Припаять резистор **1 кОм** от пада 2 LAN8720 (нет. -GREEN / нога R37 пин1 / J1 пин10) к GND:

```
+3V3 ── R37 (10кОм, есть) ──┬── пад 2 LAN8720 (nINTSEL)
GND  ── 1кОм (добавить)    ─┘
```

Напряжение: V(пад2) = 3.3В × 1к / (10к + 1к) = **0.30В** → nINTSEL = 0 → **REFCLKO-режим** → 50 МГц на паде 14 → PA1.

---

## 2. Программные исправления

### 2.1 `Core/Src/stm32f4xx_hal_msp.c` — ожидание стабилизации кристалла

**Проблема:** кристаллический генератор 25 МГц LAN8720 требует до ~300 мс для стабилизации после soft-reset. При 200 мс ожидании ETH init мог запускаться с нестабильным клоком → случайные перезагрузки.

**Было:**
```c
/* Allow crystal / PLL to lock and CLKOUT to stabilise */
HAL_Delay(200);
```

**Стало:**
```c
/* Allow crystal / PLL to lock and CLKOUT to stabilise.
   LAN8720 crystal startup can take up to ~300ms; 500ms is safe. */
HAL_Delay(500);
```

---

### 2.2 `Core/Src/ethernetif.c` — запуск DHCP при появлении линка

**Проблема:** `MX_LWIP_Init` вызывает `dhcp_start` только если линк уже есть при старте. Если линк появляется позже (через `ethernetif_poll_link`), DHCP никогда не запускался → IP не назначался.

**Добавлен include:**
```c
#include "lwip/dhcp.h"
```

**В функции `ethernetif_poll_link`, блок link-up:**
```c
// Было:
HAL_ETH_Start_IT(&EthHandle);
netif_set_up(netif);
netif_set_link_up(netif);

// Стало:
HAL_ETH_Start_IT(&EthHandle);
netif_set_up(netif);
netif_set_link_up(netif);
dhcp_start(netif);          // ← добавлено
```

**В блоке link-down:**
```c
// Было:
HAL_ETH_Stop_IT(&EthHandle);
netif_set_down(netif);
netif_set_link_down(netif);

// Стало:
HAL_ETH_Stop_IT(&EthHandle);
dhcp_stop(netif);           // ← добавлено
netif_set_down(netif);
netif_set_link_down(netif);
```

---

### 2.3 `Core/Inc/tcp_echo_client.h` — адрес TCP echo сервера

Сеть роутера оказалась `10.0.1.x`, а не `192.168.1.x`.

**Было:**
```c
#define TCP_ECHO_SERVER_IP0   192
#define TCP_ECHO_SERVER_IP1   168
#define TCP_ECHO_SERVER_IP2   1
#define TCP_ECHO_SERVER_IP3   100
```

**Стало:**
```c
#define TCP_ECHO_SERVER_IP0   10
#define TCP_ECHO_SERVER_IP1   0
#define TCP_ECHO_SERVER_IP2   1
#define TCP_ECHO_SERVER_IP3   18    // IP ПК = 10.0.1.18
```

---

## 3. Ранее сделанные исправления (предыдущие сессии)

### 3.1 `Core/Src/main.c` — MCO1 как резервный источник 25 МГц
```c
// В SystemClock_Config:
HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_HSE, RCC_MCODIV_1);
// PA8 = 25 МГц (HSE/1) — резерв на случай отказа кристалла LAN8720
```

### 3.2 `Core/Src/stm32f4xx_hal_msp.c` — MDIO-сканирование и PHY soft-reset
В `HAL_ETH_MspInit` добавлено:
- Импульс сброса на PB10 (NRST LAN8720): LOW 10 мс → HIGH, ждать 100 мс
- MDIO-сканирование адресов 0–31 для поиска PHY
- Чтение ID2, BCR, BSR, MCSR (0x11), SMR (0x12) с выводом на UART
- BCR soft-reset (бит 15) с ожиданием очистки
- MDIO CR[4:2]=0x14 (Div102 для HCLK 160 МГц → MDC ≈ 1.57 МГц)

### 3.3 `Core/Src/ethernetif.c` — защита от краша без REF_CLK
```c
// В ethernetif_poll_link:
if (EthHandle.gState != HAL_ETH_STATE_READY)
{
    return;  // LAN8742.IO.ReadReg = NULL → краш без этой защиты
}
```

### 3.4 `Core/Src/stm32f4xx_it.c` — обработчик ETH прерывания
```c
void ETH_IRQHandler(void)
{
    HAL_ETH_IRQHandler(&EthHandle);
}
```

---

### 2.4 `Core/Src/tcp_echo_client.c` — UART RX → TCP forwarding (polling вместо ISR)

**Проблема:** `HAL_UART_Receive_IT` внутри `HAL_UART_RxCpltCallback` возвращала `HAL_BUSY`, потому что `HAL_UART_Transmit` (polling TX) удерживал `huart->Lock`. Прерывание переставало перевооружаться после первого байта → строка с UART никогда не накапливалась → UART-данные не уходили в TCP.

**Решение:** убраны ISR (`HAL_UART_RxCpltCallback`) и `HAL_UART_Receive_IT`. Вместо них — функция `poll_uart_rx()`, которая в главном цикле опрашивает флаг `UART_FLAG_RXNE` и читает регистр DR напрямую. Нет HAL-блокировок, нет конфликта с TX.

```c
static void poll_uart_rx(void)
{
  while (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) && !s_uart_line_ready)
  {
    uint8_t b = (uint8_t)(huart1.Instance->DR & 0xFFU);
    if (b == '\r' || b == '\n')
    {
      if (s_uart_line_pos > 0)
      {
        s_uart_line[s_uart_line_pos++] = '\n';
        s_uart_line[s_uart_line_pos]   = '\0';
        s_uart_line_ready = 1;
      }
    }
    else if (s_uart_line_pos < UART_LINE_BUF - 2U)
    {
      s_uart_line[s_uart_line_pos++] = (char)b;
    }
  }
}
```

`poll_uart_rx()` вызывается в начале `tcp_echo_client_poll`. Если строка (до `\r`/`\n`) накоплена, она немедленно уходит по TCP (`[tcp_echo] tx(uart): ...`). Если нет новых данных — цикл продолжается как прежде (пинги каждые 2 с).

---

## Итоговый результат

```
[eth] HAL_ETH_Init OK
[lwip] IP: 10.0.1.60
[tcp_echo] connected
[tcp_echo] tx: ping #0 from stm32f407
[tcp_echo] rx: Hello Module #49 ! I am module #59.
[alive] t=180000ms eth=LINK_UP
```

Стек полностью работает: RMII 50 МГц → HAL_ETH_Init → LAN8742 PHY@0 → lwIP NO_SYS=1 → DHCP → TCP.

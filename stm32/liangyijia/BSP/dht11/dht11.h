#ifndef __DHT11_H
#define __DHT11_H

#endif

#include "main.h"
#include "gpio.h"
#include "tim.h"
#include "stdio.h"
#include "stm32f1xx_hal.h"

#define DHT11_DAT_PIN GPIO_PIN_13
#define DHT11_DAT_Port GPIOB

void DHT11_Send(void);
uint8_t Is_DHT11_ASK(void);
void DHT11_Data(uint8_t *humi, uint8_t *temp);

typedef enum DHT11_STATUS
{
    DHT11_DAT_RESET,
    DHT11_DAT_SET
} DHT11_STATUS;
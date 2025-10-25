#include "DHT11.h"

void delay_us(uint32_t Delay)
{
    __HAL_TIM_SET_COUNTER(&htim1, 0);             // 设置定时器当前计数寄存器为0
    __HAL_TIM_ENABLE(&htim1);                     // 开始计数
    while (__HAL_TIM_GET_COUNTER(&htim1) < Delay) // 延时
    {
    }
    __HAL_TIM_DISABLE(&htim1); // 延时完毕，关闭定时器
}

/* 拉低数据线 */
static DHT11_STATUS DHT11_DATA_RESET(void)
{
    DHT11_STATUS STA = DHT11_DAT_RESET;
    HAL_GPIO_WritePin(DHT11_DAT_Port, DHT11_DAT_PIN, GPIO_PIN_RESET);
    return STA;
}

/* 拉高数据线 */
static DHT11_STATUS DHT11_DATA_SET(void)
{
    DHT11_STATUS STA = DHT11_DAT_SET;
    HAL_GPIO_WritePin(DHT11_DAT_Port, DHT11_DAT_PIN, GPIO_PIN_SET);
    return STA;
}
/* 获取数据线的状态 */
static GPIO_PinState DHT11_DATA_STA(void)
{
    return HAL_GPIO_ReadPin(DHT11_DAT_Port, DHT11_DAT_PIN);
}

/* 主机输出模式 */
static void DHT11_OUT(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP; // 设置为推挽输出
    GPIO_InitStruct.Pin = DHT11_DAT_PIN;
    GPIO_InitStruct.Pull = GPIO_PULLUP; // 加上上拉电阻，保证总线空闲时刻为高电平
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_Init(DHT11_DAT_Port, &GPIO_InitStruct);
}

/* 从机输入模式 */
static void DHT11_INPUT(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT; // 设置为输入模式
    GPIO_InitStruct.Pin = DHT11_DAT_PIN;
    GPIO_InitStruct.Pull = GPIO_PULLUP; // 加上上拉电阻，保证总线空闲时刻为高电平
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_Init(DHT11_DAT_Port, &GPIO_InitStruct);
}

/* 开始信号 */
void DHT11_Send(void)
{
    DHT11_OUT();        // 切换为输出模式
    DHT11_DATA_RESET(); // 拉低数据线20毫秒作为开始信号
    HAL_Delay(20);
    DHT11_DATA_SET(); // 拉高数据线并等待20-40us
    delay_us(30);
}

/* 从机应答 */
uint8_t Is_DHT11_ASK(void)
{
    uint8_t cnt = 0; // 计时变量
    DHT11_Send();    // 发送开始信号
    DHT11_INPUT();   // 切换为输入模式
    while (DHT11_DATA_STA() == 1 && cnt < 100)
    {
        cnt++;
        delay_us(1); // 等待低电平信号的到来
    }

    if (cnt >= 100)
        return 1; // 如果此时还是为高电平，则dht11没响应
    else
        cnt = 0;
    while (DHT11_DATA_STA() == 0 && cnt < 100) // DHT11响应信号
    {
        cnt++;
        delay_us(1);
    }
    return (cnt > 100) ? 1 : 0; // 判断响应信号是否正常
}
/* 接收数据(Bit) */
uint8_t DHT11_Data_Bit(void)
{
    uint8_t cnt = 0;
    while (1 == DHT11_DATA_STA() && cnt < 100) // dht11响应信号后，会再次拉高数据线80us，用一个while循环等待数据信号的到来
    {
        cnt++;
        delay_us(1);
    }
    if (cnt >= 100)
    {
        return 1;
    }
    else
        cnt = 0;
    while (0 == DHT11_DATA_STA() && cnt < 100)
    {
        cnt++;
        delay_us(1);
    } // DHT11每发送一个数据位之前还会将数据线拉低50us，这个while循环也是用来等待数据位的到来
    if (cnt >= 100)
    {
        return 1;
    }
    /* 26~28us的高电平代表数据0，70us的高电平代表数据1*/
    else
        delay_us(30);                       // 延时30us 这是一个阈值，判断高电平信号的时间；
    return (DHT11_DATA_STA() == 1) ? 1 : 0; // 高电平时间大于26~28us返回1，反之为0；
}

/* 接收数据(Byte) */
uint8_t DHT11_Data_Byte(void)
{
    uint8_t data = 0;
    for (uint8_t i = 0; i < 8; i++)
    {
        data <<= 1;
        data |= DHT11_Data_Bit();
    }
    return data;
}

void DHT11_Data(uint8_t *humi, uint8_t *temp)
{
    uint8_t buf[5]; // 存储温湿度的数值
    Is_DHT11_ASK();
    for (uint8_t i = 0; i < 5; i++)
    {
        buf[i] = DHT11_Data_Byte();
    }
    if (buf[0] + buf[1] + buf[2] + buf[3] == buf[4])
    {
        *humi = buf[0];
        *temp = buf[2];
    }
}

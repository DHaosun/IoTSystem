/**
 * @file    app.c
 * @brief   应用层功能实现 - STM32与ESP32串口通信
 * @author  User
 * @date    2025
 */

#include "app.h"
#include "dht11.h"
#include "bh1750.h"
#include "oled.h"
#include <stdio.h>

// 全局传感器数据变量定义
uint8_t g_temperature = 1;        // 温度值 (°C)
uint8_t g_humidity = 1;           // 湿度值 (%)
uint16_t g_light_intensity = 0;   // 光照强度值 (lux)
uint16_t g_rain_level = 0;        // 雨量大小 (0-4095)
uint8_t g_clothesline_status = 1; // 晾衣架状态 (0=收缩, 1=伸展)
uint8_t g_work_mode = MODE_AUTO;  // 工作模式 (默认自动模式)

/**
 * @brief 发送传感器数据到ESP32
 * @param temperature 温度值 (°C)
 * @param humidity 湿度值 (%)
 * @param light_intensity 光照强度值 (lux)
 * @return HAL_StatusTypeDef 发送状态
 *
 * 数据格式：8字节
 * 字节0: 帧头 (0xAA)
 * 字节1: 温度 (uint8, 整数°C)
 * 字节2: 湿度 (uint8, 整数%)
 * 字节3-4: 光照强度 (uint16, 大端序, lux)
 * 字节5: 晾衣架状态 (uint8, 0=收缩, 1=伸展)
 * 字节6: 工作模式 (uint8, 0=自动, 1=手动)
 * 字节7: 帧尾 (0xCC)
 */
void App_SendSensorData(uint8_t temperature, uint8_t humidity, uint16_t light_intensity, uint8_t clothesline_status)
{
    uint8_t data_frame[8];

    // 字节0: 帧头
    data_frame[0] = 0xAA;

    // 字节1: 温度 (直接使用整数值)
    data_frame[1] = temperature;

    // 字节2: 湿度 (直接使用整数值)
    data_frame[2] = humidity;

    // 字节3-4: 光照强度 (uint16, 大端序)
    // 限制光照强度在16位范围内 (0-65535)
    if (light_intensity > 0xFFFF)
    {
        light_intensity = 0xFFFF;
    }
    data_frame[3] = (uint8_t)(light_intensity >> 8);   // 高字节
    data_frame[4] = (uint8_t)(light_intensity & 0xFF); // 低字节

    // 字节5: 晾衣架状态
    data_frame[5] = g_clothesline_status;

    // 字节6: 工作模式
    data_frame[6] = g_work_mode;

    // 字节7: 帧尾
    data_frame[7] = 0xCC;

    // 通过UART1发送8字节数据
    HAL_UART_Transmit(&huart1, data_frame, 8, 1000);
}

/**
 * @brief OLED显示所有传感器数据
 * @note 显示温度、湿度、光照强度和雨量大小
 */
void App_DisplaySensorData(void)
{
    char display_str[64];

    // 第一行：显示温度和湿度
    sprintf(display_str, "T:%d H:%d ", g_temperature, g_humidity);
    OLED_ShowString(0, 0, display_str, 16, 0);

    // 第二行：显示光照强度
    sprintf(display_str, "Light:%u lux ", g_light_intensity);
    OLED_ShowString(0, 2, display_str, 16, 0);

    // 第三行：显示雨量大小
    sprintf(display_str, "Rain:%u    ", g_rain_level);
    OLED_ShowString(0, 4, display_str, 16, 0);

    if (g_clothesline_status == 1)
    {
        sprintf(display_str, "Extend ");
    }
    else
    {
        sprintf(display_str, "Retract ");
    }
    OLED_ShowString(0, 6, display_str, 16, 0);
}

/**
 * @brief 获取所有传感器数据并更新全局变量
 * @note 该函数会更新g_temperature, g_humidity, g_light_intensity, g_rain_level
 */
void App_UpdateSensorData(void)
{
    uint8_t bh1750_data[2];

    // 读取温湿度数据
    DHT11_Send();

    DHT11_Data(&g_humidity, &g_temperature);

    // 读取光照强度数据
    uint8_t bh1750_opecode = BH1750_ONE_HI_RSLT_1;

    // 启动BH1750测量
    if (BH1750_WriteOpecode(&bh1750_opecode, 1) == HAL_OK)
    {
        HAL_Delay(180); // 等待测量完成，一次测量模式需要120ms，这里给180ms余量

        // 读取光照数据
        if (BH1750_ReadData(bh1750_data, 2) == HAL_OK)
        {
            g_light_intensity = BH1750_RawToLux(bh1750_data);
        }
    }

    // 读取雨量数据
    g_rain_level = App_ReadRainSensor();
}

/**
 * @brief 采集雨滴传感器模拟电压值
 * @return uint16_t 雨量相关值 (0-4095)
 *
 * 雨滴传感器工作原理：
 * - 传感器板上无水时，输出高电压 (接近VCC)
 * - 传感器板上有水时，输出低电压 (接近GND)
 * - 水量越多，输出电压越低
 *
 * 返回值说明：
 * - 返回值 = 4095 - ADC原始值
 * - 这样处理后，返回值与雨量大小正相关
 * - 无雨：返回值接近0
 * - 有雨：返回值越大表示雨量越大
 *
 * ADC配置：
 * - 使用ADC1通道1 (PA1引脚)
 * - 12位分辨率 (0-4095)
 * - 单次转换模式
 */
uint16_t App_ReadRainSensor(void)
{
    uint32_t adc_value = 0;

    // 启动ADC转换
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
        // ADC启动失败，返回0表示无雨
        return 0;
    }

    // 等待转换完成，超时时间100ms
    if (HAL_ADC_PollForConversion(&hadc1, 100) == HAL_OK)
    {
        // 获取ADC转换结果
        adc_value = HAL_ADC_GetValue(&hadc1);
    }

    // 停止ADC转换
    // HAL_ADC_Stop(&hadc1);

    // 将ADC值转换为雨量相关值
    // 雨滴传感器：无水时输出高电压，有水时输出低电压
    // 因此需要反转：雨量值 = 4095 - ADC原始值
    uint16_t rain_level = (uint16_t){4095 - (uint16_t)adc_value};

    return rain_level;
}

/**
 * @brief 处理来自ESP32的控制指令
 * @param command 接收到的指令字节
 */
void App_ProcessCommand(uint8_t command)
{
    switch (command)
    {
    case CMD_EXTEND:
        // 处理晾衣架伸展指令
        App_ServoControl(180);
        g_clothesline_status = 1; // 更新状态为伸展
        break;

    case CMD_RETRACT:
        // 处理晾衣架收缩指令
        App_ServoControl(0);
        g_clothesline_status = 0; // 更新状态为收缩
        break;

    case CMD_MODE_AUTO:
        // 切换到自动模式
        g_work_mode = MODE_AUTO;
        break;

    case CMD_MODE_MANUAL:
        // 切换到手动模式
        g_work_mode = MODE_MANUAL;
        break;

    default:
        // 未知指令
        break;
    }
}

/**
 * @brief 控制SG90舵机转向指定角度
 * @param angle 目标角度 (0-180度)
 * @return HAL_StatusTypeDef 控制状态
 *
 * SG90舵机控制参数：
 * - PWM频率：50Hz (周期20ms)
 * - 0度：脉宽0.5ms，对应CCR值50
 * - 90度：脉宽1.5ms，对应CCR值150
 * - 180度：脉宽2.5ms，对应CCR值250
 *
 * TIM2配置：
 * - 预分频：719 (实际720分频)
 * - 自动重装载：1999 (实际2000计数)
 * - PWM输出：TIM2_CH2 (PB3引脚)
 */
void App_ServoControl(uint16_t angle)
{
    // 角度范围检查
    if (angle > 180)
    {
        angle = 180;
    }

    // 计算CCR值：线性映射 0-180度 到 50-250 CCR值
    // CCR = 50 + (angle * 200) / 180
    uint32_t ccr_value = 50 + (angle * 200) / 180;

    // 设置PWM占空比
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, ccr_value);

    // 启动PWM输出（如果尚未启动）
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_2);
}

/**
 * @brief 智能雨天检测算法
 * @return uint8_t 检测结果 (1=下雨, 0=无雨)
 *
 * 检测逻辑：
 * 1. 湿度过高 (>80%) - 可能下雨
 * 2. 光照强度过低 (<200 lux) - 阴天或雨天
 * 3. 雨量传感器检测到水分 (>500) - 直接检测到雨水
 *
 * 综合判断：
 * - 如果雨量传感器检测到明显水分，直接判定为下雨
 * - 如果湿度高且光照低，判定为可能下雨
 */
uint8_t App_DetectRain(void)
{
    // 雨天检测阈值定义
    const uint8_t HUMIDITY_THRESHOLD = 70; // 湿度阈值 (%)
    const uint16_t LIGHT_THRESHOLD = 400;  // 光照强度阈值 (lux)
    const uint16_t RAIN_THRESHOLD = 500;   // 雨量阈值

    // 直接雨量检测 - 最高优先级
    if (g_rain_level > RAIN_THRESHOLD)
    {
        return 1; // 检测到雨水
    }

    // 综合环境检测 - 湿度和光照
    uint8_t high_humidity = (g_humidity > HUMIDITY_THRESHOLD) ? 1 : 0;
    uint8_t low_light = (g_light_intensity < LIGHT_THRESHOLD) ? 1 : 0;

    // 如果湿度高且光照低，判定为可能下雨
    if (high_humidity && low_light)
    {
        return 1; // 可能下雨
    }

    // 调试信息输出

    return 0; // 无雨
}

/**
 * @brief 智能晾衣架控制函数
 * @note 根据工作模式和雨天检测结果控制晾衣架
 *       自动模式：根据雨天检测自动控制
 *       手动模式：只响应ESP32的控制指令，不执行自动控制
 *
 * 状态更新：
 * - g_clothesline_status = 0: 收回状态
 * - g_clothesline_status = 1: 伸展状态
 */
void App_SmartClotheslineControl(void)
{
    // 只有在自动模式下才执行自动控制逻辑
    if (g_work_mode == MODE_AUTO)
    {
        // 执行雨天检测
        uint8_t is_raining = App_DetectRain();

        if (is_raining)
        {
            // 检测到下雨 - 收回晾衣架

            // 控制舵机转到0度 (收回位置)
            App_ServoControl(0);

            g_clothesline_status = 0; // 更新状态为收回
        }
        else
        {
            // 无雨天气 - 伸展晾衣架

            // 控制舵机转到180度 (伸展位置)
            App_ServoControl(180);

            g_clothesline_status = 1; // 更新状态为伸展
        }
    }
    // 手动模式下不执行自动控制，只响应ESP32的控制指令
}

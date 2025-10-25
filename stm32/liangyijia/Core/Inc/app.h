#ifndef __APP_H__
#define __APP_H__

// 包含文件
#include "main.h"
#include "usart.h"
#include "tim.h"
#include "adc.h"

// ESP32控制指令定义
#define CMD_EXTEND 0x3B     // 晾衣架伸展指令
#define CMD_RETRACT 0x5E    // 晾衣架收缩指令
#define CMD_MODE_AUTO 0xA0  // 切换到自动模式指令
#define CMD_MODE_MANUAL 0xA1 // 切换到手动模式指令

// 工作模式定义
#define MODE_AUTO 0x00   // 自动模式
#define MODE_MANUAL 0x01 // 手动模式

// 全局传感器数据变量声明
extern uint8_t g_temperature;        // 温度值 (°C)
extern uint8_t g_humidity;           // 湿度值 (%)
extern uint16_t g_light_intensity;   // 光照强度值 (lux)
extern uint16_t g_rain_level;        // 雨量大小 (0-4095)
extern uint8_t g_clothesline_status; // 晾衣架状态 (0=收缩, 1=伸展)
extern uint8_t g_work_mode;          // 工作模式 (0=自动, 1=手动)

// 函数声明
/**
 * @brief 发送传感器数据到ESP32
 * @param temperature 温度值 (°C)
 * @param humidity 湿度值 (%)
 * @param light_intensity 光照强度值 (lux)
 * @return HAL_StatusTypeDef 发送状态
 */
void App_SendSensorData(uint8_t temperature, uint8_t humidity, uint16_t light_intensity, uint8_t clothesline_status);

/**
 * @brief 处理来自ESP32的控制指令
 * @param command 接收到的指令字节
 */
void App_ProcessCommand(uint8_t command);

/**
 * @brief 控制SG90舵机转向指定角度
 * @param angle 目标角度 (0-180度)
 * @return HAL_StatusTypeDef 控制状态
 */
void App_ServoControl(uint16_t angle);

/**
 * @brief 采集雨滴传感器模拟电压值
 * @return uint16_t 雨量相关值 (0-4095)，值越大表示雨量越大
 */
uint16_t App_ReadRainSensor(void);

/**
 * @brief 获取所有传感器数据并更新全局变量
 * @note 该函数会更新g_temperature, g_humidity, g_light_intensity, g_rain_level
 */
void App_UpdateSensorData(void);

/**
 * @brief OLED显示所有传感器数据
 * @note 显示温度、湿度、光照强度和雨量大小
 */
void App_DisplaySensorData(void);

/**
 * @brief 智能雨天检测算法
 * @return uint8_t 检测结果 (1=下雨, 0=无雨)
 * @note 综合湿度、光照强度和雨量传感器数据进行智能判断
 */
uint8_t App_DetectRain(void);

/**
 * @brief 智能晾衣架控制函数
 * @note 根据雨天检测结果自动控制晾衣架收回或伸展
 *       下雨时收回(舵机180度)，无雨时伸展(舵机0度)
 */
void App_SmartClotheslineControl(void);

#endif

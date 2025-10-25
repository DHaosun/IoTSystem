/**
 * @file app_types.h
 * @brief 应用程序类型定义
 * @details 定义项目中使用的数据结构和枚举类型
 */

#pragma once

#include <stdint.h>

// 晾衣架工作状态（简化版本，只保留extend和retract）
typedef enum
{
    CLOTHESLINE_EXTEND,   // 伸展状态
    CLOTHESLINE_RETRACT   // 收缩状态
} clothesline_status_t;

// 系统工作模式
typedef enum
{
    SYSTEM_MODE_AUTO,     // 自动模式
    SYSTEM_MODE_MANUAL    // 手动模式
} system_mode_t;

// 传感器数据结构体，供UART与MQTT模块共享
typedef struct
{
    float temperature;                       // 摄氏度
    float humidity;                          // 百分比
    int light_intensity;                     // lux
    uint32_t timestamp_ms;                   // 数据接收时间戳（毫秒）
    clothesline_status_t clothesline_status; // 晾衣架状态
} sensor_data_t;

// 状态转换函数声明
const char *clothesline_status_to_string(clothesline_status_t status);
clothesline_status_t clothesline_status_from_string(const char* status_str);

// 系统模式转换函数声明
const char *system_mode_to_string(system_mode_t mode);
system_mode_t system_mode_from_string(const char* mode_str);
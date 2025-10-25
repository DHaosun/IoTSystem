/**
 * @file mqtt_service.h
 * @brief MQTT通信服务 - 与云端通信的核心模块
 * @details 负责发布传感器数据到指定主题，接收控制指令并转发给UART服务
 * @author 智能晾衣架项目组
 * @date 2024
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "app_types.h"

// ==================== 核心功能接口 ====================

/**
 * @brief 初始化MQTT服务
 * @details 配置MQTT客户端，连接到云端服务器，订阅控制主题
 */
void mqtt_service_init(void);

/**
 * @brief 检查MQTT连接状态
 * @return true 已连接，false 未连接
 * @details 用于判断是否可以进行数据发布
 */
bool mqtt_service_is_connected(void);

/**
 * @brief 发布传感器数据
 * @param data 传感器数据结构体指针
 * @details 将四个传感器数据发布到MQTT主题：
 *          - smart_clothesline/{deviceId}/sensors/temperature
 *          - smart_clothesline/{deviceId}/sensors/humidity  
 *          - smart_clothesline/{deviceId}/sensors/light
 *          - smart_clothesline/{deviceId}/sensors (完整JSON)
 */
void mqtt_service_publish_sensor(const sensor_data_t *data);

/**
 * @brief 发布设备状态
 * @param status 状态字符串
 * @details 发布设备状态到 smart_clothesline/{deviceId}/status 主题
 *          常用状态：online, offline, extend, retract, system_ready等
 */
void mqtt_service_publish_status(const char *status);


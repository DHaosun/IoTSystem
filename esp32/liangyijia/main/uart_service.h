/**
 * @file uart_service.h
 * @brief UART通信服务 - 与STM32通信的核心模块
 * @details 负责接收STM32发送的四个传感器数据（温度、湿度、光照强度、晾衣架状态）
 *          并向STM32发送控制指令（伸展/收回）
 * @author 智能晾衣架项目组
 * @date 2024
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_types.h"

// ==================== 核心功能接口 ====================

/**
 * @brief 初始化UART服务
 * @details 配置UART硬件参数，启动数据接收任务
 */
void uart_service_init(void);

/**
 * @brief 获取最新传感器数据
 * @param out 输出的传感器数据结构体
 * @return true 有有效数据，false 无数据或数据无效
 * @details 返回从STM32接收到的最新四个传感器数据：
 *          - 温度（摄氏度）
 *          - 湿度（百分比）
 *          - 光照强度（lux）
 *          - 晾衣架状态（枚举值）
 */
bool uart_service_get_last_data(sensor_data_t *out);

/**
 * @brief 发送晾衣架伸展指令
 * @return ESP_OK 发送成功，ESP_FAIL 发送失败
 * @details 向STM32发送0x3B指令，控制晾衣架伸展
 */
esp_err_t uart_send_extend(void);

/**
 * @brief 发送晾衣架收回指令
 * @return ESP_OK 发送成功，ESP_FAIL 发送失败
 * @details 向STM32发送0x5E指令，控制晾衣架收回
 */
esp_err_t uart_send_retract(void);

/**
 * @brief 发送切换到自动模式指令
 * @return ESP_OK 发送成功，ESP_FAIL 发送失败
 * @details 向STM32发送0xA0指令，切换到自动模式
 */
esp_err_t uart_send_mode_auto(void);

/**
 * @brief 发送切换到手动模式指令
 * @return ESP_OK 发送成功，ESP_FAIL 发送失败
 * @details 向STM32发送0xA1指令，切换到手动模式
 */
esp_err_t uart_send_mode_manual(void);

/**
 * @brief 获取当前系统工作模式
 * @return 当前系统模式（自动/手动）
 * @details 返回从STM32接收到的最新系统工作模式
 */
system_mode_t uart_service_get_current_mode(void);

// ==================== 诊断和监控接口 ====================

/**
 * @brief UART错误统计结构体
 */
typedef struct {
    uint32_t break_errors;    // Break错误计数
    uint32_t parity_errors;   // 奇偶校验错误计数
    uint32_t frame_errors;    // 帧错误计数
} uart_error_stats_t;

/**
 * @brief 获取UART错误统计信息
 * @param stats 输出的错误统计结构体
 * @details 用于系统诊断和故障排查
 */
void uart_service_get_error_stats(uart_error_stats_t *stats);

/**
 * @brief 发送原始控制指令（内部使用）
 * @param code 指令代码
 * @return ESP_OK 发送成功，ESP_FAIL 发送失败
 * @details 底层指令发送接口，一般不直接使用
 */
esp_err_t uart_send_command(uint8_t code);
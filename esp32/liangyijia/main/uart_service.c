/**
 * @file uart_service.c
 * @brief UART串口通信服务模块
 * @details 负责ESP32与STM32之间的串口通信，包括传感器数据接收和控制指令发送
 * @author 智能晾衣架项目组
 * @date 2024
 */

#include "uart_service.h"
#include "app_config.h"
#include "app_types.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

// ==================== 模块标识和全局变量 ====================
static const char *TAG = "UART_SERVICE";

// 传感器数据缓存
static sensor_data_t s_last_sensor_data = {0}; // 最新传感器数据
static bool s_has_valid_data = false;          // 是否有有效数据标志
static SemaphoreHandle_t s_data_mutex = NULL;  // 数据访问互斥锁

// 系统工作模式
static system_mode_t s_current_mode = SYSTEM_MODE_AUTO; // 当前系统模式，默认自动模式
static SemaphoreHandle_t s_mode_mutex = NULL;           // 模式访问互斥锁

// UART事件队列
static QueueHandle_t s_uart_queue = NULL; // UART事件队列

// UART错误统计
static uint32_t s_break_error_count = 0;  // Break错误计数
static uint32_t s_parity_error_count = 0; // 奇偶校验错误计数
static uint32_t s_frame_error_count = 0;  // 帧错误计数

// ==================== 私有函数声明 ====================
static void uart_receive_task(void *pvParameters);
static bool parse_sensor_data_binary(const uint8_t *data_frame, sensor_data_t *output);
static void process_received_buffer(uint8_t *buffer, size_t *buffer_length);

// ==================== 公共接口实现 ====================

/**
 * @brief 初始化UART服务
 * @details 配置UART1参数，安装驱动程序，创建数据接收任务
 */
void uart_service_init(void)
{
    ESP_LOGI(TAG, "开始初始化UART服务...");

    // 创建数据访问互斥锁
    s_data_mutex = xSemaphoreCreateMutex();
    if (s_data_mutex == NULL)
    {
        ESP_LOGE(TAG, "创建数据互斥锁失败");
        return;
    }

    // 创建模式访问互斥锁
    s_mode_mutex = xSemaphoreCreateMutex();
    if (s_mode_mutex == NULL)
    {
        ESP_LOGE(TAG, "创建模式互斥锁失败");
        return;
    }

    // 配置UART参数
    const uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,           // 波特率115200
        .data_bits = UART_DATA_8_BITS,         // 8位数据位
        .parity = UART_PARITY_DISABLE,         // 无校验位
        .stop_bits = UART_STOP_BITS_1,         // 1位停止位
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, // 禁用硬件流控
        .source_clk = UART_SCLK_DEFAULT,       // 默认时钟源
    };

    // 安装UART驱动程序，启用事件队列支持中断模式
    esp_err_t ret = uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE, UART_BUF_SIZE, 50, &s_uart_queue, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "UART驱动安装失败: %s", esp_err_to_name(ret));
        return;
    }

    // 配置UART参数
    ret = uart_param_config(UART_PORT_NUM, &uart_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "UART参数配置失败: %s", esp_err_to_name(ret));
        return;
    }

    // 设置UART引脚
    ret = uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "UART引脚设置失败: %s", esp_err_to_name(ret));
        return;
    }

    // 设置UART模式为标准UART模式，提高稳定性
    ret = uart_set_mode(UART_PORT_NUM, UART_MODE_UART);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "UART模式设置失败: %s", esp_err_to_name(ret));
        return;
    }

    // 设置RX超时阈值，针对115200波特率优化
    ret = uart_set_rx_timeout(UART_PORT_NUM, 3);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "UART RX超时设置失败: %s", esp_err_to_name(ret));
        return;
    }

    // 设置RX FIFO满阈值，针对115200波特率优化
    ret = uart_set_rx_full_threshold(UART_PORT_NUM, 8);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "UART RX FIFO阈值设置失败: %s", esp_err_to_name(ret));
        return;
    }

    // 清空可能存在的缓冲区数据
    uart_flush(UART_PORT_NUM);

    // 创建UART数据接收任务
    BaseType_t task_ret = xTaskCreate(
        uart_receive_task, // 任务函数
        "uart_rx_task",    // 任务名称
        4096,              // 堆栈大小
        NULL,              // 任务参数
        5,                 // 任务优先级
        NULL               // 任务句柄
    );

    if (task_ret != pdPASS)
    {
        ESP_LOGE(TAG, "创建UART接收任务失败");
        return;
    }

    ESP_LOGI(TAG, "UART1初始化完成（中断模式）- 端口:%d, 波特率:%d, TX引脚:%d, RX引脚:%d",
             UART_PORT_NUM, UART_BAUD_RATE, UART_TX_PIN, UART_RX_PIN);
}

/**
 * @brief 获取最新传感器数据
 * @param output 输出参数，存储传感器数据
 * @return true 有有效数据，false 无有效数据
 */
bool uart_service_get_last_data(sensor_data_t *output)
{
    if (!output)
    {
        ESP_LOGW(TAG, "获取传感器数据失败：输出参数为空");
        return false;
    }

    // 获取互斥锁保护数据访问
    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if (s_has_valid_data)
        {
            *output = s_last_sensor_data;
            xSemaphoreGive(s_data_mutex);
            return true;
        }
        xSemaphoreGive(s_data_mutex);
    }

    return false;
}

/**
 * @brief 发送控制指令
 * @param command_code 控制指令代码
 * @return ESP_OK 发送成功，ESP_FAIL 发送失败
 */
esp_err_t uart_send_command(uint8_t command_code)
{
    int bytes_written = uart_write_bytes(UART_PORT_NUM, (const char *)&command_code, 1);

    if (bytes_written == 1)
    {
        ESP_LOGI(TAG, "发送指令: 0x%02X", command_code);
        return ESP_OK;
    }
    else
    {
        ESP_LOGE(TAG, "发送失败: 0x%02X", command_code);
        return ESP_FAIL;
    }
}

/**
 * @brief 发送晾衣架伸展指令
 * @return ESP_OK 发送成功，ESP_FAIL 发送失败
 */
esp_err_t uart_send_extend(void)
{
    ESP_LOGI(TAG, "晾衣架伸展");
    return uart_send_command(0x3B);
}

/**
 * @brief 发送晾衣架收缩指令
 * @return ESP_OK 发送成功，ESP_FAIL 发送失败
 */
esp_err_t uart_send_retract(void)
{
    ESP_LOGI(TAG, "晾衣架收缩");
    return uart_send_command(0x5E);
}

/**
 * @brief 发送切换到自动模式指令
 * @return ESP_OK 发送成功，ESP_FAIL 发送失败
 */
esp_err_t uart_send_mode_auto(void)
{
    ESP_LOGI(TAG, "切换到自动模式");
    return uart_send_command(0xA0);
}

/**
 * @brief 发送切换到手动模式指令
 * @return ESP_OK 发送成功，ESP_FAIL 发送失败
 */
esp_err_t uart_send_mode_manual(void)
{
    ESP_LOGI(TAG, "切换到手动模式");
    return uart_send_command(0xA1);
}

/**
 * @brief 获取当前系统工作模式
 * @return 当前系统模式（自动/手动）
 */
system_mode_t uart_service_get_current_mode(void)
{
    system_mode_t current_mode = SYSTEM_MODE_AUTO; // 默认值
    
    if (s_mode_mutex != NULL && xSemaphoreTake(s_mode_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        current_mode = s_current_mode;
        xSemaphoreGive(s_mode_mutex);
    }
    
    return current_mode;
}

/**
 * @brief 获取UART错误统计信息
 * @param stats 输出的错误统计结构体指针
 */
void uart_service_get_error_stats(uart_error_stats_t *stats)
{
    if (stats)
    {
        stats->break_errors = s_break_error_count;
        stats->parity_errors = s_parity_error_count;
        stats->frame_errors = s_frame_error_count;
    }
}

// ==================== 私有函数实现 ====================

/**
 * @brief UART数据接收任务（中断模式）
 * @param pvParameters 任务参数（未使用）
 * @details 使用事件队列接收UART中断事件，解析传感器数据并更新缓存
 */
static void uart_receive_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t receive_buffer[128];           // 接收缓冲区
    static uint8_t accumulate_buffer[256]; // 数据累积缓冲区
    static size_t accumulate_length = 0;   // 累积数据长度
    TickType_t last_poll_time = xTaskGetTickCount();

    ESP_LOGI(TAG, "UART数据接收任务启动（混合模式：中断+轮询）");

    while (1)
    {
        bool data_received = false;

        // 方式1：检查UART事件（中断驱动）
        if (xQueueReceive(s_uart_queue, (void *)&event, pdMS_TO_TICKS(20)))
        {
            switch (event.type)
            {
            case UART_DATA:
                // 读取可用数据
                int received_length = uart_read_bytes(UART_PORT_NUM, receive_buffer,
                                                      sizeof(receive_buffer), pdMS_TO_TICKS(50));

                if (received_length > 0)
                {
                    data_received = true;
                    ESP_LOGD(TAG, "中断模式接收到 %d 字节", received_length);

                    // 防止缓冲区溢出
                    if (accumulate_length + received_length >= sizeof(accumulate_buffer))
                    {
                        ESP_LOGW(TAG, "接收缓冲区溢出，重置");
                        accumulate_length = 0;
                    }

                    // 将新数据追加到累积缓冲区
                    memcpy(accumulate_buffer + accumulate_length, receive_buffer, received_length);
                    accumulate_length += received_length;

                    // 立即处理累积的数据
                    process_received_buffer(accumulate_buffer, &accumulate_length);
                }
                break;

            case UART_FIFO_OVF:
                ESP_LOGW(TAG, "UART FIFO溢出");
                uart_flush_input(UART_PORT_NUM);
                xQueueReset(s_uart_queue);
                break;

            case UART_BUFFER_FULL:
                ESP_LOGW(TAG, "UART缓冲区满");
                uart_flush_input(UART_PORT_NUM);
                xQueueReset(s_uart_queue);
                break;

            case UART_BREAK:
                s_break_error_count++;
                // 清空输入缓冲区，避免数据混乱
                uart_flush_input(UART_PORT_NUM);
                // 只在错误频繁时报告
                if (s_break_error_count % 10 == 1)
                {
                    ESP_LOGW(TAG, "UART Break信号检测 (累计%lu次) - 可能是线路问题", s_break_error_count);
                }
                break;

            case UART_PARITY_ERR:
                s_parity_error_count++;
                if (s_parity_error_count % 10 == 1)
                {
                    ESP_LOGW(TAG, "UART奇偶校验错误 (累计%lu次) - 检查波特率设置", s_parity_error_count);
                }
                break;

            case UART_FRAME_ERR:
                s_frame_error_count++;
                if (s_frame_error_count % 10 == 1)
                {
                    ESP_LOGW(TAG, "UART帧错误 (累计%lu次) - 检查波特率设置", s_frame_error_count);
                }
                break;

            case UART_PATTERN_DET:
                // 忽略模式检测事件
                break;

            default:
                ESP_LOGW(TAG, "未知UART事件: %d", event.type);
                break;
            }
        }

        // 方式2：定期轮询检查（防止错过数据）
        TickType_t current_time = xTaskGetTickCount();
        if (!data_received && (current_time - last_poll_time) >= pdMS_TO_TICKS(50))
        {
            // 检查是否有可用数据
            size_t available_bytes = 0;
            esp_err_t ret = uart_get_buffered_data_len(UART_PORT_NUM, &available_bytes);

            if (ret == ESP_OK && available_bytes > 0)
            {
                // 轮询模式读取数据
                int received_length = uart_read_bytes(UART_PORT_NUM, receive_buffer,
                                                      sizeof(receive_buffer), pdMS_TO_TICKS(20));

                if (received_length > 0)
                {
                    data_received = true;
                    ESP_LOGD(TAG, "轮询模式接收到 %d 字节", received_length);

                    // 防止缓冲区溢出
                    if (accumulate_length + received_length >= sizeof(accumulate_buffer))
                    {
                        ESP_LOGW(TAG, "接收缓冲区溢出，重置");
                        accumulate_length = 0;
                    }

                    // 将新数据追加到累积缓冲区
                    memcpy(accumulate_buffer + accumulate_length, receive_buffer, received_length);
                    accumulate_length += received_length;

                    // 立即处理累积的数据
                    process_received_buffer(accumulate_buffer, &accumulate_length);
                }
            }
            last_poll_time = current_time;
        }

        // 如果没有接收到数据，短暂延时避免CPU占用过高
        if (!data_received)
        {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

/**
 * @brief 处理接收到的二进制数据缓冲区
 * @param buffer 数据缓冲区
 * @param buffer_length 缓冲区长度指针
 *
 * 数据格式：8字节固定格式
 * 字节0: 帧头 (0xAA)
 * 字节1: 温度 (uint8, 整数°C)
 * 字节2: 湿度 (uint8, 整数%)
 * 字节3-4: 光照强度 (uint16, 大端序, lux)
 * 字节5: 晾衣架状态 (uint8, 0=收缩, 1=伸展)
 * 字节6-7: 帧尾 (0xBB, 0xCC)
 */
static void process_received_buffer(uint8_t *buffer, size_t *buffer_length)
{
    sensor_data_t parsed_data;
    bool found_valid_frame = false;

    // 查找帧头0xAA
    for (size_t i = 0; i < *buffer_length; i++)
    {
        if (i + 8 > *buffer_length)
        {
            // 数据不足8字节，保留当前数据等待更多数据
            if (i > 0)
            {
                // 移除无效的前导字节
                memmove(buffer, buffer + i, *buffer_length - i);
                *buffer_length -= i;
            }
            break;
        }

        if (buffer[i] == 0xAA)
        {
            // 找到帧头，尝试解析8字节数据帧

            if (parse_sensor_data_binary(buffer + i, &parsed_data))
            {
                // 添加时间戳
                parsed_data.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                
                // 注意：晾衣架状态已在parse_sensor_data_binary函数中从UART数据解析获得
                // 不需要在这里覆盖，保持从STM32接收到的状态值

                // 更新全局数据
                if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                {
                    s_last_sensor_data = parsed_data;
                    s_has_valid_data = true;
                    xSemaphoreGive(s_data_mutex);

                    ESP_LOGI(TAG, "传感器数据: T=%.1f°C H=%.1f%% L=%d 晾衣架状态=%s",
                             parsed_data.temperature, parsed_data.humidity,
                             parsed_data.light_intensity, 
                             clothesline_status_to_string(parsed_data.clothesline_status));
                }
                else
                {
                    ESP_LOGW(TAG, "数据锁获取失败");
                }

                // 移除已处理的数据
                memmove(buffer, buffer + i + SENSOR_DATA_FRAME_SIZE, *buffer_length - i - SENSOR_DATA_FRAME_SIZE);
                *buffer_length -= (i + SENSOR_DATA_FRAME_SIZE);
                return; // 找到一个有效帧后返回
            }
            else
            {
                // 解析失败，继续查找下一个帧头
                ESP_LOGW(TAG, "数据帧解析失败，继续查找下一个帧头");
            }
        }
    }

    // 如果缓冲区过长仍未找到有效数据，或者数据过旧，清空缓冲区
    if (*buffer_length > 64 && !found_valid_frame)
    {
        ESP_LOGW(TAG, "缓冲区数据过长且无有效帧头，清空缓冲区 (长度: %zu)", *buffer_length);
        *buffer_length = 0;
    }
}

/**
 * @brief 解析8字节二进制传感器数据
 * @param data_frame 8字节数据帧指针
 * @param output 输出的传感器数据结构
 * @return 解析是否成功
 *
 * 数据格式：
 * 字节0: 帧头 (0xAA)
 * 字节1: 温度 (uint8, 整数°C)
 * 字节2: 湿度 (uint8, 整数%)
 * 字节3-4: 光照强度 (uint16, 大端序, lux)
 * 字节5: 晾衣架状态 (uint8, 0=收缩, 1=伸展)
 * 字节6-7: 帧尾 (0xBB, 0xCC)
 */
static bool parse_sensor_data_binary(const uint8_t *data_frame, sensor_data_t *output)
{
    if (!data_frame || !output)
    {
        return false;
    }

    // 检查帧头
    if (data_frame[0] != 0xAA)
    {
        ESP_LOGW(TAG, "帧头错误: 0x%02X (期望: 0xAA)", data_frame[0]);
        return false;
    }

    // 检查帧尾第二个字节
    if (data_frame[7] != 0xCC)
    {
        ESP_LOGW(TAG, "帧尾第二字节错误: 0x%02X (期望: 0xCC)", data_frame[7]);
        return false;
    }

    // 解析帧尾第一个字节中的模式信息
    uint8_t mode_byte = data_frame[6];
    system_mode_t received_mode;
    
    switch (mode_byte)
    {
        case 0x00:
            received_mode = SYSTEM_MODE_AUTO;
            break;
        case 0x01:
            received_mode = SYSTEM_MODE_MANUAL;
            break;
        default:
            ESP_LOGW(TAG, "未知模式字节: 0x%02X，使用默认自动模式", mode_byte);
            received_mode = SYSTEM_MODE_AUTO;
            break;
    }

    // 更新当前系统模式
    if (xSemaphoreTake(s_mode_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        s_current_mode = received_mode;
        xSemaphoreGive(s_mode_mutex);
        ESP_LOGD(TAG, "系统模式更新为: %s", system_mode_to_string(received_mode));
    }

    // 解析温度 (字节1, uint8, 整数°C)
    uint8_t temp_raw = data_frame[1];
    float temperature = (float)temp_raw;

    // 解析湿度 (字节2, uint8, 整数%)
    uint8_t hum_raw = data_frame[2];
    float humidity = (float)hum_raw;

    // 解析光照强度 (字节3-4, 大端序, uint16, lux)
    uint16_t light_raw = (data_frame[3] << 8) | data_frame[4];
    uint32_t light_intensity = (uint32_t)light_raw;

    // 解析晾衣架状态 (字节5)
    uint8_t clothesline_raw = data_frame[5];
    clothesline_status_t clothesline_status;
    
    // 将原始状态值映射到枚举
    switch (clothesline_raw)
    {
        case 0:
            clothesline_status = CLOTHESLINE_RETRACT;  // 收缩状态
            break;
        case 1:
            clothesline_status = CLOTHESLINE_EXTEND;   // 伸展状态
            break;
        default:
            clothesline_status = CLOTHESLINE_RETRACT;    // 默认收缩状态
            break;
    }

    // 数据范围检查
    if (temperature >= 0.0f && temperature <= 100.0f &&
        humidity >= 0.0f && humidity <= 100.0f &&
        light_intensity <= 65535)
    { // 16位最大值

        output->temperature = temperature;
        output->humidity = humidity;
        output->light_intensity = (int)light_intensity;
        output->clothesline_status = clothesline_status;
        output->timestamp_ms = esp_timer_get_time() / 1000; // 设置时间戳

        ESP_LOGD(TAG, "解析数据: 帧头=0x%02X, 温度=%u(%.1f°C), 湿度=%u(%.1f%%), 光照=%u lux, 晾衣架状态=%u(%s), 模式=%s(0x%02X), 帧尾=0x%02X",
                 data_frame[0], temp_raw, temperature, hum_raw, humidity, light_raw, clothesline_raw,
                 clothesline_status_to_string(clothesline_status), system_mode_to_string(received_mode), mode_byte, data_frame[7]);

        return true;
    }
    else
    {
        ESP_LOGW(TAG, "传感器数据超出有效范围: 温度=%.1f°C, 湿度=%.1f%%, 光照=%u lux",
                 temperature, humidity, light_raw);
        return false;
    }
}
/**
 * @file main.c
 * @brief 智能晾衣架ESP32 C3主控程序
 * @details 负责系统初始化、WiFi连接、MQTT通信和传感器数据处理
 * @author 智能晾衣架项目组
 * @date 2024
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

// ESP-IDF系统头文件
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

// FreeRTOS头文件
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

// 项目模块头文件
#include "app_config.h"
#include "app_types.h"
#include "uart_service.h"
#include "mqtt_service.h"

// ==================== 模块标识和全局变量 ====================
static const char *TAG = "MAIN";

// WiFi事件组和状态位定义
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0 // WiFi连接成功标志位
#define WIFI_FAIL_BIT BIT1      // WiFi连接失败标志位

// WiFi连接重试配置
#define WIFI_MAXIMUM_RETRY 5 // 最大重试次数
static int s_retry_num = 0;  // 当前重试次数

// ==================== 私有函数声明 ====================
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void wifi_init_sta(void);
static void sensor_task(void *pvParameters);
static void system_status_task(void *pvParameters);

// ==================== WiFi事件处理 ====================

/**
 * @brief WiFi和IP事件处理器
 * @param arg 用户参数（未使用）
 * @param event_base 事件基础类型
 * @param event_id 事件ID
 * @param event_data 事件数据
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        // WiFi启动，开始连接
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi启动");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        // WiFi断开连接处理
        if (s_retry_num < WIFI_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "WiFi重连 (%d/%d)", s_retry_num, WIFI_MAXIMUM_RETRY);
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi连接失败");
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        // 获取IP地址成功
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi连接成功: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief 初始化WiFi站点模式
 * @details 配置WiFi为STA模式并连接到指定网络
 */
static void wifi_init_sta(void)
{
    // 创建WiFi事件组
    s_wifi_event_group = xEventGroupCreate();

    // 初始化网络接口
    ESP_ERROR_CHECK(esp_netif_init());

    // 创建默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 创建默认WiFi STA接口
    esp_netif_create_default_wifi_sta();

    // 初始化WiFi驱动
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册WiFi和IP事件处理器
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    // 配置WiFi连接参数 - 使用安全的字符串复制
    wifi_config_t wifi_config = {0}; // 初始化为零
    
    // 安全复制SSID和密码，防止缓冲区溢出
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    
    // 确保字符串以null结尾
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
    
    ESP_LOGI(TAG, "WiFi配置: SSID='%s', 密码长度=%d", 
             wifi_config.sta.ssid, strlen((char*)wifi_config.sta.password));

    // 设置WiFi模式和配置
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi初始化完成");

    // 等待WiFi连接结果
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "WiFi连接成功");
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGE(TAG, "WiFi连接失败");
    }
    else
    {
        ESP_LOGE(TAG, "WiFi连接超时");
    }
}

// ==================== 任务函数 ====================

/**
 * @brief 传感器数据发送任务
 * @param pvParameters 任务参数（未使用）
 * @details 定期获取传感器数据并通过MQTT发布
 */
static void sensor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "传感器任务启动");

    static uint32_t last_published_timestamp = 0;

    while (1)
    {
        // 检查MQTT连接状态
        if (mqtt_service_is_connected())
        {
            sensor_data_t sensor_data;

            // 获取最新传感器数据
            if (uart_service_get_last_data(&sensor_data))
            {
                // 检查数据新鲜度，避免重复发布相同数据
                if (sensor_data.timestamp_ms != last_published_timestamp)
                {
                    // 发布传感器数据（包含工作模式信息）
                    mqtt_service_publish_sensor(&sensor_data);
                    last_published_timestamp = sensor_data.timestamp_ms;
                    
                    // 获取当前模式用于日志显示
                    system_mode_t current_mode = uart_service_get_current_mode();
                    ESP_LOGI(TAG, "MQTT发布: T=%.1f°C H=%.1f%% L=%d 晾衣架状态=%s 模式=%s",
                             sensor_data.temperature, sensor_data.humidity, sensor_data.light_intensity,
                             clothesline_status_to_string(sensor_data.clothesline_status),
                             system_mode_to_string(current_mode));
                }
            }
            else
            {
                ESP_LOGW(TAG, "无传感器数据");
            }
        }
        else
        {
            ESP_LOGW(TAG, "MQTT未连接");
        }

        // 等待下一次发送周期
        vTaskDelay(pdMS_TO_TICKS(SENSOR_UPDATE_INTERVAL_MS));
    }
}

/**
 * @brief 系统状态监控任务
 * @param pvParameters 任务参数（未使用）
 * @details 定期检查系统状态并发布心跳信息
 */
static void system_status_task(void *pvParameters)
{
    ESP_LOGI(TAG, "系统状态监控任务启动");

    uint32_t heartbeat_count = 0;

    while (1)
    {
        // 每30秒发送一次心跳
        if (mqtt_service_is_connected())
        {
            heartbeat_count++;

            // 发布系统状态
            if (heartbeat_count % 6 == 0)
            { // 每3分钟发布一次详细状态
                mqtt_service_publish_status("heartbeat_detailed");
                ESP_LOGI(TAG, "系统心跳 #%lu (详细状态)", heartbeat_count);

                // 每3分钟报告一次UART错误统计
                uart_error_stats_t uart_stats;
                uart_service_get_error_stats(&uart_stats);
                if (uart_stats.break_errors > 0 || uart_stats.parity_errors > 0 || uart_stats.frame_errors > 0)
                {
                    ESP_LOGW(TAG, "UART错误统计: Break=%lu, Parity=%lu, Frame=%lu",
                             uart_stats.break_errors, uart_stats.parity_errors, uart_stats.frame_errors);
                }
            }
            else
            {
                mqtt_service_publish_status("heartbeat");
                ESP_LOGD(TAG, "系统心跳 #%lu", heartbeat_count);
            }
        }

        // 等待30秒
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

// ==================== 主函数 ====================

/**
 * @brief 创建应用任务
 * @details 创建传感器数据发送和系统状态监控任务
 */
static void create_application_tasks(void)
{
    // 创建传感器数据发送任务
    if (xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL) == pdPASS)
    {
        ESP_LOGI(TAG, "✅ 传感器任务创建成功");
    }
    else
    {
        ESP_LOGE(TAG, "❌ 传感器任务创建失败");
    }

    // 创建系统状态监控任务
    if (xTaskCreate(system_status_task, "status_task", 2048, NULL, 3, NULL) == pdPASS)
    {
        ESP_LOGI(TAG, "✅ 状态监控任务创建成功");
    }
    else
    {
        ESP_LOGE(TAG, "❌ 状态监控任务创建失败");
    }
}

/**
 * @brief 等待MQTT连接建立
 * @return true 连接成功，false 连接超时
 */
static bool wait_for_mqtt_connection(void)
{
    ESP_LOGI(TAG, "⏳ 等待MQTT连接...");
    
    const int max_retry = 15; // 最多等待15秒
    for (int retry = 0; retry < max_retry; retry++)
    {
        if (mqtt_service_is_connected())
        {
            ESP_LOGI(TAG, "✅ MQTT连接成功");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGE(TAG, "❌ MQTT连接超时");
    return false;
}

/**
 * @brief 应用程序主入口
 */
void app_main(void)
{
    ESP_LOGI(TAG, "🚀 智能晾衣架系统启动");

    // ==================== 系统初始化 ====================

    // 1. 初始化NVS存储
    ESP_LOGI(TAG, "🔧 初始化NVS存储...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS存储需要擦除，正在重新初始化...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "✅ NVS存储初始化完成");

    // 2. 初始化WiFi连接
    ESP_LOGI(TAG, "🔧 初始化WiFi连接...");
    wifi_init_sta();

    // 3. 初始化UART服务（与STM32通信）
    ESP_LOGI(TAG, "🔧 初始化UART服务...");
    uart_service_init();
    ESP_LOGI(TAG, "✅ UART服务初始化完成");

    // 4. 初始化MQTT服务
    ESP_LOGI(TAG, "🔧 初始化MQTT服务...");
    mqtt_service_init();

    // ==================== 启动应用服务 ====================

    if (wait_for_mqtt_connection())
    {
        // MQTT连接成功，启动完整功能
        create_application_tasks();
        mqtt_service_publish_status("system_ready");
        ESP_LOGI(TAG, "🎉 系统初始化完成，在线模式运行");
    }
    else
    {
        // MQTT连接失败，离线模式运行
        ESP_LOGW(TAG, "⚠️ 系统离线模式运行，请检查网络和MQTT配置");
    }

    ESP_LOGI(TAG, "✅ 系统启动完成");
}

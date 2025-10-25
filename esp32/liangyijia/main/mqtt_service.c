/**
 * @file mqtt_service.c
 * @brief MQTT通信服务模块
 * @details 负责与MQTT服务器的连接、消息发布和订阅，处理远程控制指令
 * @author 智能晾衣架项目组
 * @date 2024
 */

#include "mqtt_service.h"
#include "app_config.h"
#include "uart_service.h"

#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>

// ==================== 模块标识和全局变量 ====================
static const char *TAG = "MQTT_SERVICE";

// MQTT客户端相关变量
static esp_mqtt_client_handle_t s_mqtt_client = NULL; // MQTT客户端句柄
static bool s_is_connected = false;                   // MQTT连接状态
static SemaphoreHandle_t s_connection_mutex = NULL;   // 连接状态互斥锁

// ==================== 私有函数声明 ====================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data);
static void handle_control_message(const char *topic, const char *data);
static void handle_mode_set_message(const char *topic, const char *data);
static void handle_request_message(const char *topic, const char *data);
static cJSON *create_base_json_object(void);
static esp_err_t publish_json_message(const char *topic, cJSON *json_obj);

// ==================== 公共接口实现 ====================

/**
 * @brief 初始化MQTT服务
 * @details 配置MQTT客户端参数，注册事件处理器并启动连接
 */
void mqtt_service_init(void)
{
    ESP_LOGI(TAG, "MQTT服务初始化");

    // 创建连接状态互斥锁
    s_connection_mutex = xSemaphoreCreateMutex();
    if (s_connection_mutex == NULL)
    {
        ESP_LOGE(TAG, "互斥锁创建失败");
        return;
    }

    // 配置MQTT客户端参数
    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = MQTT_BROKER_URI,        // MQTT服务器地址
        .credentials.client_id = DEVICE_ID,           // 客户端ID
        .session.keepalive = 60,                      // 心跳间隔60秒
        .session.disable_clean_session = false,       // 启用清理会话
        .network.timeout_ms = 10000,                  // 网络超时10秒
        .network.refresh_connection_after_ms = 20000, // 连接刷新间隔20秒
    };

    // 初始化MQTT客户端
    s_mqtt_client = esp_mqtt_client_init(&mqtt_config);
    if (s_mqtt_client == NULL)
    {
        ESP_LOGE(TAG, "MQTT客户端初始化失败");
        return;
    }

    // 注册MQTT事件处理器
    esp_err_t ret = esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "注册MQTT事件处理器失败: %s", esp_err_to_name(ret));
        return;
    }

    // 启动MQTT客户端
    ret = esp_mqtt_client_start(s_mqtt_client);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "启动MQTT客户端失败: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "MQTT客户端启动");
}

/**
 * @brief 检查MQTT连接状态
 * @return true 已连接，false 未连接
 */
bool mqtt_service_is_connected(void)
{
    bool connected = false;

    if (xSemaphoreTake(s_connection_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        connected = s_is_connected;
        xSemaphoreGive(s_connection_mutex);
    }

    return connected;
}

/**
 * @brief 发布设备状态消息
 * @param status 状态字符串（如"standby", "extending", "retracted"等）
 */
void mqtt_service_publish_status(const char *status)
{
    if (!status)
    {
        ESP_LOGW(TAG, "状态字符串为空，无法发布");
        return;
    }

    if (!mqtt_service_is_connected())
    {
        ESP_LOGW(TAG, "MQTT未连接，无法发布状态");
        return;
    }

    // 创建状态JSON对象
    cJSON *status_json = create_base_json_object();
    if (!status_json)
    {
        ESP_LOGE(TAG, "创建状态JSON对象失败");
        return;
    }

    // 添加状态信息
    cJSON_AddStringToObject(status_json, "status", status);
    cJSON_AddStringToObject(status_json, "type", "status_update");

    // 发布状态消息
    esp_err_t ret = publish_json_message(STATUS_TOPIC, status_json);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "状态发布: %s", status);
    }
    else
    {
        ESP_LOGE(TAG, "状态发布失败: %s", status);
    }

    cJSON_Delete(status_json);
}

/**
 * @brief 发布传感器数据
 * @param data 传感器数据结构体指针
 * @details 同时发布分主题数据和完整JSON数据
 */
void mqtt_service_publish_sensor(const sensor_data_t *data)
{
    if (!data)
    {
        ESP_LOGW(TAG, "传感器数据为空，无法发布");
        return;
    }

    if (!mqtt_service_is_connected())
    {
        ESP_LOGW(TAG, "MQTT未连接，无法发布传感器数据");
        return;
    }

    // 获取当前工作模式
    system_mode_t current_mode = uart_service_get_current_mode();

    // 创建JSON对象包含所有传感器数据和工作模式
    cJSON *json_obj = cJSON_CreateObject();
    if (!json_obj)
    {
        ESP_LOGE(TAG, "创建JSON对象失败");
        return;
    }

    // 添加传感器数据
    cJSON_AddNumberToObject(json_obj, "temperature", data->temperature);
    cJSON_AddNumberToObject(json_obj, "humidity", data->humidity);
    cJSON_AddNumberToObject(json_obj, "light_intensity", data->light_intensity);
    cJSON_AddStringToObject(json_obj, "clothesline_status", clothesline_status_to_string(data->clothesline_status));

    // 添加工作模式信息
    cJSON_AddStringToObject(json_obj, "work_mode", system_mode_to_string(current_mode));

    // 添加时间戳
    cJSON_AddNumberToObject(json_obj, "timestamp", data->timestamp_ms);

    // 发布完整的JSON数据到基础传感器主题
    esp_err_t result = publish_json_message(SENSOR_TOPIC, json_obj);

    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "传感器数据发布完成: T=%.1f°C H=%.1f%% L=%d 状态=%s 模式=%s",
                 data->temperature, data->humidity, data->light_intensity,
                 clothesline_status_to_string(data->clothesline_status),
                 system_mode_to_string(current_mode));
    }
    else
    {
        ESP_LOGE(TAG, "传感器数据发布失败");
    }

    // 清理JSON对象
    cJSON_Delete(json_obj);
}

// ==================== 私有函数实现 ====================

/**
 * @brief MQTT事件处理器
 * @param handler_args 处理器参数（未使用）
 * @param base 事件基础类型
 * @param event_id 事件ID
 * @param event_data 事件数据
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT连接成功");

        // 更新连接状态
        if (xSemaphoreTake(s_connection_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            s_is_connected = true;
            xSemaphoreGive(s_connection_mutex);
        }

        // 订阅控制主题、模式设置主题和请求主题
        esp_mqtt_client_subscribe(s_mqtt_client, CONTROL_TOPIC, 0);
        ESP_LOGI(TAG, "订阅控制主题");

        esp_mqtt_client_subscribe(s_mqtt_client, MODE_SET_TOPIC, 0);
        ESP_LOGI(TAG, "订阅模式设置主题");

        esp_mqtt_client_subscribe(s_mqtt_client, REQUEST_TOPIC, 0);
        ESP_LOGI(TAG, "订阅请求主题");

        // 发布设备上线状态
        mqtt_service_publish_status("online");
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT连接断开");

        // 更新连接状态
        if (xSemaphoreTake(s_connection_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            s_is_connected = false;
            xSemaphoreGive(s_connection_mutex);
        }
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "主题订阅成功, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "取消主题订阅, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "消息发布成功, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        // 提取主题和数据
        char topic_buffer[128] = {0};
        char data_buffer[256] = {0};

        if (event->topic_len < sizeof(topic_buffer))
        {
            memcpy(topic_buffer, event->topic, event->topic_len);
        }

        if (event->data_len < sizeof(data_buffer))
        {
            memcpy(data_buffer, event->data, event->data_len);
        }

        ESP_LOGI(TAG, "收到消息: %s", data_buffer);

        // 根据主题分发消息处理
        if (strcmp(topic_buffer, CONTROL_TOPIC) == 0)
        {
            handle_control_message(topic_buffer, data_buffer);
        }
        else if (strcmp(topic_buffer, MODE_SET_TOPIC) == 0)
        {
            handle_mode_set_message(topic_buffer, data_buffer);
        }
        else if (strcmp(topic_buffer, REQUEST_TOPIC) == 0)
        {
            handle_request_message(topic_buffer, data_buffer);
        }
        else
        {
            ESP_LOGW(TAG, "收到未知主题的消息: %s", topic_buffer);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT发生错误");
        break;

    default:
        ESP_LOGD(TAG, "其他MQTT事件: %d", event->event_id);
        break;
    }
}

/**
 * @brief 处理控制消息
 * @param topic 消息主题
 * @param data 消息数据
 */
static void handle_control_message(const char *topic, const char *data)
{
    ESP_LOGI(TAG, "控制指令: %s", data);

    if (strcmp(data, "extend") == 0)
    {
        // 立即发送串口信息给STM32
        esp_err_t ret = uart_send_extend();
        if (ret == ESP_OK)
        {
            mqtt_service_publish_status("extend");

            // 延时
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else
        {
            ESP_LOGE(TAG, "伸展指令失败");
        }
    }
    else if (strcmp(data, "retract") == 0)
    {
        // 立即发送串口信息给STM32
        esp_err_t ret = uart_send_retract();
        if (ret == ESP_OK)
        {
            mqtt_service_publish_status("retract");

            // 延时
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else
        {
            ESP_LOGE(TAG, "收缩指令失败");
        }
    }

    else
    {
        ESP_LOGW(TAG, "未知控制指令: %s", data);
    }
}

/**
 * @brief 处理模式设置消息
 * @param topic 消息主题
 * @param data 消息数据
 */
static void handle_mode_set_message(const char *topic, const char *data)
{
    ESP_LOGI(TAG, "模式设置指令: %s", data);

    if (strcmp(data, "auto") == 0)
    {
        // 发送切换到自动模式指令给STM32
        esp_err_t ret = uart_send_mode_auto();
        if (ret == ESP_OK)
        {
            mqtt_service_publish_status("mode_auto");
            ESP_LOGI(TAG, "切换到自动模式成功");

            // 延时
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else
        {
            ESP_LOGE(TAG, "切换到自动模式失败");
        }
    }
    else if (strcmp(data, "manual") == 0)
    {
        // 发送切换到手动模式指令给STM32
        esp_err_t ret = uart_send_mode_manual();
        if (ret == ESP_OK)
        {
            mqtt_service_publish_status("mode_manual");
            ESP_LOGI(TAG, "切换到手动模式成功");

            // 延时
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else
        {
            ESP_LOGE(TAG, "切换到手动模式失败");
        }
    }
    else
    {
        ESP_LOGW(TAG, "未知模式设置指令: %s", data);
    }
}

/**
 * @brief 处理请求消息
 * @param topic 消息主题
 * @param data 消息数据
 */
static void handle_request_message(const char *topic, const char *data)
{
    ESP_LOGI(TAG, "处理数据请求: %s", data);

    // 尝试解析JSON格式的请求
    cJSON *json = cJSON_Parse(data);
    if (json != NULL)
    {
        // 解析JSON格式的请求
        cJSON *action = cJSON_GetObjectItem(json, "action");
        if (action && cJSON_IsString(action))
        {
            const char *action_str = action->valuestring;
            ESP_LOGI(TAG, "解析到请求动作: %s", action_str);

            if (strcmp(action_str, "request_data") == 0)
            {
                // 获取并发布最新传感器数据
                sensor_data_t sensor_data;
                if (uart_service_get_last_data(&sensor_data))
                {
                    mqtt_service_publish_sensor(&sensor_data);
                    ESP_LOGI(TAG, "响应传感器数据请求成功");
                }
                else
                {
                    ESP_LOGW(TAG, "暂无有效传感器数据可发送");
                    mqtt_service_publish_status("no_sensor_data");
                }
            }
            else if (strcmp(action_str, "request_status") == 0)
            {
                // 发布当前设备状态
                mqtt_service_publish_status("retract");
                ESP_LOGI(TAG, "响应状态请求成功");
            }
            else
            {
                ESP_LOGW(TAG, "未知的JSON请求动作: %s", action_str);
            }
        }
        else
        {
            ESP_LOGW(TAG, "JSON请求中缺少action字段");
        }
        cJSON_Delete(json);
    }
    else
    {
        // 兼容旧的字符串格式请求
        if (strcmp(data, "sensor_data") == 0 || strcmp(data, "get_sensors") == 0)
        {
            // 获取并发布最新传感器数据
            sensor_data_t sensor_data;
            if (uart_service_get_last_data(&sensor_data))
            {
                mqtt_service_publish_sensor(&sensor_data);
                ESP_LOGI(TAG, "响应传感器数据请求成功");
            }
            else
            {
                ESP_LOGW(TAG, "暂无有效传感器数据可发送");
                mqtt_service_publish_status("no_sensor_data");
            }
        }
        else if (strcmp(data, "status") == 0 || strcmp(data, "get_status") == 0)
        {
            // 发布当前设备状态
            mqtt_service_publish_status("retract");
            ESP_LOGI(TAG, "响应状态请求成功");
        }
        else
        {
            ESP_LOGW(TAG, "未知的请求类型: %s", data);
        }
    }
}

/**
 * @brief 创建基础JSON对象
 * @return cJSON对象指针，包含设备ID和时间戳
 */
static cJSON *create_base_json_object(void)
{
    cJSON *json_obj = cJSON_CreateObject();
    if (!json_obj)
    {
        return NULL;
    }

    // 添加设备ID和时间戳
    cJSON_AddStringToObject(json_obj, "device_id", DEVICE_ID);
    cJSON_AddNumberToObject(json_obj, "timestamp", (double)time(NULL));

    return json_obj;
}

/**
 * @brief 发布JSON消息
 * @param topic 发布主题
 * @param json_obj JSON对象
 * @return ESP_OK 成功，ESP_FAIL 失败
 */
static esp_err_t publish_json_message(const char *topic, cJSON *json_obj)
{
    if (!topic || !json_obj)
    {
        return ESP_FAIL;
    }

    char *json_string = cJSON_Print(json_obj);
    if (!json_string)
    {
        ESP_LOGE(TAG, "JSON序列化失败");
        return ESP_FAIL;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, json_string, 0, 0, 0);
    free(json_string);

    if (msg_id >= 0)
    {
        return ESP_OK;
    }
    else
    {
        ESP_LOGE(TAG, "MQTT消息发布失败");
        return ESP_FAIL;
    }
}
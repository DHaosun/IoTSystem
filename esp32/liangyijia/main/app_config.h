#pragma once

// ==================== 系统头文件包含 ====================
#include "driver/uart.h" // UART驱动头文件，定义UART_NUM_1等常量

// ==================== WiFi网络配置 ====================
// WiFi SSID最大长度32字节，密码最大长度64字节
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64

// WiFi网络配置 - 请修改为你的实际WiFi信息
#define WIFI_SSID "xm17pm"         // WiFi网络名称（最大31字符）
#define WIFI_PASSWORD "hust666666" // WiFi密码（最大63字符）
#define WIFI_MAXIMUM_RETRY 5       // WiFi连接最大重试次数

// 编译时长度检查
_Static_assert(sizeof(WIFI_SSID) <= WIFI_SSID_MAX_LEN, "WiFi SSID too long");
_Static_assert(sizeof(WIFI_PASSWORD) <= WIFI_PASSWORD_MAX_LEN, "WiFi password too long");

// ==================== MQTT服务器配置 ====================
#define MQTT_BROKER_URI "mqtt://broker.emqx.io:1883" // MQTT服务器地址
#define DEVICE_ID "hust666"                          // 设备唯一标识符

// MQTT主题定义 - 用于数据发布和订阅
#define SENSOR_TOPIC "smart_clothesline/" DEVICE_ID "/sensors"    // 传感器数据主题
#define CONTROL_TOPIC "smart_clothesline/" DEVICE_ID "/control"   // 控制指令主题（extend/retract）
#define MODE_SET_TOPIC "smart_clothesline/" DEVICE_ID "/mode/set" // 模式设置主题（web端发送模式切换指令）
#define STATUS_TOPIC "smart_clothesline/" DEVICE_ID "/status"     // 设备状态主题
#define REQUEST_TOPIC "smart_clothesline/" DEVICE_ID "/request"   // 请求响应主题

// 传感器数据发送间隔（毫秒）
#define SENSOR_UPDATE_INTERVAL_MS 2000

// 传感器数据帧大小（字节）
#define SENSOR_DATA_FRAME_SIZE 8

// ==================== UART串口配置（ESP32-C3 UART1） ====================
// ESP32-C3 UART1引脚配置：TXD=GPIO0, RXD=GPIO1（与STM32通信）
#define UART_PORT_NUM UART_NUM_1 // 使用UART1端口
#define UART_TX_PIN 0            // UART1发送引脚GPIO0
#define UART_RX_PIN 1            // UART1接收引脚GPIO1
#define UART_BAUD_RATE 115200    // 串口波特率
#define UART_BUF_SIZE 2048       // UART缓冲区大小
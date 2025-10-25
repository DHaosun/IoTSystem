# ==================== 智能晾衣架ESP32 C3主组件配置 ====================
# @file CMakeLists.txt
# @brief ESP32 C3智能晾衣架主组件构建配置
# @details 定义主组件的源文件、头文件目录和依赖关系
# @author 智能晾衣架项目组
# @date 2024

# ==================== 组件注册配置 ====================
# 注册主组件，包含所有源文件和头文件目录
idf_component_register(
    # 源文件列表 - 包含所有需要编译的.c文件
    SRCS 
        "main.c"           # 主程序入口，系统初始化和任务管理
        "mqtt_service.c"   # MQTT通信服务，负责与云端的数据交互
        "uart_service.c"   # UART通信服务，负责与STM32的串口通信
        "app_types.c"      # 应用类型定义，包含状态转换函数
    
    # 头文件包含目录 - 指定头文件搜索路径
    INCLUDE_DIRS 
        "."                # 当前目录，包含所有.h头文件
    
    # 组件依赖 - 指定需要链接的ESP-IDF组件
    REQUIRES
        esp_wifi           # WiFi功能组件
        esp_netif          # 网络接口组件
        esp_event          # 事件处理组件
        mqtt               # MQTT客户端组件
        nvs_flash          # 非易失性存储组件
        driver             # 硬件驱动组件（UART等）
        freertos           # FreeRTOS实时操作系统
        esp_system         # ESP32系统组件
        esp_common         # ESP32通用组件
        log                # 日志系统组件
        json               # JSON解析组件
)

# ==================== 编译选项配置 ====================
# 设置组件特定的编译选项
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall              # 启用所有警告
    -Wextra            # 启用额外警告
    -Wno-unused-parameter  # 忽略未使用参数警告（FreeRTOS任务函数常见）
)

# ==================== 预处理器定义 ====================
# 添加预处理器宏定义
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    # 项目标识
    PROJECT_NAME="SmartClothesline"
    PROJECT_VERSION="1.0.0"
    
    # 调试配置
    $<$<CONFIG:Debug>:DEBUG_MODE=1>
    $<$<CONFIG:Release>:DEBUG_MODE=0>
    
    # UART配置标识
    UART_PORT_USED=1  # 标识使用UART1
)

# ==================== 构建信息输出 ====================
# 在构建时输出组件信息
message(STATUS "🔧 智能晾衣架主组件配置完成")
message(STATUS "📁 源文件: main.c, mqtt_service.c, uart_service.c")
message(STATUS "📂 头文件目录: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "🔗 依赖组件: esp_wifi, mqtt, nvs_flash, driver, freertos")

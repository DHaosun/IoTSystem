/**
 * @file app_types.c
 * @brief 应用程序类型定义实现
 * @details 实现应用程序中使用的数据类型相关的工具函数
 * @author 智能晾衣架项目组
 * @date 2024
 */

#include "app_types.h"
#include <string.h>

/**
 * @brief 将状态枚举转换为字符串
 * @param status 状态枚举值
 * @return 状态字符串
 */
const char* clothesline_status_to_string(clothesline_status_t status)
{
    switch (status) {
        case CLOTHESLINE_EXTEND:
            return "extend";
        case CLOTHESLINE_RETRACT:
            return "retract";
        default:
            return "retract"; // 默认返回收缩状态
    }
}

/**
 * @brief 将字符串转换为状态枚举
 * @param status_str 状态字符串
 * @return 状态枚举值
 */
clothesline_status_t clothesline_status_from_string(const char* status_str)
{
    if (status_str == NULL) {
        return CLOTHESLINE_RETRACT;
    }
    
    if (strcmp(status_str, "extend") == 0) {
        return CLOTHESLINE_EXTEND;
    } else if (strcmp(status_str, "retract") == 0) {
        return CLOTHESLINE_RETRACT;
    } else {
        return CLOTHESLINE_RETRACT; // 默认返回收缩状态
    }
}

/**
 * @brief 将系统模式枚举转换为字符串
 * @param mode 系统模式枚举值
 * @return 模式字符串
 */
const char* system_mode_to_string(system_mode_t mode)
{
    switch (mode) {
        case SYSTEM_MODE_AUTO:
            return "auto";
        case SYSTEM_MODE_MANUAL:
            return "manual";
        default:
            return "auto"; // 默认返回自动模式
    }
}

/**
 * @brief 将字符串转换为系统模式枚举
 * @param mode_str 模式字符串
 * @return 系统模式枚举值
 */
system_mode_t system_mode_from_string(const char* mode_str)
{
    if (mode_str == NULL) {
        return SYSTEM_MODE_AUTO;
    }
    
    if (strcmp(mode_str, "auto") == 0) {
        return SYSTEM_MODE_AUTO;
    } else if (strcmp(mode_str, "manual") == 0) {
        return SYSTEM_MODE_MANUAL;
    } else {
        return SYSTEM_MODE_AUTO; // 默认返回自动模式
    }
}
#ifndef BATTERY_CONTROL_H
#define BATTERY_CONTROL_H

#include <stdbool.h>

/**
 * @brief 电池管理控制函数声明
 * 这些函数在main.c中实现，供web服务器调用
 */

/**
 * @brief 创建电池状态的JSON字符串
 * @return JSON格式的电池状态字符串，使用后需要free释放
 */
char* create_battery_status_json(void);

/**
 * @brief 设置外部电池电量等级
 * @param level 电池电量百分比 (0-100)
 */
void set_external_battery_level(int level);

/**
 * @brief 设置外部充电状态
 * @param charging true表示充电中，false表示未充电
 */
void set_external_charging_status(bool charging);

/**
 * @brief 恢复自动电池监测模式
 */
void restore_auto_battery_mode(void);

/**
 * @brief 恢复自动充电状态检测模式
 */
void restore_auto_charging_mode(void);

#endif // BATTERY_CONTROL_H

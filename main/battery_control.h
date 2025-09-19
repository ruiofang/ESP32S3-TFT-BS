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
 * @brief 设置外部电池电量百分比（新函数名）
 * @param percentage 电池电量百分比 (0-100)
 */
void set_external_battery_percentage(int percentage);

/**
 * @brief 设置外部电压值
 * @param voltage 电压值 (18.0V-29.4V)
 */
void set_external_voltage(float voltage);

/**
 * @brief 获取当前电池电压
 * @return 当前电池电压值
 */
float get_battery_voltage(void);

/**
 * @brief 获取当前电池电量百分比
 * @return 当前电池电量百分比 (0-100)
 */
int get_battery_percentage(void);

/**
 * @brief 获取当前充电状态
 * @return true表示充电中，false表示未充电
 */
bool is_charging(void);

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

/**
 * @file ble_talk_network.h
 * @brief BLE 对讲组网组件 —— 公共接口声明
 *
 * 本模块封装了全部组网协议逻辑，包括：
 *   - 房间创建 / 加入 / 退出 / 回连 / 解散 
 *   - 配对超时与回连超时管理
 *   - 成员列表维护
 *
 * 应用层集成步骤：
 *   1. 调用 @ref ble_talk_network_init 初始化组件。
 *   2. 通过 @ref ble_talk_network_register_callbacks 注册 UI 回调。
 *   3. 通过 @ref ble_talk_network_set_ops 注册底层操作钩子。
 *   4. 在全局 BLE 事件处理器中转发 @ref ble_talk_network_event_handler。
 *   5. 使用高层动作 API 组件业务流程。
 *
 * @copyright Copyright (c) 2024-2024 SiFli Technologies (Nanjing) Co., Ltd.
 * @license   Apache-2.0
 */

#ifndef BLE_TALK_NETWORK_H__
#define BLE_TALK_NETWORK_H__

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include <stdlib.h>
#include "bf0_ble_gap.h"
#undef LOG_TAG
#define LOG_TAG "ble_talk_network"
#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                             可配置宏定义                                    */
/* -------------------------------------------------------------------------- */

/** @brief 房间内同时讲话人数上限 */
#ifndef BLE_TALK_NETWORK_MAX_SPEAKERS
#define BLE_TALK_NETWORK_MAX_SPEAKERS       3
#endif

/** @brief 房间最大成员数，含 Master */
#ifndef BLE_TALK_NETWORK_MAX_ROOM_MEMBERS
#define BLE_TALK_NETWORK_MAX_ROOM_MEMBERS   8
#endif

/** @brief 配对超时时间，单位 ms*/
#ifndef BLE_TALK_NETWORK_PAIRING_TIMEOUT_MS
#define BLE_TALK_NETWORK_PAIRING_TIMEOUT_MS     (30 * 1000)
#endif

/** @brief 回连超时时间，单位 ms（可通过编译选项覆盖） */
#ifndef BLE_TALK_NETWORK_RECONNECT_TIMEOUT_MS
#define BLE_TALK_NETWORK_RECONNECT_TIMEOUT_MS   (10 * 1000)
#endif

/** @brief 广播停止延迟，单位 ms；确保接收端收到最后几帧 */
#define BLE_TALK_NETWORK_ADV_STOP_DELAY         (5000)

/** @brief 房间 ID 字符串最大长度（含 '\0'） */
#ifndef ROOM_ID_LEN
#define ROOM_ID_LEN     32
#endif

/* -------------------------------------------------------------------------- */
/*                               枚举类型                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief 设备角色
 */
typedef enum {
    BLE_TALK_NETWORK_INITIATOR_ROLE    = 0x00,  /**< Master —— 房间创建者 */
    BLE_TALK_NETWORK_PARTICIPATOR_ROLE,         /**< Slave  —— 参与者     */
} ble_talk_network_role_t;

/**
 * @brief 网络阶段状态
 */
typedef enum {
    BLE_TALK_PHASE_STANDBY      = 0,    /**< 待机       */
    BLE_TALK_PHASE_PAIRING,             /**< 配对中     */
    BLE_TALK_PHASE_WAITING_TALK,        /**< 等待对讲   */
    BLE_TALK_PHASE_TALKING,             /**< 对讲中     */
} ble_talk_phase_t;

/* -------------------------------------------------------------------------- */
/*                      平台操作接口（应用层实现并注册）                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief 底层操作钩子
 *
 * 组件在协议流程中按需调用，应用层必须实现并通过
 * @ref ble_talk_network_set_ops 注册。
 */
typedef struct {
    void    (*scan_enable)(void);    /**< 开启 BLE 扫描               */
    void    (*scan_stop)(void);      /**< 停止 BLE 扫描               */
    uint8_t (*is_speaking)(void);    /**< 查询本机是否正在发言（PTT）  */
    void    (*sender_stop)(void);    /**< 停止音频发送                 */
} ble_talk_network_ops_t;

/* -------------------------------------------------------------------------- */
/*                  事件通知回调                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 组件事件通知回调集合
 *
 * 应用层注册后仅做 UI 更新（LED / 提示音等），
 * 无需再做协议级状态管理。
 */
typedef struct {
    /**
     * @brief 网络阶段变化通知
     *
     * 典型用法：根据 @p new_phase 配合 @ref ble_talk_network_get_role()
     *           设置 LED 颜色。
     *
     * @param[in] old_phase 变化前的阶段
     * @param[in] new_phase 变化后的阶段
     */
    void (*on_phase_changed)(ble_talk_phase_t old_phase,
                             ble_talk_phase_t new_phase);

    /**
     * @brief 配对超时通知
     * @note  组件已自动回到 STANDBY，此回调仅用于 UI 提示。
     */
    void (*on_pairing_timeout)(void);

    /**
     * @brief 回连超时通知
     * @note  组件已自动回到 STANDBY，此回调仅用于 UI 提示。
     */
    void (*on_reconnect_timeout)(void);

    /**
     * @brief 房间满员通知
     * @note  本机被拒绝加入，组件已自动回到 STANDBY。
     */
    void (*on_room_full)(void);

} ble_talk_network_callbacks_t;

/* -------------------------------------------------------------------------- */
/*                            初始化与注册                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief  初始化组网组件
 * @return 0 成功
 */
int  ble_talk_network_init(void);

/**
 * @brief 初始化网络控制广播资源
 * @note  应在 BLE 上电完成后调用一次。
 */
void ble_talk_network_advertising_init(void);

/**
 * @brief 注册事件通知回调
 * @param[in] cbs  回调函数集合指针
 */
void ble_talk_network_register_callbacks(const ble_talk_network_callbacks_t *cbs);

/**
 * @brief 注册底层操作接口
 * @param[in] ops  操作函数集合指针
 */
void ble_talk_network_set_ops(const ble_talk_network_ops_t *ops);

/* -------------------------------------------------------------------------- */
/*                            BLE 事件转发                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief  在全局 BLE 事件处理器中转发事件给组网组件
 *
 * @param[in] event_id  BLE 事件 ID
 * @param[in] data      事件数据指针
 * @param[in] len       数据长度
 * @param[in] context   事件上下文
 * @return    0 成功或事件未消费
 */
int  ble_talk_network_event_handler(uint16_t event_id, uint8_t *data,
                                    uint16_t len, uint32_t context);

/* -------------------------------------------------------------------------- */
/*                            高层动作 API                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief  Master 创建房间并开始配对广播
 * @pre    phase == STANDBY && role == INITIATOR
 * @return 0 成功，-1 前置条件不满足
 */
int  ble_talk_network_create_room(void);

/**
 * @brief  Slave 开始扫描寻找可用房间
 * @pre    phase == STANDBY && role == PARTICIPATOR
 * @return 0 成功，-1 前置条件不满足
 */
int  ble_talk_network_scan_rooms(void);

/**
 * @brief  Master 确认开始对讲（向所有成员发送 SYNC）
 * @pre    phase == WAITING_TALK && role == INITIATOR
 * @return 0 成功，-1 前置条件不满足
 */
int  ble_talk_network_confirm_talking(void);

/**
 * @brief  退出当前房间
 *
 * - Slave  ：发送 ABANDON 并保留回连信息。
 * - Master ：发送 ABANDON 并解散房间。
 *
 * @pre    phase == TALKING
 * @return 0 成功，-1 前置条件不满足
 */
int  ble_talk_network_leave_room(void);

/**
 * @brief  Slave 回连上次退出的房间
 * @pre    phase == STANDBY && is_reconnect_pending == 1
 * @return 0 成功，-1 前置条件不满足
 */
int  ble_talk_network_reconnect(void);

/**
 * @brief  切换 Master / Slave 角色
 * @pre    phase == STANDBY
 * @return 切换后的角色 @ref ble_talk_network_role_t
 */
ble_talk_network_role_t ble_talk_network_switch_role(void);

/**
 * @brief 进入休眠前清理
 *
 * 停止广播/扫描，重置为 STANDBY + Slave，清除全部房间信息。
 */
void ble_talk_network_enter_idle(void);

/* -------------------------------------------------------------------------- */
/*                              状态查询                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief  获取当前网络阶段
 * @return 当前阶段 @ref ble_talk_phase_t
 */
ble_talk_phase_t        ble_talk_network_get_phase(void);

/**
 * @brief  获取当前设备角色
 * @return 当前角色 @ref ble_talk_network_role_t
 */
ble_talk_network_role_t ble_talk_network_get_role(void);

/**
 * @brief  获取当前房间 ID
 * @return 房间 ID 字符串指针；未加入房间时返回空串
 */
const char             *ble_talk_network_get_room_id(void);

/**
 * @brief  查询是否有待回连的房间
 * @retval 1 有待回连房间
 * @retval 0 无
 */
uint8_t                 ble_talk_network_is_reconnect_pending(void);

/**
 * @brief  获取当前讲话人数
 * @return 当前讲话人数（不超过 @ref BLE_TALK_NETWORK_MAX_SPEAKERS）
 */
uint8_t                 ble_talk_network_get_speaker_count(void);

/**
 * @brief  判断厂商自定义数据是否为组网控制数据
 *
 * @param[in] mfr_data  指向 Company ID 之后的数据起始
 * @param[in] len       数据长度（字节，不含 Company ID）
 * @retval    1 是组网控制数据
 * @retval    0 不是
 */
uint8_t ble_talk_network_is_control_data(const uint8_t *mfr_data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* BLE_TALK_NETWORK_H__ */

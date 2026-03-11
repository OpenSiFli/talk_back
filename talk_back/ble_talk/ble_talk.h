/**
 * @file ble_talk.h
 * @brief BLE 对讲组件 —— 音频收发公共接口声明
 *
 * 本头文件集中暴露对讲组件的公开接口，涵盖三部分：
 *   1. Talk  —— 音频编解码管线（初始化 / 反初始化 / 下行回调）
 *   2. Sender  —— 扩展广播与周期广播发送侧
 *   3. Receiver —— 扫描与周期同步接收侧
 *
 *
 * @copyright Copyright (c) 2024-2024 SiFli Technologies (Nanjing) Co., Ltd.
 * @license   Apache-2.0
 */

#ifndef BLE_TALK_API_H__
#define BLE_TALK_API_H__

#include <rtthread.h>
#include <stdint.h>
#include "bf0_ble_gap.h"
#include "audio_server.h"
#include "log.h"
#undef LOG_TAG
#define LOG_TAG "ble_talk"
#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                             可配置宏定义                                    */
/* -------------------------------------------------------------------------- */

/** @brief 周期同步超时，单位 10 ms */
#define DEFAULT_SYNC_TO             800

/** @brief 无效同步索引标识 */
#define INVALID_SYNC_IDX            0xFF

/** @brief 周期同步检测周期，单位 s */
#define DEFAULT_SYNCING_PERIOD      10

/** @brief 扫描窗口，单位 ms */
#define DEFAULT_SCAN_WIN            30

/** @brief 慢速扫描间隔，单位 ms */
#define DEFAULT_SCAN_SLOW_INTERVAL  100

/** @brief 快速扫描间隔，单位 ms */
#define DEFAULT_SCAN_FAST_INTERVAL  60

/** @brief 默认本地名称 */
#define DEFAULT_LOCAL_NAME          "SIFLI_APP"

/** @brief 示例本地名称 */
#define EXAMPLE_LOCAL_NAME          "SIFLI_EXAMPLE"

/** @brief 默认房间码（未设置 room_id 时用于广播过滤） */
#ifndef BLE_TALK_DEFAULT_NETWORK_CODE
#define BLE_TALK_DEFAULT_NETWORK_CODE   "0000"
#endif

/** @brief 麦克风常开 */
#ifndef BLE_TALK_MIC_ALWAYS_ON
#define BLE_TALK_MIC_ALWAYS_ON      1
#endif

/** @brief 周期广播负载最大长度，单位字节 */
#ifndef BLE_TALK_PERIODIC_ADV_MAX_LEN
#define BLE_TALK_PERIODIC_ADV_MAX_LEN   100
#endif

/* -------------------------------------------------------------------------- */
/*                         接收侧：枚举与数据结构                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 接收设备同步状态
 */
typedef enum {
    APP_RECV_DEV_STATE_IDLE,        /**< 空闲     */
    APP_RECV_DEV_STATE_CREATED,     /**< 已创建   */
    APP_RECV_DEV_STATE_SYNCING,     /**< 同步中   */
    APP_RECV_DEV_STATE_SYNCED,      /**< 已同步   */
} app_recv_dev_state_t;

/**
 * @brief 接收侧全局状态
 */
typedef enum {
    APP_RECV_STATE_IDLE,            /**< 空闲   */
    APP_RECV_STATE_SYNCING,         /**< 同步中 */
} app_recv_state_t;

/**
 * @brief 接收设备条目（周期同步上下文）
 */
typedef struct {
    uint8_t         sync_idx;       /**< 同步活动索引     */
    uint8_t         dev_state;      /**< @ref app_recv_dev_state_t */
    ble_gap_addr_t  addr;           /**< 远端设备地址     */
} sync_info_t;

/* -------------------------------------------------------------------------- */
/*                        Talk 音频管线接口                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief  初始化对讲音频管线
 * @param[in] flag  收发模式选择 (AUDIO_TX / AUDIO_RX / AUDIO_TXRX)
 * @return 0 成功，非 0 失败
 */
int talk_init(audio_rwflag_t flag);

/**
 * @brief  释放对讲音频管线资源
 * @return 0 成功，非 0 失败
 */
int talk_deinit(void);

/**
 * @brief  查询扬声器是否已开启
 * @retval 1 开启
 * @retval 0 关闭
 */
int talk_is_speaker_enabled(void);

/**
 * @brief  下行音频数据入口
 *
 * @param[in] actv_idx  活动索引
 * @param[in] data      音频数据指针
 * @param[in] data_len  数据长度（字节）
 */
void ble_talk_downlink(uint8_t actv_idx, uint8_t *data, uint16_t data_len);

/* -------------------------------------------------------------------------- */
/*                     Sender 侧接口（扩展 / 周期广播发送）                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化发送器模块
 * @note  需在 BLE 子系统上电就绪后调用一次。
 */
void ble_app_sender_init(void);

/**
 * @brief  查询当前是否处于语音发送工作态
 * @retval 1 发送中
 * @retval 0 空闲
 */
uint8_t ble_app_sender_is_working(void);

/**
 * @brief  开始语音发送（PTT 按下触发）
 * @return 0 成功，非 0 失败
 */
uint8_t ble_app_sender_trigger(void);

/**
 * @brief  停止语音发送（PTT 松开触发）
 * @return 0 成功，非 0 失败
 */
uint8_t ble_app_sender_stop(void);

/**
 * @brief 初始化周期广播资源
 * @note  通常在 BLE 上电完成后调用一次。
 */
void ble_app_peri_advertising_init(void);

/**
 * @brief 更新广播中的房间编码信息
 * @note  修改 room_id 后调用以使广播数据生效。
 */
void ble_app_sender_update_adv_room_code(void);

/**
 * @brief  发送一帧语音数据到广播通道（组件内部使用）
 *
 * @param[in] len         数据长度
 * @param[in] voice_data  数据指针
 * @return    0 成功，非 0 失败
 */
uint8_t app_send_voice_data(uint16_t len, uint8_t *voice_data);

/* -------------------------------------------------------------------------- */
/*               Receiver 侧接口（扫描与周期广播同步接收）                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief  初始化扩展广告扫描模块
 * @return 0 成功，非 0 失败
 */
uint8_t ble_app_scan_init(void);

/**
 * @brief  开启扩展广告扫描
 * @return 0 成功，非 0 失败
 */
uint8_t ble_app_scan_enable(void);

/**
 * @brief  停止扩展广告扫描
 * @return 0 成功，非 0 失败
 */
uint8_t ble_app_scan_stop(void);

/**
 * @brief  重启扩展广告扫描
 * @return 0 成功，非 0 失败
 */
uint8_t ble_app_scan_restart(void);

/**
 * @brief 初始化接收器（周期同步管理）
 */
void ble_app_receviver_init(void);

/**
 * @brief  在全局 BLE 事件分发处转发接收器相关事件
 *
 * @param[in] event_id  BLE 事件 ID
 * @param[in] data      事件数据指针
 * @param[in] len       数据长度
 * @param[in] context   事件上下文
 * @return    0 成功或未消费
 */
int ble_app_receiver_event_handler(uint16_t event_id, uint8_t *data,
                                   uint16_t len, uint32_t context);

/**
 * @brief  解析扩展广告中的网络 / 音频帧
 *
 * 区分组网控制数据（跳过）与音频房间标识（用于周期同步匹配）。
 *
 * @param[in] ind  扩展广告报告指针
 */
void ble_app_network_parser(ble_gap_ext_adv_report_ind_t *ind);

/**
 * @brief  查询当前已同步的周期广播设备数量
 * @return 已同步设备数
 */
uint8_t ble_app_receiver_get_synced_num(void);

/* -------------------------------------------------------------------------- */
/*                         应用层回调注册                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief 对讲组件事件回调集合
 *
 * 应用层注册后用于 UI / 灯控等通知，组件在状态变更时调用。
 */
typedef struct {
    /**
     * @brief 扫描状态变更通知
     * @param[in] is_scanning  1 = 已开始，0 = 已停止
     */
    void (*on_scan_state_changed)(uint8_t is_scanning);

    /**
     * @brief 周期同步建立后通知
     * @param[in] synced_num  当前已同步设备数量
     */
    void (*on_receiver_synced)(uint8_t synced_num);

    /**
     * @brief 周期同步断开后通知
     * @param[in] synced_num  当前已同步设备数量
     */
    void (*on_receiver_sync_stopped)(uint8_t synced_num);

} ble_talk_callbacks_t;

/**
 * @brief 注册对讲组件 UI 回调
 * @param[in] cbs  回调函数集合指针
 */
void ble_talk_register_callbacks(const ble_talk_callbacks_t *cbs);

#ifdef __cplusplus
}
#endif

#endif /* BLE_TALK_API_H__ */

/*
 * BLE Talk Network Driver - 组网驱动实现
 *
 * 本文件包含全部协议逻辑：房间创建/加入/退出/解散、成员管理、
 * 超时定时器、广播控制以及高层 API 实现。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "ble_talk_network.h"
#include "bf0_ble_gap.h"
#include "bf0_sibles.h"
#include "bf0_sibles_internal.h"
#include "bf0_sibles_advertising.h"
#include "log.h"

/* ==================== 内部常量与宏 ==================== */

#define BLE_TALK_NETWORK_MAX_KEY_LEN   (6)
#define BLE_TALK_NETWORK_MAX_NAME_LEN  (64)
#define BLE_TALK_NETWORK_MAGIC         (0x85A7C3D4)

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* ==================== 内部枚举 ==================== */

typedef enum
{
    BLE_TALK_NETWORK_START_CMD = 0x00,
    BLE_TALK_NETWORK_JOIN_CMD,
    BLE_TALK_NETWORK_FINISH_CMD,
    BLE_TALK_NETWORK_ABANDON_CMD,
    BLE_TALK_NETWORK_ON_LINE_CMD,
    BLE_TALK_NETWORK_SYNC_CMD,
    BLE_TALK_NETWORK_REJOIN_ACK_CMD,
    BLE_TALK_NETWORK_ROOM_FULL_CMD,
} ble_talk_network_cmd_t;

typedef enum
{
    BLE_TALK_NETWORK_ADV_IDLE,
    BLE_TALK_NETWORK_ADV_STARTING,
    BLE_TALK_NETWORK_ADV_STARTED,
    BLE_TALK_NETWORK_ADV_STOPING,
} ble_talk_network_adv_state_t;

typedef enum
{
    BLE_TALK_NETWORK_WORK_IDLE,
    BLE_TALK_NETWORK_WORK_NETWORKING,
} ble_talk_network_work_state_t;

/* ==================== 内部数据结构 ==================== */

#pragma pack(push, 4)
typedef struct
{
    uint8_t key_value[BLE_TALK_NETWORK_MAX_KEY_LEN];
} ble_talk_network_key_t;

typedef struct
{
    ble_talk_network_key_t key;
    uint16_t name_len;
    uint8_t *name;
} ble_talk_network_start_t;

typedef struct
{
    ble_talk_network_key_t key;
    uint16_t name_len;
    uint8_t *name;
    uint16_t room_len;
    uint8_t *room;
} ble_talk_network_join_t;

typedef struct
{
    ble_talk_network_key_t key;
    uint16_t member_len;
    bd_addr_t *member;
} ble_talk_network_finish_t;

typedef struct
{
    ble_talk_network_key_t key;
} ble_talk_network_sync_t;

typedef struct
{
    ble_talk_network_key_t key;
    uint8_t speaker_cnt;
} ble_talk_network_rejoin_ack_t;

typedef struct
{
    ble_talk_network_key_t key;
    ble_talk_network_role_t role;
} ble_talk_network_abandon_t;

typedef struct
{
    ble_talk_network_key_t key;
    bd_addr_t rejected_addr;
} ble_talk_network_room_full_t;

typedef struct
{
    struct rt_mutex lock;
    struct rt_delayed_work stop_work;           /* 延迟停止广播 */
    struct rt_delayed_work pairing_timeout;     /* 配对超时 */
    struct rt_delayed_work reconnect_timeout;   /* 回连超时 */
    ble_talk_network_adv_state_t adv_state;
    ble_talk_network_work_state_t work_state;
    ble_talk_network_key_t key;
    uint16_t len;
    uint8_t *adv_data;
    bd_addr_t reconnect_allow_list[8];
    uint8_t reconnect_count;
    ble_talk_phase_t phase;
    ble_talk_network_role_t device_role;
    uint8_t speaker_cnt;
    uint8_t is_reconnect_pending;
    uint8_t pairing_has_target;
    char room_id[ROOM_ID_LEN];
    char last_room_id[ROOM_ID_LEN];
} ble_talk_network_env_t;

typedef struct
{
    uint32_t magic;
    uint8_t cmd;
    uint8_t role;
    uint16_t len;
    uint8_t *payload;
} ble_talk_network_data_t;

typedef struct
{
    bd_addr_t dev_addr;
    ble_talk_network_role_t role;
    uint16_t name_len;
    uint8_t name[BLE_TALK_NETWORK_MAX_NAME_LEN + 1];
    uint8_t reconnect_allowed;
    rt_list_t node;
} ble_talk_nework_member_node_t;

typedef struct
{
    ble_talk_network_key_t key;
    rt_list_t node;
    rt_list_t member_list;
} ble_talk_nework_group_node_t;
#pragma pack(pop)

/* ==================== 静态变量 ==================== */

SIBLES_ADVERTISING_CONTEXT_DECLAR(ble_talk_network_context);

static rt_list_t ble_talk_group_list;
static ble_talk_network_env_t network_env = {0};

static ble_talk_network_callbacks_t g_callbacks = {0};
static ble_talk_network_ops_t g_ops = {0};

static ble_talk_network_key_t g_current_token = {0};
static bool g_token_initialized = false;

/* ==================== 内部前向声明 ==================== */

static uint8_t ble_talk_network_advertising_event(uint8_t event, void *context, void *data);
static void set_phase(ble_talk_phase_t new_phase);
static void internal_cleanup_to_standby(void);

/* ==================== Key / Token 工具 ==================== */

static void build_key_from_room(const char *room_id, ble_talk_network_key_t *out_key)
{
    if (!out_key) return;
    memset(out_key, 0, sizeof(ble_talk_network_key_t));
    if (!room_id) return;
    size_t len = strlen(room_id);
    if (len == 0) return;
    size_t maxk = BLE_TALK_NETWORK_MAX_KEY_LEN;
    size_t copy_len = (len >= maxk) ? maxk : len;
    const char *src = room_id + ((len > maxk) ? (len - maxk) : 0);
    memcpy(out_key->key_value, src, copy_len);
}

static bool token_matches(ble_talk_network_key_t *key)
{
    if (!g_token_initialized) return false;
    return memcmp(&g_current_token, key, sizeof(ble_talk_network_key_t)) == 0;
}

static void set_token(ble_talk_network_key_t *key)
{
    if (key)
    {
        memcpy(&g_current_token, key, sizeof(ble_talk_network_key_t));
        g_token_initialized = true;
    }
    else
    {
        memset(&g_current_token, 0, sizeof(ble_talk_network_key_t));
        g_token_initialized = false;
    }
}

/* ==================== 广播数据管理 ==================== */

static void ble_talk_update_adv_data(sibles_advertising_context_t *context, uint8_t *data, uint16_t len)
{
    sibles_advertising_para_t para = {0};
    char local_name[31] = {0};
    uint16_t manu_company_id = SIG_SIFLI_COMPANY_ID;
    bd_addr_t addr;
    int ret = ble_get_public_address(&addr);
    if (ret == HL_ERR_NO_ERROR)
        rt_snprintf(local_name, 31, "SIFLI_APP-%x-%x-%x-%x-%x-%x",
                    addr.addr[0], addr.addr[1], addr.addr[2],
                    addr.addr[3], addr.addr[4], addr.addr[5]);
    else
        memcpy(local_name, "SIFLI_APP", 9);

    para.adv_data.completed_name = rt_malloc(rt_strlen(local_name) + sizeof(sibles_adv_type_name_t));
    para.adv_data.completed_name->name_len = rt_strlen(local_name);
    rt_memcpy(para.adv_data.completed_name->name, local_name, para.adv_data.completed_name->name_len);

    para.adv_data.manufacturer_data = rt_malloc(sizeof(sibles_adv_type_manufacturer_data_t) + len);
    para.adv_data.manufacturer_data->company_id = manu_company_id;
    para.adv_data.manufacturer_data->data_len = len;
    rt_memcpy(para.adv_data.manufacturer_data->additional_data, data, len);

    ret = sibles_advertising_update_adv_and_scan_rsp_data(context, &para.adv_data, NULL);

    rt_free(para.adv_data.completed_name);
    rt_free(para.adv_data.manufacturer_data);
}

static uint8_t *ble_talk_adv_data_found(uint8_t *p_data, uint8_t type, uint16_t *length)
{
    if (!p_data || !length)
        return NULL;

    uint8_t *p_cursor = p_data;
    uint8_t *p_end_cursor = p_data + *length;

    while (p_cursor < p_end_cursor)
    {
        uint8_t ad_type = *(p_cursor + 1);
        if (ad_type == type)
        {
            *length = *p_cursor - 1;
            break;
        }
        p_cursor += (*p_cursor + 1);
    }

    return (p_cursor >= p_end_cursor) ? NULL : (p_cursor + 2);
}

/* ==================== 广播启停 ==================== */

static void ble_talk_network_adv_work_handle(struct rt_work *work, void *work_data)
{
    (void)work_data;
    sibles_advertising_stop(ble_talk_network_context);
}

static void adv_start(void)
{
    if (BLE_TALK_NETWORK_ADV_IDLE == network_env.adv_state)
    {
        network_env.adv_state = BLE_TALK_NETWORK_ADV_STARTING;
        sibles_advertising_start(ble_talk_network_context);
    }
    else if (BLE_TALK_NETWORK_ADV_STOPING == network_env.adv_state)
    {
        rt_work_cancel(&network_env.stop_work.work);
        network_env.adv_state = BLE_TALK_NETWORK_ADV_STARTED;
        ble_talk_update_adv_data(ble_talk_network_context, network_env.adv_data, network_env.len);
    }
}

static void adv_stop(uint32_t delay)
{
    if ((BLE_TALK_NETWORK_ADV_STARTED == network_env.adv_state) ||
        (BLE_TALK_NETWORK_ADV_STARTING == network_env.adv_state))
    {
        network_env.adv_state = BLE_TALK_NETWORK_ADV_STOPING;
        rt_work_submit(&network_env.stop_work.work, delay);
    }
    else if (BLE_TALK_NETWORK_ADV_STOPING == network_env.adv_state)
    {
        rt_work_cancel(&network_env.stop_work.work);
        rt_work_submit(&network_env.stop_work.work, delay);
    }
}

static uint8_t ble_talk_network_advertising_event(uint8_t event, void *context, void *data)
{
    if (ble_talk_network_context != context) return 0;
    switch (event)
    {
    case SIBLES_ADV_EVT_ADV_STARTED:
    {
        sibles_adv_evt_startted_t *evt = (sibles_adv_evt_startted_t *)data;
        if (evt->status == HL_ERR_NO_ERROR)
        {
            network_env.adv_state = BLE_TALK_NETWORK_ADV_STARTED;
            ble_talk_update_adv_data(ble_talk_network_context, network_env.adv_data, network_env.len);
        }
        else
        {
            network_env.adv_state = BLE_TALK_NETWORK_ADV_IDLE;
        }
        break;
    }
    case SIBLES_ADV_EVT_ADV_STOPPED:
    {
        sibles_adv_evt_stopped_t *evt = (sibles_adv_evt_stopped_t *)data;
        if (evt->reason == HL_ERR_NO_ERROR)
            network_env.adv_state = BLE_TALK_NETWORK_ADV_IDLE;
        break;
    }
    default:
        break;
    }
    return 0;
}

/* ==================== 广播数据更新 ==================== */

static void env_data_update(uint8_t *data, uint16_t len)
{
    uint8_t *new_data = NULL;
    if (!network_env.adv_data)
        new_data = rt_malloc(len);
    else
        new_data = rt_realloc(network_env.adv_data, len);
    if (!new_data) return;
    network_env.adv_data = new_data;
    network_env.len = len;
    rt_memcpy(network_env.adv_data, data, len);

    if (BLE_TALK_NETWORK_ADV_IDLE == network_env.adv_state ||
        BLE_TALK_NETWORK_ADV_STOPING == network_env.adv_state)
    {
        adv_start();
    }
    else if (BLE_TALK_NETWORK_ADV_STARTED == network_env.adv_state)
    {
        ble_talk_update_adv_data(ble_talk_network_context, network_env.adv_data, network_env.len);
    }
}

/* ==================== 消息构建/释放 ==================== */

static ble_talk_network_data_t *fill_msg(uint8_t cmd, uint8_t role,
                                         uint8_t *payload, uint16_t data_len)
{
    ble_talk_network_data_t *msg = (ble_talk_network_data_t *)rt_malloc(
        sizeof(ble_talk_network_data_t) + data_len);
    RT_ASSERT(msg);
    memset(msg, 0, sizeof(ble_talk_network_data_t) + data_len);
    msg->magic = BLE_TALK_NETWORK_MAGIC;
    msg->cmd = cmd;
    msg->role = role;
    msg->len = data_len;
    msg->payload = (uint8_t *)((uint8_t *)msg + sizeof(ble_talk_network_data_t));
    return msg;
}

static void rel_msg(ble_talk_network_data_t *msg)
{
    if (msg) rt_free(msg);
}

/* ==================== Ops 调用封装 ==================== */

static void ops_scan_enable(void)
{
    if (g_ops.scan_enable) g_ops.scan_enable();
}

static void ops_scan_stop(void)
{
    if (g_ops.scan_stop) g_ops.scan_stop();
}

static void ops_sender_stop(void)
{
    if (g_ops.sender_stop) g_ops.sender_stop();
}

static uint8_t ops_is_speaking(void)
{
    if (g_ops.is_speaking) return g_ops.is_speaking();
    return 0;
}

/* ==================== 阶段切换（触发回调） ==================== */

static void set_phase(ble_talk_phase_t new_phase)
{
    ble_talk_phase_t old = network_env.phase;
    if (old == new_phase) return;
    network_env.phase = new_phase;
    if (g_callbacks.on_phase_changed)
        g_callbacks.on_phase_changed(old, new_phase);
}

/* ==================== 成员/分组管理 ==================== */

static ble_talk_nework_group_node_t *group_find(ble_talk_network_key_t *key)
{
    ble_talk_nework_group_node_t *group_node = NULL;
    rt_list_t *next, *pos;
    rt_mutex_take(&network_env.lock, RT_WAITING_FOREVER);
    rt_list_for_each_safe(pos, next, &ble_talk_group_list)
    {
        group_node = rt_list_entry(pos, ble_talk_nework_group_node_t, node);
        RT_ASSERT(group_node);
        if (!memcmp(group_node->key.key_value, key->key_value, BLE_TALK_NETWORK_MAX_KEY_LEN))
        {
            rt_mutex_release(&network_env.lock);
            return group_node;
        }
    }
    rt_mutex_release(&network_env.lock);
    return NULL;
}

static ble_talk_nework_member_node_t *member_find(ble_talk_nework_group_node_t *group, bd_addr_t *addr)
{
    ble_talk_nework_member_node_t *member = NULL;
    rt_list_t *next, *pos;
    rt_mutex_take(&network_env.lock, RT_WAITING_FOREVER);
    rt_list_for_each_safe(pos, next, &group->member_list)
    {
        member = rt_list_entry(pos, ble_talk_nework_member_node_t, node);
        RT_ASSERT(member);
        if (!memcmp(member->dev_addr.addr, addr->addr, BD_ADDR_LEN))
        {
            rt_mutex_release(&network_env.lock);
            return member;
        }
    }
    rt_mutex_release(&network_env.lock);
    return NULL;
}

static int member_add(ble_talk_network_key_t *key, bd_addr_t *addr,
                      ble_talk_network_role_t role, uint8_t *name, uint16_t name_len)
{
    if (BLE_TALK_NETWORK_WORK_NETWORKING != network_env.work_state) return 1;
    rt_mutex_take(&network_env.lock, RT_WAITING_FOREVER);
    ble_talk_nework_group_node_t *group = group_find(key);
    if (!group)
    {
        group = (ble_talk_nework_group_node_t *)rt_malloc(sizeof(ble_talk_nework_group_node_t));
        RT_ASSERT(group);
        rt_memset(group, 0, sizeof(ble_talk_nework_group_node_t));
        group->key = *key;
        rt_list_init(&group->node);
        rt_list_init(&group->member_list);
        rt_list_insert_after(&ble_talk_group_list, &group->node);
    }
    ble_talk_nework_member_node_t *member = member_find(group, addr);
    if (!member)
    {
        member = (ble_talk_nework_member_node_t *)rt_malloc(sizeof(ble_talk_nework_member_node_t));
        RT_ASSERT(member);
        rt_memset(member, 0, sizeof(ble_talk_nework_member_node_t));
        rt_list_init(&member->node);
        member->role = role;
        member->dev_addr = *addr;
        member->name_len = MIN(name_len, BLE_TALK_NETWORK_MAX_NAME_LEN);
        if (name_len) memcpy(member->name, name, member->name_len);
        rt_list_insert_after(&group->member_list, &member->node);
    }
    else
    {
        member->role = role;
        member->name_len = MIN(name_len, BLE_TALK_NETWORK_MAX_NAME_LEN);
        if (name_len) memcpy(member->name, name, member->name_len);
    }
    rt_mutex_release(&network_env.lock);
    return 0;
}

static void member_delete_all(ble_talk_nework_group_node_t *group)
{
    rt_list_t *next, *pos;
    rt_mutex_take(&network_env.lock, RT_WAITING_FOREVER);
    rt_list_for_each_safe(pos, next, &group->member_list)
    {
        ble_talk_nework_member_node_t *m = rt_list_entry(pos, ble_talk_nework_member_node_t, node);
        RT_ASSERT(m);
        rt_list_remove(&m->node);
        rt_free(m);
    }
    rt_mutex_release(&network_env.lock);
}

static int group_delete(ble_talk_network_key_t *key)
{
    rt_list_t *iter, *next;
    rt_mutex_take(&network_env.lock, RT_WAITING_FOREVER);
    rt_list_for_each_safe(iter, next, &ble_talk_group_list)
    {
        ble_talk_nework_group_node_t *g = rt_list_entry(iter, ble_talk_nework_group_node_t, node);
        if (!memcmp(g->key.key_value, key->key_value, BLE_TALK_NETWORK_MAX_KEY_LEN))
        {
            member_delete_all(g);
            rt_list_remove(&g->node);
            rt_free(g);
            rt_mutex_release(&network_env.lock);
            return 0;
        }
    }
    rt_mutex_release(&network_env.lock);
    return 1;
}

static int member_delete(ble_talk_nework_group_node_t *group, bd_addr_t *addr)
{
    rt_list_t *next, *pos;
    rt_mutex_take(&network_env.lock, RT_WAITING_FOREVER);
    rt_list_for_each_safe(pos, next, &group->member_list)
    {
        ble_talk_nework_member_node_t *m = rt_list_entry(pos, ble_talk_nework_member_node_t, node);
        if (!memcmp(m->dev_addr.addr, addr->addr, BD_ADDR_LEN))
        {
            rt_list_remove(&m->node);
            rt_free(m);
            rt_mutex_release(&network_env.lock);
            return 0;
        }
    }
    rt_mutex_release(&network_env.lock);
    return 1;
}

static bool group_only_master(ble_talk_nework_group_node_t *group)
{
    bool only = false;
    bd_addr_t master_addr;
    ble_get_public_address(&master_addr);
    rt_mutex_take(&network_env.lock, RT_WAITING_FOREVER);
    if (rt_list_len(&group->member_list) == 1)
    {
        rt_list_t *pos = group->member_list.next;
        ble_talk_nework_member_node_t *n = rt_list_entry(pos, ble_talk_nework_member_node_t, node);
        if (n) only = (memcmp(n->dev_addr.addr, master_addr.addr, BD_ADDR_LEN) == 0);
    }
    rt_mutex_release(&network_env.lock);
    return only;
}

static uint32_t group_member_count(ble_talk_network_key_t *key)
{
    ble_talk_nework_group_node_t *g = group_find(key);
    if (!g) return 0;
    rt_mutex_take(&network_env.lock, RT_WAITING_FOREVER);
    uint32_t cnt = rt_list_len(&g->member_list);
    rt_mutex_release(&network_env.lock);
    return cnt;
}

static void network_deinit(void)
{
    if (network_env.adv_data)
    {
        rt_free(network_env.adv_data);
        network_env.adv_data = NULL;
    }
    network_env.work_state = BLE_TALK_NETWORK_WORK_IDLE;
}

/* ==================== 协议消息发送 ==================== */

/* 发送 START 广播（Master 创建房间） */
static void send_start(ble_talk_network_start_t *info)
{
    bd_addr_t addr;
    uint8_t speaker_cnt = network_env.speaker_cnt;
    if (ops_is_speaking()) speaker_cnt++;
    if (speaker_cnt > BLE_TALK_NETWORK_MAX_SPEAKERS)
        speaker_cnt = BLE_TALK_NETWORK_MAX_SPEAKERS;

    uint16_t base_len = sizeof(ble_talk_network_data_t) + sizeof(ble_talk_network_start_t);
    uint16_t network_data_len = base_len + info->name_len + 1;
    ble_talk_network_data_t *msg = fill_msg(
        BLE_TALK_NETWORK_START_CMD,
        BLE_TALK_NETWORK_INITIATOR_ROLE,
        (uint8_t *)info,
        sizeof(ble_talk_network_start_t) + info->name_len);
    if (!msg) return;

    memcpy(msg->payload, &info->key, sizeof(info->key));
    memcpy(msg->payload + sizeof(info->key), &info->name_len, sizeof(info->name_len));
    rt_memset(msg->payload + sizeof(ble_talk_network_key_t) + sizeof(uint16_t), 0, sizeof(uint8_t *));
    if (info->name_len > 0 && info->name != NULL)
        memcpy(msg->payload + sizeof(ble_talk_network_start_t), info->name, info->name_len);
    *(uint8_t *)(msg->payload + sizeof(ble_talk_network_start_t) + info->name_len) = speaker_cnt;

    group_delete(&info->key);
    ble_get_public_address(&addr);
    network_env.work_state = BLE_TALK_NETWORK_WORK_NETWORKING;
    network_env.key = info->key;
    set_token(&info->key);
    member_add(&info->key, &addr, BLE_TALK_NETWORK_INITIATOR_ROLE, info->name, info->name_len);
    ops_scan_enable();
    env_data_update((uint8_t *)msg, network_data_len);
    rel_msg(msg);
}

/* 发送 JOIN 请求 */
static void send_join(const ble_talk_network_key_t *key, const char *name)
{
    if (!key || !name) return;
    uint16_t name_len = (uint16_t)rt_strnlen(name, BLE_TALK_NETWORK_MAX_NAME_LEN);
    uint16_t room_len = (uint16_t)strlen(network_env.room_id);
    if (room_len > ROOM_ID_LEN) room_len = ROOM_ID_LEN;
    uint16_t join_payload_len = sizeof(ble_talk_network_key_t) + sizeof(uint16_t)
                                + name_len + sizeof(uint16_t) + room_len;
    uint16_t network_data_len = sizeof(ble_talk_network_data_t) + join_payload_len;
    ble_talk_network_data_t *msg = fill_msg(
        BLE_TALK_NETWORK_JOIN_CMD,
        BLE_TALK_NETWORK_PARTICIPATOR_ROLE,
        NULL, join_payload_len);
    if (!msg) return;

    uint8_t *p = msg->payload;
    memcpy(p, key, sizeof(ble_talk_network_key_t)); p += sizeof(ble_talk_network_key_t);
    memcpy(p, &name_len, sizeof(uint16_t));         p += sizeof(uint16_t);
    memcpy(p, name, name_len);                       p += name_len;
    memcpy(p, &room_len, sizeof(uint16_t));          p += sizeof(uint16_t);
    memcpy(p, network_env.room_id, room_len);

    env_data_update((uint8_t *)msg, network_data_len);
    rel_msg(msg);
}

/* 发送 SYNC（Master 确认开始对讲） */
static void send_sync(ble_talk_network_key_t *key)
{
    if (!key) return;
    ble_talk_network_sync_t payload;
    rt_memset(&payload, 0, sizeof(payload));
    rt_memcpy(&payload.key, key, sizeof(ble_talk_network_key_t));
    uint16_t network_data_len = sizeof(ble_talk_network_data_t) + sizeof(payload);
    ble_talk_network_data_t *msg = fill_msg(
        BLE_TALK_NETWORK_SYNC_CMD,
        BLE_TALK_NETWORK_INITIATOR_ROLE,
        (uint8_t *)&payload, sizeof(payload));
    if (!msg) return;
    rt_memcpy(msg->payload, &payload, sizeof(payload));
    ops_scan_enable();
    env_data_update((uint8_t *)msg, network_data_len);
    rel_msg(msg);
}

/* 发送 ABANDON */
static void send_abandon(ble_talk_network_abandon_t *info, ble_talk_network_role_t role)
{
    uint16_t payload_len = sizeof(ble_talk_network_abandon_t);
    uint16_t network_data_len = sizeof(ble_talk_network_data_t) + payload_len;
    ble_talk_network_data_t *msg = fill_msg(
        BLE_TALK_NETWORK_ABANDON_CMD, role,
        (uint8_t *)info, payload_len);
    if (msg)
    {
        memcpy(msg->payload, info, sizeof(ble_talk_network_abandon_t));
        env_data_update((uint8_t *)msg, network_data_len);
        rel_msg(msg);
    }
}

/* 发送 REJOIN_ACK */
static void send_rejoin_ack(const ble_talk_network_key_t *key, uint8_t speaker_cnt)
{
    if (!key) return;
    ble_talk_network_rejoin_ack_t payload;
    rt_memset(&payload, 0, sizeof(payload));
    rt_memcpy(&payload.key, key, sizeof(ble_talk_network_key_t));
    payload.speaker_cnt = speaker_cnt;
    uint16_t payload_len = sizeof(payload);
    uint16_t network_data_len = sizeof(ble_talk_network_data_t) + payload_len;
    ble_talk_network_data_t *msg = fill_msg(
        BLE_TALK_NETWORK_REJOIN_ACK_CMD,
        BLE_TALK_NETWORK_INITIATOR_ROLE,
        (uint8_t *)&payload, payload_len);
    if (!msg) return;
    rt_memcpy(msg->payload, &payload, payload_len);
    env_data_update((uint8_t *)msg, network_data_len);
    rel_msg(msg);
}

/* 发送 ROOM_FULL 通知 */
static void send_room_full(const ble_talk_network_key_t *key, const bd_addr_t *rejected_addr)
{
    if (!key || !rejected_addr) return;
    ble_talk_network_room_full_t payload;
    rt_memset(&payload, 0, sizeof(payload));
    rt_memcpy(&payload.key, key, sizeof(ble_talk_network_key_t));
    rt_memcpy(&payload.rejected_addr, rejected_addr, sizeof(bd_addr_t));
    uint16_t payload_len = sizeof(payload);
    uint16_t network_data_len = sizeof(ble_talk_network_data_t) + payload_len;
    ble_talk_network_data_t *msg = fill_msg(
        BLE_TALK_NETWORK_ROOM_FULL_CMD,
        BLE_TALK_NETWORK_INITIATOR_ROLE,
        (uint8_t *)&payload, payload_len);
    if (!msg) return;
    rt_memcpy(msg->payload, &payload, payload_len);
    env_data_update((uint8_t *)msg, network_data_len);
    rel_msg(msg);
}

/* 生成本机 JOIN 用名称 */
static void get_local_join_name(char *buf, size_t buf_len)
{
    bd_addr_t addr;
    ble_get_public_address(&addr);
    snprintf(buf, buf_len, "SLAVE_%02x%02x", addr.addr[0], addr.addr[1]);
}

/* ==================== 统一清理到待机 ==================== */

static void internal_cleanup_to_standby(void)
{
    ops_sender_stop();
    ops_scan_stop();
    adv_stop(0);
    memset(network_env.room_id, 0, sizeof(network_env.room_id));
    set_token(NULL);
    network_env.pairing_has_target = 0;
    set_phase(BLE_TALK_PHASE_STANDBY);
}

/* ==================== 超时处理（内部） ==================== */

static void pairing_timeout_handler(struct rt_work *work, void *work_data)
{
    (void)work;
    (void)work_data;
    if (network_env.phase != BLE_TALK_PHASE_PAIRING) return;

    LOG_D("Pairing timeout. Returning to standby mode.");
    ops_scan_stop();

    if (network_env.device_role == BLE_TALK_NETWORK_INITIATOR_ROLE &&
        strlen(network_env.room_id) > 0)
    {
        /* Master 超时：发送 ABANDON 解散房间 */
        ble_talk_network_key_t key;
        build_key_from_room(network_env.room_id, &key);
        ble_talk_network_abandon_t abn;
        memcpy(&abn.key, &key, sizeof(ble_talk_network_key_t));
        send_abandon(&abn, BLE_TALK_NETWORK_INITIATOR_ROLE);
    }

    memset(network_env.room_id, 0, sizeof(network_env.room_id));
    set_token(NULL);
    network_env.pairing_has_target = 0;
    set_phase(BLE_TALK_PHASE_STANDBY);

    if (g_callbacks.on_pairing_timeout)
        g_callbacks.on_pairing_timeout();
}

static void reconnect_timeout_handler(struct rt_work *work, void *work_data)
{
    (void)work;
    (void)work_data;
    if (network_env.phase != BLE_TALK_PHASE_WAITING_TALK ||
        !network_env.is_reconnect_pending)
        return;

    LOG_D("Reconnect timeout. Returning to standby mode.");
    ops_scan_stop();
    adv_stop(0);
    memset(network_env.room_id, 0, sizeof(network_env.room_id));
    set_token(NULL);
    network_env.pairing_has_target = 0;
    set_phase(BLE_TALK_PHASE_STANDBY);

    if (g_callbacks.on_reconnect_timeout)
        g_callbacks.on_reconnect_timeout();
}

/* ==================== 协议事件处理 ==================== */

static void handle_start_cmd(ble_gap_ext_adv_report_ind_t *ind,
                             ble_talk_network_data_t *nd)
{
    ble_talk_network_start_t *start = (ble_talk_network_start_t *)nd->payload;

    /* 仅 Slave 在配对/回连待机阶段处理 */
    int is_pairing = (network_env.device_role == BLE_TALK_NETWORK_PARTICIPATOR_ROLE &&
                      network_env.phase == BLE_TALK_PHASE_PAIRING);
    int is_reconnect = (network_env.device_role == BLE_TALK_NETWORK_PARTICIPATOR_ROLE &&
                        network_env.is_reconnect_pending &&
                        network_env.phase == BLE_TALK_PHASE_STANDBY);
    if (!is_pairing && !is_reconnect) return;

    uint8_t *name_src = nd->payload + sizeof(ble_talk_network_start_t);

    if (is_pairing)
    {
        if (!network_env.pairing_has_target)
        {
            uint16_t cpy = start->name_len;
            if (cpy >= ROOM_ID_LEN) cpy = ROOM_ID_LEN - 1;
            rt_memset(network_env.room_id, 0, ROOM_ID_LEN);
            rt_memcpy(network_env.room_id, name_src, cpy);
            /* 同步保存到 last_room_id */
            strncpy(network_env.last_room_id, network_env.room_id, ROOM_ID_LEN - 1);
            network_env.last_room_id[ROOM_ID_LEN - 1] = '\0';
            network_env.pairing_has_target = 1;
        }
        else
        {
            uint16_t cur_len = (uint16_t)strlen(network_env.room_id);
            if (!(cur_len == start->name_len &&
                  memcmp(network_env.room_id, name_src, cur_len) == 0))
                return;
        }
    }

    LOG_D("Slave: received create-room broadcast, attempting to join");
    set_token(&start->key);
    network_env.work_state = BLE_TALK_NETWORK_WORK_NETWORKING;

    /* 解析 speaker_cnt */
    uint16_t expect_min = sizeof(ble_talk_network_start_t) + start->name_len;
    if (nd->len >= expect_min + 1)
    {
        uint8_t sc = *(uint8_t *)(nd->payload + expect_min);
        network_env.speaker_cnt = MIN(sc, BLE_TALK_NETWORK_MAX_SPEAKERS);
    }

    /* 临时分配 name 用于 member_add */
    start->name = (uint8_t *)rt_malloc(start->name_len + 1);
    if (!start->name) return;
    rt_memset(start->name, 0, start->name_len + 1);
    memcpy(start->name, name_src, start->name_len);

    if (0 == member_add(&start->key, &ind->addr.addr, nd->role,
                        start->name, start->name_len))
    {
        /* 自动发送 JOIN */
        char local_name[32];
        get_local_join_name(local_name, sizeof(local_name));
        send_join(&start->key, local_name);
        set_phase(BLE_TALK_PHASE_WAITING_TALK);
    }

    rt_free(start->name);
}

static void handle_join_cmd(ble_gap_ext_adv_report_ind_t *ind,
                            ble_talk_network_data_t *nd)
{
    if (network_env.device_role != BLE_TALK_NETWORK_INITIATOR_ROLE) return;

    ble_talk_network_key_t *key = (ble_talk_network_key_t *)nd->payload;

    /* 对讲中：只允许白名单设备回连 */
    if (network_env.phase == BLE_TALK_PHASE_TALKING)
    {
        bd_addr_t join_addr;
        memset(&join_addr, 0, sizeof(join_addr));
        memcpy(join_addr.addr, ind->addr.addr.addr, BD_ADDR_LEN);
        int allowed = 0;
        for (uint8_t i = 0; i < network_env.reconnect_count; i++)
        {
            if (memcmp(network_env.reconnect_allow_list[i].addr,
                       join_addr.addr, BD_ADDR_LEN) == 0)
            {
                allowed = 1;
                network_env.reconnect_count--;
                break;
            }
        }
        if (!allowed) return;
    }

    /* 各种验证 */
    ble_talk_nework_group_node_t *group = group_find(key);
    if (group)
    {
        ble_talk_nework_member_node_t *existing = member_find(group, &ind->addr.addr);
        if (existing) return;
    }
    if (nd->len < sizeof(ble_talk_network_key_t) + sizeof(uint16_t)) return;
    if (!token_matches(key)) return;

    uint16_t offset = sizeof(ble_talk_network_key_t);
    uint16_t name_len = *(uint16_t *)(nd->payload + offset);
    offset += sizeof(uint16_t);
    if (nd->len < offset + name_len) return;
    uint8_t *name_ptr = nd->payload + offset;
    offset += name_len;

    uint16_t room_len = 0;
    uint8_t *room_ptr = NULL;
    if (nd->len >= offset + sizeof(uint16_t))
    {
        room_len = *(uint16_t *)(nd->payload + offset);
        offset += sizeof(uint16_t);
        if (nd->len >= offset + room_len)
            room_ptr = nd->payload + offset;
    }

    /* 房间 ID 校验 */
    if (room_ptr)
    {
        uint8_t my_len = (uint8_t)strlen(network_env.room_id);
        if (!(room_len == my_len &&
              rt_memcmp(room_ptr, network_env.room_id, my_len) == 0))
            return;
    }

    /* 房间容量检查 */
    uint32_t member_cnt = group_member_count(key);
    if (member_cnt >= BLE_TALK_NETWORK_MAX_ROOM_MEMBERS)
    {
        LOG_D("The room is full(%d), refuse to join", (int)BLE_TALK_NETWORK_MAX_ROOM_MEMBERS);
        send_room_full(key, &ind->addr.addr);
        return;
    }

    /* 添加成员 */
    uint8_t *name_copy = (uint8_t *)rt_malloc(name_len + 1);
    if (!name_copy) return;
    rt_memset(name_copy, 0, name_len + 1);
    memcpy(name_copy, name_ptr, name_len);

    if (0 == member_add(key, &ind->addr.addr, nd->role, name_copy, name_len))
    {
        LOG_D("The slave device has been successfully added.: %.*s", name_len, name_copy);
        if (network_env.phase == BLE_TALK_PHASE_PAIRING)
        {
            set_phase(BLE_TALK_PHASE_WAITING_TALK);
        }
        if (network_env.phase == BLE_TALK_PHASE_TALKING)
        {
            /* 对讲中回连加入，发送 REJOIN_ACK */
            uint8_t sc = network_env.speaker_cnt;
            if (ops_is_speaking()) sc++;
            if (sc > BLE_TALK_NETWORK_MAX_SPEAKERS) sc = BLE_TALK_NETWORK_MAX_SPEAKERS;
            send_rejoin_ack(key, sc);
            adv_stop(BLE_TALK_NETWORK_ADV_STOP_DELAY);
        }
    }
    rt_free(name_copy);
}

static void handle_sync_cmd(ble_gap_ext_adv_report_ind_t *ind,
                            ble_talk_network_data_t *nd)
{
    if (network_env.phase != BLE_TALK_PHASE_WAITING_TALK) return;
    ble_talk_network_sync_t *sync = (ble_talk_network_sync_t *)nd->payload;
    if (!token_matches(&sync->key)) return;

    adv_stop(BLE_TALK_NETWORK_ADV_STOP_DELAY);
    set_phase(BLE_TALK_PHASE_TALKING);
}

static void handle_room_full_cmd(ble_gap_ext_adv_report_ind_t *ind,
                                 ble_talk_network_data_t *nd)
{
    ble_talk_network_room_full_t *info = (ble_talk_network_room_full_t *)nd->payload;
    if (!token_matches(&info->key)) return;

    /* 自检：只有被拒绝的设备才需要处理 */
    bd_addr_t my_addr;
    ble_get_public_address(&my_addr);
    if (memcmp(&info->rejected_addr, &my_addr, sizeof(bd_addr_t)) != 0)
    {
        LOG_D("Received full room broadcast, but not self, ignore");
        return;
    }
    LOG_D("Room is full, returning to standby mode");
    ops_sender_stop();
    ops_scan_stop();
    adv_stop(0);
    memset(network_env.room_id, 0, sizeof(network_env.room_id));
    set_token(NULL);
    network_env.pairing_has_target = 0;
    set_phase(BLE_TALK_PHASE_STANDBY);

    if (g_callbacks.on_room_full)
        g_callbacks.on_room_full();
}

static void handle_rejoin_ack_cmd(ble_gap_ext_adv_report_ind_t *ind,
                                  ble_talk_network_data_t *nd)
{
    if (nd->len < sizeof(ble_talk_network_rejoin_ack_t)) return;
    ble_talk_network_rejoin_ack_t *ack = (ble_talk_network_rejoin_ack_t *)nd->payload;
    if (!token_matches(&ack->key)) return;
    if (!network_env.is_reconnect_pending ||
        network_env.phase != BLE_TALK_PHASE_WAITING_TALK)
        return;

    LOG_D("Received rejoin-ack broadcast");
    uint8_t clamped = MIN(ack->speaker_cnt, BLE_TALK_NETWORK_MAX_SPEAKERS);
    network_env.speaker_cnt = clamped;
    network_env.is_reconnect_pending = 0;
    /* 取消回连超时定时器 */
    rt_work_cancel(&network_env.reconnect_timeout.work);
    adv_stop(BLE_TALK_NETWORK_ADV_STOP_DELAY);
    set_phase(BLE_TALK_PHASE_TALKING);
}

static void handle_abandon_cmd(ble_gap_ext_adv_report_ind_t *ind,
                               ble_talk_network_data_t *nd)
{
    if (network_env.phase != BLE_TALK_PHASE_TALKING) return;
    ble_talk_network_abandon_t *abn = (ble_talk_network_abandon_t *)nd->payload;
    if (!token_matches(&abn->key)) return;
    abn->role = nd->role;

    LOG_D("Received leave-room broadcast");
    if (abn->role == BLE_TALK_NETWORK_INITIATOR_ROLE)
    {
        LOG_D("The master left, and the room was disbanded.");
        /* 收到 Master 解散消息（Slave 侧） */
        group_delete(&abn->key);
        network_deinit();
        ops_sender_stop();
        ops_scan_stop();
        adv_stop(0);
        memset(network_env.room_id, 0, sizeof(network_env.room_id));
        set_token(NULL);
        network_env.pairing_has_target = 0;
        set_phase(BLE_TALK_PHASE_STANDBY);
    }
    else
    {
        LOG_D("Slave left, now room devices=%d", (int)group_member_count(&abn->key) - 1);
        /* 收到 Slave 退出消息（Master 侧） */
        ble_talk_nework_group_node_t *group = group_find(&abn->key);
        if (group)
        {
            /* 记录到回连白名单 */
            if (network_env.device_role == BLE_TALK_NETWORK_INITIATOR_ROLE &&
                network_env.phase == BLE_TALK_PHASE_TALKING &&
                network_env.reconnect_count <
                    (sizeof(network_env.reconnect_allow_list) /
                     sizeof(network_env.reconnect_allow_list[0])))
            {
                memcpy(&network_env.reconnect_allow_list[network_env.reconnect_count],
                       ind->addr.addr.addr, BD_ADDR_LEN);
                network_env.reconnect_count++;
            }

            member_delete(group, &ind->addr.addr);

            /* 如果房间只剩 Master，自动解散 */
            if (network_env.device_role == BLE_TALK_NETWORK_INITIATOR_ROLE &&
                group_only_master(group))
            {
                LOG_D("There is only Master in the room. auto disbanding.");
                ble_talk_network_abandon_t auto_abn;
                memcpy(&auto_abn.key, &abn->key, sizeof(ble_talk_network_key_t));
                send_abandon(&auto_abn, BLE_TALK_NETWORK_INITIATOR_ROLE);
                memset(network_env.room_id, 0, sizeof(network_env.room_id));
                set_token(NULL);
                ops_sender_stop();
                ops_scan_stop();
                adv_stop(0);
                set_phase(BLE_TALK_PHASE_STANDBY);
            }
        }
    }
}

/* 事件分发 */
static void event_dispatch(ble_gap_ext_adv_report_ind_t *ind,
                           ble_talk_network_data_t *nd)
{
    LOG_D("Received network message: cmd=%d, role=%d, len=%d", nd->cmd, nd->role, nd->len);
    switch (nd->cmd)
    {
    case BLE_TALK_NETWORK_START_CMD:
        handle_start_cmd(ind, nd);
        break;
    case BLE_TALK_NETWORK_JOIN_CMD:
        handle_join_cmd(ind, nd);
        break;
    case BLE_TALK_NETWORK_SYNC_CMD:
        handle_sync_cmd(ind, nd);
        break;
    case BLE_TALK_NETWORK_ROOM_FULL_CMD: 
        handle_room_full_cmd(ind, nd);
        break;
    case BLE_TALK_NETWORK_REJOIN_ACK_CMD:
        handle_rejoin_ack_cmd(ind, nd);
        break;
    case BLE_TALK_NETWORK_ABANDON_CMD:
        handle_abandon_cmd(ind, nd);
        break;
    default:
        break;
    }
}

/* 解析扩展广告载荷 */
static void payload_parser(ble_gap_ext_adv_report_ind_t *ind)
{
    if ((ind->info & (~GAPM_REPORT_INFO_REPORT_TYPE_MASK)) != GAPM_REPORT_INFO_COMPLETE_BIT)
        return;
    uint8_t *data = ind->data;
    uint16_t len = ind->length;
    data = ble_talk_adv_data_found(data, BLE_GAP_AD_TYPE_MANU_SPECIFIC_DATA, &len);
    if (!data) return;
    if (len < 2) return;
    uint16_t company = (uint8_t)data[0] | data[1] << 8;
    if (SIG_SIFLI_COMPANY_ID != company) return;
    if (len < (2 + sizeof(ble_talk_network_data_t))) return;

    ble_talk_network_data_t *nd = (ble_talk_network_data_t *)&data[2];
    if (BLE_TALK_NETWORK_MAGIC != nd->magic) return;
    uint16_t header_and_company = 2 + sizeof(ble_talk_network_data_t);
    uint16_t avail = (len >= header_and_company) ? (len - header_and_company) : 0;
    if (nd->len > avail) return;

    nd->payload = rt_malloc(nd->len + 1);
    if (!nd->payload) return;
    memcpy(nd->payload, data + header_and_company, nd->len);
    event_dispatch(ind, nd);
    rt_free(nd->payload);
}

/* ==================== 公共 API: 初始化与注册 ==================== */

int ble_talk_network_init(void)
{
    rt_list_init(&ble_talk_group_list);
    rt_mutex_init(&network_env.lock, "talk", RT_IPC_FLAG_FIFO);
    rt_delayed_work_init(&network_env.stop_work, ble_talk_network_adv_work_handle, &network_env);
    rt_delayed_work_init(&network_env.pairing_timeout, pairing_timeout_handler, NULL);
    rt_delayed_work_init(&network_env.reconnect_timeout, reconnect_timeout_handler, NULL);
    network_env.phase = BLE_TALK_PHASE_STANDBY;
    network_env.device_role = BLE_TALK_NETWORK_PARTICIPATOR_ROLE;
    network_env.speaker_cnt = 0;
    network_env.is_reconnect_pending = 0;
    network_env.pairing_has_target = 0;
    memset(network_env.room_id, 0, sizeof(network_env.room_id));
    memset(network_env.last_room_id, 0, sizeof(network_env.last_room_id));
    return 0;
}

void ble_talk_network_advertising_init(void)
{
    sibles_advertising_para_t para = {0};
    uint8_t ret;
    char local_name[31] = {0};
    uint16_t manu_company_id = SIG_SIFLI_COMPANY_ID;
    bd_addr_t addr;
    ret = ble_get_public_address(&addr);
    if (ret == HL_ERR_NO_ERROR)
        rt_snprintf(local_name, 31, "SIFLI_APP-%x-%x-%x-%x-%x-%x",
                    addr.addr[0], addr.addr[1], addr.addr[2],
                    addr.addr[3], addr.addr[4], addr.addr[5]);
    else
        memcpy(local_name, "SIFLI_APP", 9);

    ble_gap_dev_name_t *dev_name = (ble_gap_dev_name_t *)rt_malloc(
        sizeof(ble_gap_dev_name_t) + strlen(local_name));
    dev_name->len = strlen(local_name);
    memcpy(dev_name->name, local_name, dev_name->len);
    ble_gap_set_dev_name(dev_name);
    rt_free(dev_name);

    para.own_addr_type = GAPM_STATIC_ADDR;
    para.config.adv_mode = SIBLES_ADV_EXTENDED_MODE;
    para.config.mode_config.extended_config.duration = 0;
    para.config.mode_config.extended_config.interval = 0x140;
    para.config.mode_config.extended_config.max_skip = 0;
    para.config.mode_config.extended_config.phy = GAP_PHY_TYPE_LE_1M;
    para.config.mode_config.extended_config.adv_sid = 0;
    para.config.mode_config.extended_config.connectable_enable = 0;
    para.config.max_tx_pwr = 0x7F;
    para.config.is_auto_restart = 0;
    para.evt_handler = ble_talk_network_advertising_event;
    para.adv_data.completed_name = (sibles_adv_type_name_t *)rt_malloc(
        rt_strlen(local_name) + sizeof(sibles_adv_type_name_t));
    para.adv_data.completed_name->name_len = rt_strlen(local_name);
    rt_memcpy(para.adv_data.completed_name->name, local_name,
              para.adv_data.completed_name->name_len);

    ret = sibles_advertising_init(ble_talk_network_context, &para);
    RT_ASSERT(ret == SIBLES_ADV_NO_ERR);
    rt_free(para.adv_data.completed_name);
}

void ble_talk_network_register_callbacks(const ble_talk_network_callbacks_t *cbs)
{
    if (cbs) g_callbacks = *cbs;
}

void ble_talk_network_set_ops(const ble_talk_network_ops_t *ops)
{
    if (ops) g_ops = *ops;
}

/* ==================== 公共 API: BLE 事件转发 ==================== */

int ble_talk_network_event_handler(uint16_t event_id, uint8_t *data,
                                   uint16_t len, uint32_t context)
{
    switch (event_id)
    {
    case BLE_GAP_EXT_ADV_REPORT_IND:
    {
        ble_gap_ext_adv_report_ind_t *ind = (ble_gap_ext_adv_report_ind_t *)data;
        if ((ind->info & GAPM_REPORT_INFO_REPORT_TYPE_MASK) == GAPM_REPORT_TYPE_ADV_EXT)
        {
            if (0 == ind->period_adv_intv)
                payload_parser(ind);
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

/* ==================== 公共 API: 高层动作 ==================== */

int ble_talk_network_create_room(void)
{
    if (network_env.phase != BLE_TALK_PHASE_STANDBY) return -1;
    if (network_env.device_role != BLE_TALK_NETWORK_INITIATOR_ROLE) return -1;

    /* 生成房间 ID: ROOM_<tick><mac2bytes> */
    bd_addr_t addr;
    ble_get_public_address(&addr);
    snprintf(network_env.room_id, sizeof(network_env.room_id),
             "ROOM_%08lx%02x%02x", rt_tick_get(), addr.addr[0], addr.addr[1]);
    LOG_D("create room, ID: %s", network_env.room_id);

    /* 构建 START 广播 */
    ble_talk_network_start_t start;
    memset(&start, 0, sizeof(start));
    build_key_from_room(network_env.room_id, &start.key);
    start.name_len = strlen(network_env.room_id);
    start.name = (uint8_t *)network_env.room_id;

    adv_start();
    ops_scan_enable();
    send_start(&start);

    /* 启动配对超时定时器 */
    rt_work_submit(&network_env.pairing_timeout.work,
                   BLE_TALK_NETWORK_PAIRING_TIMEOUT_MS);

    set_phase(BLE_TALK_PHASE_PAIRING);
    return 0;
}

int ble_talk_network_scan_rooms(void)
{
    if (network_env.phase != BLE_TALK_PHASE_STANDBY) return -1;
    if (network_env.device_role != BLE_TALK_NETWORK_PARTICIPATOR_ROLE) return -1;

    set_token(NULL);
    adv_start();
    ops_scan_enable();

    /* 启动配对超时定时器 */
    rt_work_submit(&network_env.pairing_timeout.work,
                   BLE_TALK_NETWORK_PAIRING_TIMEOUT_MS);

    set_phase(BLE_TALK_PHASE_PAIRING);
    return 0;
}

int ble_talk_network_confirm_talking(void)
{
    if (network_env.phase != BLE_TALK_PHASE_WAITING_TALK) return -1;
    if (network_env.device_role != BLE_TALK_NETWORK_INITIATOR_ROLE) return -1;

    /* 停止配对超时定时器 */
    rt_work_cancel(&network_env.pairing_timeout.work);

    ble_talk_network_key_t key;
    build_key_from_room(network_env.room_id, &key);
    send_sync(&key);

    adv_stop(BLE_TALK_NETWORK_ADV_STOP_DELAY);
    set_phase(BLE_TALK_PHASE_TALKING);
    return 0;
}

int ble_talk_network_leave_room(void)
{
    if (network_env.phase != BLE_TALK_PHASE_TALKING) return -1;

    ble_talk_network_key_t key;

    if (network_env.device_role == BLE_TALK_NETWORK_PARTICIPATOR_ROLE)
    {
        /* Slave 退出房间，保留回连信息 */
        uint8_t cur_len = (uint8_t)rt_strnlen(network_env.room_id, ROOM_ID_LEN);
        if (cur_len > 0)
        {
            rt_memcpy(network_env.last_room_id, network_env.room_id,
                      MIN(cur_len, ROOM_ID_LEN));
            if (cur_len < ROOM_ID_LEN)
                network_env.last_room_id[cur_len] = '\0';
            network_env.is_reconnect_pending = 1;
            LOG_D("save last room ID: %s", network_env.last_room_id);
        }

        const char *key_src = (strlen(network_env.last_room_id) > 0)
                                  ? network_env.last_room_id
                                  : network_env.room_id;
        build_key_from_room(key_src, &key);

        ble_talk_network_abandon_t abn;
        memcpy(&abn.key, &key, sizeof(ble_talk_network_key_t));
        send_abandon(&abn, BLE_TALK_NETWORK_PARTICIPATOR_ROLE);

        ops_sender_stop();
        ops_scan_stop();
        memset(network_env.room_id, 0, sizeof(network_env.room_id));
        adv_stop(3000);
        network_env.pairing_has_target = 0;
        set_phase(BLE_TALK_PHASE_STANDBY);
    }
    else
    {
        /* Master 解散房间 */
        build_key_from_room(network_env.room_id, &key);

        ble_talk_network_abandon_t abn;
        memcpy(&abn.key, &key, sizeof(ble_talk_network_key_t));
        send_abandon(&abn, BLE_TALK_NETWORK_INITIATOR_ROLE);
        group_delete(&key);

        memset(network_env.room_id, 0, sizeof(network_env.room_id));
        set_token(NULL);
        ops_sender_stop();
        ops_scan_stop();
        adv_stop(3000);
        set_phase(BLE_TALK_PHASE_STANDBY);
    }

    return 0;
}

int ble_talk_network_reconnect(void)
{
    if (network_env.phase != BLE_TALK_PHASE_STANDBY) return -1;
    if (!network_env.is_reconnect_pending) return -1;
    if (network_env.device_role != BLE_TALK_NETWORK_PARTICIPATOR_ROLE) return -1;

    LOG_D("Slave reconnect device start find room: %s", network_env.last_room_id);

    /* 恢复 room_id 与 token */
    strncpy(network_env.room_id, network_env.last_room_id, ROOM_ID_LEN - 1);
    network_env.room_id[ROOM_ID_LEN - 1] = '\0';

    ble_talk_network_key_t key;
    build_key_from_room(network_env.room_id, &key);
    set_token(&key);

    /* 发送 JOIN 请求 */
    char local_name[32];
    get_local_join_name(local_name, sizeof(local_name));
    send_join(&key, local_name);

    adv_start();
    ops_scan_enable();

    /* 启动回连超时定时器 */
    rt_work_submit(&network_env.reconnect_timeout.work,
                   BLE_TALK_NETWORK_RECONNECT_TIMEOUT_MS);

    set_phase(BLE_TALK_PHASE_WAITING_TALK);
    return 0;
}

ble_talk_network_role_t ble_talk_network_switch_role(void)
{
    if (network_env.phase != BLE_TALK_PHASE_STANDBY)
        return network_env.device_role;

    if (network_env.device_role == BLE_TALK_NETWORK_PARTICIPATOR_ROLE)
        network_env.device_role = BLE_TALK_NETWORK_INITIATOR_ROLE;
    else
        network_env.device_role = BLE_TALK_NETWORK_PARTICIPATOR_ROLE;

    LOG_D("Switch to the role of: %s",
          network_env.device_role == BLE_TALK_NETWORK_INITIATOR_ROLE ? "Master" : "Slave");
    return network_env.device_role;
}

void ble_talk_network_enter_idle(void)
{
    ops_sender_stop();
    ops_scan_stop();
    adv_stop(0);

    /* 取消所有定时器 */
    rt_work_cancel(&network_env.pairing_timeout.work);
    rt_work_cancel(&network_env.reconnect_timeout.work);

    /* 强制切换为 Slave + STANDBY */
    network_env.device_role = BLE_TALK_NETWORK_PARTICIPATOR_ROLE;
    memset(network_env.room_id, 0, sizeof(network_env.room_id));
    memset(network_env.last_room_id, 0, sizeof(network_env.last_room_id));
    network_env.is_reconnect_pending = 0;
    network_env.pairing_has_target = 0;
    set_token(NULL);
    network_env.phase = BLE_TALK_PHASE_STANDBY;
}

/* ==================== 公共 API: 状态查询 ==================== */

ble_talk_phase_t ble_talk_network_get_phase(void)
{
    return network_env.phase;
}

ble_talk_network_role_t ble_talk_network_get_role(void)
{
    return network_env.device_role;
}

const char *ble_talk_network_get_room_id(void)
{
    return network_env.room_id;
}

uint8_t ble_talk_network_is_reconnect_pending(void)
{
    return network_env.is_reconnect_pending;
}

uint8_t ble_talk_network_get_speaker_count(void)
{
    return network_env.speaker_cnt;
}

uint8_t ble_talk_network_is_control_data(const uint8_t *mfr_data, uint16_t len)
{
    if (!mfr_data || len < sizeof(uint32_t))
        return 0;
    uint32_t magic;
    memcpy(&magic, mfr_data, sizeof(uint32_t));
    return (magic == BLE_TALK_NETWORK_MAGIC) ? 1 : 0;
}


void ble_talk_network_advertising_stop(uint32_t delay)
{
    adv_stop(delay);
}

void ble_talk_network_advertising_start(void)
{
    adv_start();
}

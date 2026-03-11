/*
 * SPDX-FileCopyrightText: 2024-2024 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include <stdlib.h>

#include "bf0_ble_gap.h"
#include "bf0_sibles.h"
#include "bf0_sibles_internal.h"
#include "bf0_sibles_advertising.h"

#include "ble_talk_network.h"
#include "ble_talk/ble_talk.h"


static struct {
    struct rt_delayed_work work;
    uint8_t state;              /* app_recv_state_t */
    uint8_t is_scaning;
    uint8_t is_scan_restart;
    sync_info_t sync_dev[BLE_TALK_NETWORK_MAX_SPEAKERS];
    uint8_t sync_created_dev;
    uint8_t synced_dev_num;
    uint8_t syncing_idx;
} g_receiver_env = {0};

static ble_talk_callbacks_t g_talk_cbs;

uint8_t ble_app_receiver_get_synced_num(void)
{
    return g_receiver_env.synced_dev_num;
}

void ble_talk_register_callbacks(const ble_talk_callbacks_t *cbs)
{
    if (cbs) {
        g_talk_cbs = *cbs;
    } else {
        memset(&g_talk_cbs, 0, sizeof(g_talk_cbs));
    }
}

uint8_t ble_app_scan_enable(void)
{
    uint8_t ret = 1;
    LOG_D("scan enable %d", g_receiver_env.is_scaning);
    if (g_receiver_env.is_scaning == 0)
    {
        uint8_t is_slow = (g_receiver_env.synced_dev_num != 0 || ble_app_sender_is_working());
        uint16_t inv = is_slow ? DEFAULT_SCAN_SLOW_INTERVAL : DEFAULT_SCAN_FAST_INTERVAL;
        ble_gap_scan_start_t scan_param =
        {
            .own_addr_type = GAPM_STATIC_ADDR,
            .type = GAPM_SCAN_TYPE_OBSERVER,
            .dup_filt_pol = 0,
            .scan_param_1m.scan_intv = inv * 8 / 5,
            .scan_param_1m.scan_wd = DEFAULT_SCAN_WIN * 8 / 5,
            .duration = 0,
            .period = 0,
            .prop = GAPM_SCAN_PROP_PHY_1M_BIT,
        };
        ret = ble_gap_scan_start_ex(&scan_param);
    }

    return ret;

}

uint8_t ble_app_scan_stop(void)
{
    uint8_t ret = 0;
    if (g_receiver_env.is_scaning)
        ret = ble_gap_scan_stop();
    return ret;
}

uint8_t ble_app_scan_restart(void)
{
    LOG_D("scan restart");
    if (g_receiver_env.is_scaning)
    {
        g_receiver_env.is_scan_restart = 1;
        ble_gap_scan_stop();
    }
    return 0;
}

static void ble_app_create_per_adv_sync(void)
{
    for (uint32_t i = 0; i < BLE_TALK_NETWORK_MAX_SPEAKERS; i++)
        ble_gap_create_periodic_advertising_sync();
}

static sync_info_t *ble_app_find_idle_sync_dev(void)
{
    sync_info_t *dev = NULL;
    for (uint32_t i = 0; i < BLE_TALK_NETWORK_MAX_SPEAKERS; i++)
    {
        if (g_receiver_env.sync_dev[i].dev_state == APP_RECV_DEV_STATE_IDLE)
        {
            dev = &g_receiver_env.sync_dev[i];
            break;
        }
    }

    return dev;
}

static sync_info_t *ble_app_find_available_sync_dev(ble_gap_addr_t *addr)
{
    sync_info_t *dev = NULL;
    uint8_t is_dup = 0;
    for (uint32_t i = 0; i < BLE_TALK_NETWORK_MAX_SPEAKERS; i++)
    {
        if (g_receiver_env.sync_dev[i].dev_state == APP_RECV_DEV_STATE_CREATED)
        {
            dev = &g_receiver_env.sync_dev[i];
        }

        if ((memcmp(&g_receiver_env.sync_dev[i].addr, addr, sizeof(ble_gap_addr_t)) == 0) &&
                (g_receiver_env.sync_dev[i].dev_state > APP_RECV_DEV_STATE_CREATED))
        {
            is_dup = 1;
        }
    }

    return is_dup ? NULL : dev;
}

static sync_info_t *ble_app_find_sync_dev_by_idx(uint8_t idx)
{
    sync_info_t *dev = NULL;
    for (uint32_t i = 0; i < BLE_TALK_NETWORK_MAX_SPEAKERS; i++)
    {
        if ((g_receiver_env.sync_dev[i].dev_state >= APP_RECV_DEV_STATE_CREATED) &&
                (g_receiver_env.sync_dev[i].sync_idx == idx))
        {
            dev = &g_receiver_env.sync_dev[i];
            break;
        }
    }

    return dev;
}

static uint8_t ble_app_sync_dev_created(uint8_t actv_idx)
{
    sync_info_t *dev = NULL;
    // 1 means failed
    uint8_t ret = 1;
    if ((dev = ble_app_find_idle_sync_dev()) != NULL)
    {
        g_receiver_env.sync_created_dev++;
        dev->sync_idx = actv_idx;
        dev->dev_state = APP_RECV_DEV_STATE_CREATED;
        ret = 0;
    }

    return ret;
}

static uint8_t ble_app_sync_dev_synced(uint8_t actv_idx)
{
    sync_info_t *dev = NULL;
    // 1 means failed
    uint8_t ret = 1;
    if ((dev = ble_app_find_sync_dev_by_idx(actv_idx)) != NULL)
    {
        g_receiver_env.synced_dev_num++;
        g_receiver_env.syncing_idx = INVALID_CONN_IDX;
        dev->dev_state = APP_RECV_DEV_STATE_SYNCED;
        ret = 0;
    }

    return ret;
}

static uint8_t ble_app_sync_dev_stopped(uint8_t actv_idx)
{
    sync_info_t *dev = NULL;
    // 1 means failed
    uint8_t ret = 1;
    if ((dev = ble_app_find_sync_dev_by_idx(actv_idx)) != NULL)
    {
        if (dev->dev_state == APP_RECV_DEV_STATE_SYNCED)
        {
            g_receiver_env.synced_dev_num--;
            
        }                     
        else if (dev->dev_state == APP_RECV_DEV_STATE_SYNCING)
            g_receiver_env.syncing_idx = INVALID_CONN_IDX;

        memset(&dev->addr, 0, sizeof(dev->addr));
        dev->dev_state = APP_RECV_DEV_STATE_CREATED;
        ret = 0;
    }
    return ret;
}

static uint8_t ble_app_sync_dev_deleted(uint8_t actv_idx)
{
    sync_info_t *dev = NULL;
    // 1 means failed
    uint8_t ret = 1;
    if ((dev = ble_app_find_sync_dev_by_idx(actv_idx)) != NULL)
    {
        g_receiver_env.sync_created_dev--;
        memset(&dev->addr, 0, sizeof(dev->addr));
        dev->sync_idx = INVALID_SYNC_IDX;
        dev->dev_state = APP_RECV_DEV_STATE_IDLE;
        ret = 0;
    }

    return ret;
}

static uint8_t ble_app_start_per_adv_sync(uint8_t sync_idx, ble_gap_addr_t *addr, uint8_t adv_sid, uint16_t sync_to)
{
    uint8_t ret = GAP_ERR_INVALID_PARAM;
    if (sync_idx != INVALID_SYNC_IDX)
    {
        ble_gap_periodic_advertising_sync_start_t sync_param =
        {
            .actv_idx = sync_idx,
            .addr = *addr,
            .skip = 0,
            .sync_to = sync_to,
            .type = GAP_PER_SYNC_TYPE_GENERAL,
            .adv_sid = adv_sid,
        };

        ret = ble_gap_start_periodic_advertising_sync(&sync_param);
    }
    return ret;
}

static void ble_app_stop_per_adv_sync(uint8_t sync_idx)
{
    if (sync_idx != INVALID_SYNC_IDX)
    {
        ble_gap_eriodic_advertising_sync_stop_t stop_param;
        stop_param.actv_idx = sync_idx;
        ble_gap_stop_periodic_advertising_sync(&stop_param);
    }

}

static void ble_app_receiver_data_parser(ble_gap_ext_adv_report_ind_t *ind)
{
    // ind->data is audio data
    //LOG_HEX("per data", 16, ind->data, ind->length);
    ble_talk_downlink(ind->actv_idx, ind->data, ind->length);
}

uint8_t *ble_app_adv_data_found(uint8_t *p_data, uint8_t type, uint16_t *length)
{
    if (!p_data || !length)
        return NULL;

    // Cursor
    uint8_t *p_cursor = p_data;
    // End of data
    uint8_t *p_end_cursor = p_data + *length;

    while (p_cursor < p_end_cursor)
    {
        // Extract AD type
        uint8_t ad_type = *(p_cursor + 1);

        // Check if it's AD Type which shall be unique
        if (ad_type == type)
        {
            *length = *p_cursor - 1;
            break;
        }

        /* Go to next advertising info */
        p_cursor += (*p_cursor + 1);
    }

    return (p_cursor >= p_end_cursor) ? NULL : (p_cursor + 2);
}

void ble_app_network_parser(ble_gap_ext_adv_report_ind_t *ind)
{
    // Not connectable/scannable/directable
    if ((ind->info & (~GAPM_REPORT_INFO_REPORT_TYPE_MASK)) != GAPM_REPORT_INFO_COMPLETE_BIT)
        return;

    uint8_t *data = ind->data;
    uint16_t len = ind->length;

    if ((data = ble_app_adv_data_found(data, BLE_GAP_AD_TYPE_MANU_SPECIFIC_DATA, &len)) != NULL)
    {
        uint16_t company = (uint8_t)data[0] | data[1] << 8;
        if (SIG_SIFLI_COMPANY_ID == company &&
            ble_talk_network_is_control_data(&data[2], len - 2))
            return;

        /* Room identification filtering for audio periodic broadcasting */
        uint8_t cod_len = data[2];
        const char *remote_code = (const char *)&data[3];
        const char *my_room = ble_talk_network_get_room_id();
        uint8_t my_len = (uint8_t)rt_strnlen(my_room ? my_room : "", ROOM_ID_LEN);
        rt_kprintf("recv room code: %.*s\r\n", cod_len, remote_code);
        rt_kprintf("my room code: %.*s\r\n", my_len, my_room);

        if (my_len == 0)
        {
            return;
        }
        if (cod_len == my_len && my_room && (rt_memcmp(remote_code, my_room, my_len) == 0))
        {
            // Find target device in same room
            sync_info_t *dev;
                if ((g_receiver_env.state == APP_RECV_STATE_IDLE)
                    && ((dev = ble_app_find_available_sync_dev(&ind->addr)) != NULL))
            {
                if (ble_app_start_per_adv_sync(dev->sync_idx, &ind->addr, ind->adv_sid, DEFAULT_SYNC_TO / 10) == HL_ERR_NO_ERROR)
                {
                    LOG_D("sync start");
                    g_receiver_env.state = APP_RECV_STATE_SYNCING;
                    g_receiver_env.syncing_idx = dev->sync_idx;
                    dev->dev_state = APP_RECV_DEV_STATE_SYNCING;
                    dev->addr = ind->addr;
                }
            }
        }
    }
}

int ble_app_receiver_event_handler(uint16_t event_id, uint8_t *data, uint16_t len, uint32_t context)
{
    switch (event_id)
    {
    case BLE_GAP_SCAN_START_CNF:
    {
        ble_gap_start_scan_cnf_t *cnf = (ble_gap_start_scan_cnf_t *)data;
        LOG_I("Scan start status %d", cnf->status);
        if (cnf->status == HL_ERR_NO_ERROR)
        {
            g_receiver_env.is_scaning = 1;
            if (g_talk_cbs.on_scan_state_changed) g_talk_cbs.on_scan_state_changed(1);
        }
        break;
    }
    case BLE_GAP_SCAN_STOP_CNF:
    {
        ble_gap_stop_scan_cnf_t *cnf = (ble_gap_stop_scan_cnf_t *)data;
        LOG_I("Scan stop");
        if (g_receiver_env.is_scan_restart)
        {
            g_receiver_env.is_scan_restart = 0;
            ble_app_scan_enable();
        }
        break;
    }
    case BLE_GAP_SCAN_STOPPED_IND:
    {
        ble_gap_scan_stopped_ind_t *ind = (ble_gap_scan_stopped_ind_t *)data;
        LOG_I("Scan stopped %d", ind->reason);
        g_receiver_env.is_scaning = 0;
        if (g_talk_cbs.on_scan_state_changed) g_talk_cbs.on_scan_state_changed(0);

        break;
    }
    case BLE_GAP_EXT_ADV_REPORT_IND:
    {
        ble_gap_ext_adv_report_ind_t *ind = (ble_gap_ext_adv_report_ind_t *)data;
        if ((ind->info & GAPM_REPORT_INFO_REPORT_TYPE_MASK) == GAPM_REPORT_TYPE_PER_ADV)
        {
            // Only for the context of periodic adv is fulfilled duplicated numeric which increased one by one for a new adv.
            ble_app_receiver_data_parser(ind);
        }
        else if ((ind->info & GAPM_REPORT_INFO_REPORT_TYPE_MASK) == GAPM_REPORT_TYPE_ADV_EXT)
        {
            ble_app_network_parser(ind);
        }
        break;
    }
    case BLE_GAP_CREATE_PERIODIC_ADV_SYNC_CNF:
    {
        ble_gap_create_per_adv_sync_cnf_t *cnf = (ble_gap_create_per_adv_sync_cnf_t *)data;
        LOG_I("Create PER_ADV_SYNC result %d", cnf->status);
        break;
    }
    case BLE_GAP_PERIODIC_ADV_SYNC_CREATED_IND:
    {
        ble_gap_per_adv_sync_created_ind_t *ind = (ble_gap_per_adv_sync_created_ind_t *)data;
        sync_info_t *dev = NULL;
        if (ble_app_sync_dev_created(ind->actv_idx) != 0)
            RT_ASSERT("sync created failed" && 0);

        LOG_D("PER_ADV_SYNC created %d", ind->actv_idx);
        break;
    }
    case BLE_GAP_START_PERIODIC_ADV_SYNC_CNF:
    {
        ble_gap_start_per_adv_sync_cnf_t *cnf = (ble_gap_start_per_adv_sync_cnf_t *)data;
        LOG_I("Start PER_ADV_SYNC result %d", cnf->status);
        if (cnf->status == HL_ERR_NO_ERROR)
        {
            rt_work_submit(&g_receiver_env.work.work, DEFAULT_SYNCING_PERIOD * 1000);
        }
        else
        {
            g_receiver_env.state = APP_RECV_STATE_SYNCING;
        }
        break;
    }
    case BLE_GAP_PERIODIC_ADV_SYNC_ESTABLISHED_IND:
    {
        ble_gap_per_adv_sync_established_t *ind = (ble_gap_per_adv_sync_established_t *)data;
        LOG_I("PER_ADV_SYNC established(%x-%x-%x-%x-%x-%x)", ind->addr.addr.addr[0], ind->addr.addr.addr[1],
              ind->addr.addr.addr[2], ind->addr.addr.addr[3],
              ind->addr.addr.addr[4], ind->addr.addr.addr[5]);

        ble_app_sync_dev_synced(ind->actv_idx);
        if (g_talk_cbs.on_receiver_synced) g_talk_cbs.on_receiver_synced(g_receiver_env.synced_dev_num);
        g_receiver_env.state = APP_RECV_STATE_IDLE;

        if (g_receiver_env.synced_dev_num >= BLE_TALK_NETWORK_MAX_SPEAKERS)
        {
            ble_gap_scan_stop();
        }
        else
        {
            // Adjust scan parameters
            ble_app_scan_restart();
        }
        break;
    }
    case BLE_GAP_STOP_PERIODIC_ADV_SYNC_CNF:
    {
        ble_gap_stop_per_adv_sync_cnf_t *cnf = (ble_gap_stop_per_adv_sync_cnf_t *)data;
        LOG_D("PER_ADV_SYNC stop cnf %d %d", cnf->status, cnf->actv_index);
        break;
    }
    case BLE_GAP_PERIODIC_ADV_SYNC_STOPPED_IND:
    {
        ble_gap_per_adv_sync_stopped_ind_t *ind = (ble_gap_per_adv_sync_stopped_ind_t *)data;
        LOG_D("PER_ADV_ SYNC stopped reason %d, idx %d ", ind->reason, ind->actv_idx);

        ble_app_sync_dev_stopped(ind->actv_idx);
        if (g_talk_cbs.on_receiver_sync_stopped) g_talk_cbs.on_receiver_sync_stopped(g_receiver_env.synced_dev_num);
        g_receiver_env.state = APP_RECV_STATE_IDLE;

        if (g_receiver_env.synced_dev_num < BLE_TALK_NETWORK_MAX_SPEAKERS
            && !g_receiver_env.is_scaning)
        {
            ble_app_scan_enable();
        }
        break;
    }
    case BLE_GAP_DELETE_PERIODIC_ADV_SYNC_CNF:
    {
        ble_gap_delete_per_adv_sync_cnf_t *cnf = (ble_gap_delete_per_adv_sync_cnf_t *)data;
        LOG_D("per_adv_sync(%d) deleted %d", cnf->actv_index, cnf->status);
        if (cnf->status == HL_ERR_NO_ERROR)
            ble_app_sync_dev_deleted(cnf->actv_index);

        break;
    }
    default:
        break;
    }
    return 0;
}

uint8_t ble_app_scan_init()
{
    uint8_t ret = ble_app_scan_enable();
    ble_app_create_per_adv_sync();
    LOG_I("scan enabled %d", ret);
    return 0;
}

static void ble_app_per_sync_check(struct rt_work *work, void *work_data)
{
    (void)work;
    (void)work_data;
    if (g_receiver_env.state == APP_RECV_STATE_SYNCING)
    {
        ble_app_stop_per_adv_sync(g_receiver_env.syncing_idx);
    }
}

void ble_app_receviver_init(void)
{
    memset(&g_receiver_env, 0, sizeof(g_receiver_env));
    for (uint32_t i = 0; i < BLE_TALK_NETWORK_MAX_SPEAKERS; i++) {
        g_receiver_env.sync_dev[i].sync_idx = INVALID_SYNC_IDX;
        g_receiver_env.sync_dev[i].dev_state = APP_RECV_DEV_STATE_IDLE;
    }
    rt_delayed_work_init(&g_receiver_env.work, ble_app_per_sync_check, NULL);

#if BLE_TALK_MIC_ALWAYS_ON
    talk_init(AUDIO_TXRX);
#else
    talk_init(AUDIO_RX);
#endif
}

int recv(int argc, char *argv[])
{
    if (argc > 1)
    {
        if (strcmp(argv[1], "scan") == 0)
        {
            if (strcmp(argv[2], "start") == 0)
            {
                ble_gap_scan_start_t scan_param =
                {
                    .own_addr_type = GAPM_STATIC_ADDR,
                    .type = GAPM_SCAN_TYPE_OBSERVER,
                    .dup_filt_pol = atoi(argv[3]),
                    .scan_param_1m.scan_intv = atoi(argv[4]) * 8 / 5,
                    .scan_param_1m.scan_wd = atoi(argv[5]) * 8 / 5,
                    .duration = atoi(argv[6]) / 10,
                    .period = 0,
                };

                uint8_t ret = ble_gap_scan_start(&scan_param);
                LOG_D("scan start %d", ret);
            }
            else if (strcmp(argv[2], "stop") == 0)
            {
                ble_gap_scan_stop();
            }
            else
            {
                LOG_I("Scan start: diss scan start [dup, 0/1] [interval, ms] [window, ms] [duration, ms]");
                LOG_I("Scan stop: diss scan stop");
            }
        }
        else if (strcmp(argv[1], "sync") == 0)
        {
            if (strcmp(argv[2], "create") == 0)
            {
                ble_app_create_per_adv_sync();
            }
            else if (strcmp(argv[2], "start") == 0)
            {
                ble_gap_addr_t peer_addr;
                uint8_t sync_idx = atoi(argv[3]);
                hex2data(argv[4], peer_addr.addr.addr, BD_ADDR_LEN);
                LOG_HEX("enter addr", 16, peer_addr.addr.addr, BD_ADDR_LEN);
                peer_addr.addr_type = atoi(argv[5]);
                uint8_t adv_sid = atoi(argv[6]);
                uint16_t sync_to = atoi(argv[7]);
                sync_info_t *dev = ble_app_find_sync_dev_by_idx(sync_idx);
                if (dev->dev_state == APP_RECV_DEV_STATE_CREATED)
                    ble_app_start_per_adv_sync(sync_idx, &peer_addr, adv_sid, sync_to);
                else
                    LOG_W("Enter wrongly idx, dev status is %d", dev->dev_state);
            }
            else if (strcmp(argv[2], "stop") == 0)
            {
                uint8_t sync_idx = atoi(argv[3]);
                sync_info_t *dev = ble_app_find_sync_dev_by_idx(sync_idx);
                if (dev->dev_state == APP_RECV_DEV_STATE_SYNCING || dev->dev_state == APP_RECV_DEV_STATE_SYNCED)
                    ble_app_stop_per_adv_sync(sync_idx);
                else
                    LOG_W("Enter wrongly idx, dev status is %d", dev->dev_state);
            }
        }
    }

    return 0;
}

#ifdef RT_USING_FINSH
    MSH_CMD_EXPORT(recv, receiver commander);
#endif

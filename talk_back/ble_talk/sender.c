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
#include "log.h"

SIBLES_ADVERTISING_CONTEXT_DECLAR(g_app_advertising_context);
static sibles_adv_type_name_t *g_adv_completed_name_buf = RT_NULL;
static sibles_adv_type_manufacturer_data_t *g_adv_manufacturer_buf = RT_NULL;



typedef enum
{
    APP_SNED_STATE_IDLE,
    APP_SEND_STATE_ADV_PREPARE,
    APP_SEND_STATE_ADVERTISING,
} app_send_state_t;

static uint8_t ble_app_advertising_event(uint8_t event, void *context, void *data);

static struct {
    struct rt_delayed_work work;
    uint8_t state;
    uint8_t is_stopping;
} g_sender_env = {0};

static void ble_app_peri_advertising_start(void)
{
    sibles_advertising_start(g_app_advertising_context);
}

/* After the room ID changes, dynamically refresh the room code in the broadcast. */
void ble_app_sender_update_adv_room_code(void)
{
    sibles_advertising_para_t para = {0};
    const char *room_code = ble_talk_network_get_room_id();

    char local_name[31] = {0};
    uint16_t manu_company_id = SIG_SIFLI_COMPANY_ID;
    bd_addr_t addr;
    int ret = ble_get_public_address(&addr);
    if (ret == HL_ERR_NO_ERROR)
        rt_snprintf(local_name, 31, "SIFLI_APP-%x-%x-%x-%x-%x-%x", addr.addr[0], addr.addr[1], addr.addr[2], addr.addr[3], addr.addr[4], addr.addr[5]);
    else
        memcpy(local_name, DEFAULT_LOCAL_NAME, sizeof(DEFAULT_LOCAL_NAME));


    uint16_t name_len = (uint16_t)rt_strlen(local_name);
    uint32_t name_buf_sz = name_len + sizeof(sibles_adv_type_name_t);
    if (!g_adv_completed_name_buf)
        g_adv_completed_name_buf = (sibles_adv_type_name_t *)rt_malloc(name_buf_sz);
    else
        g_adv_completed_name_buf = (sibles_adv_type_name_t *)rt_realloc(g_adv_completed_name_buf, name_buf_sz);
    if (!g_adv_completed_name_buf) return;
    g_adv_completed_name_buf->name_len = name_len;
    rt_memcpy(g_adv_completed_name_buf->name, local_name, name_len);
    para.adv_data.completed_name = g_adv_completed_name_buf;

    uint8_t cod_len = room_code ? (uint8_t)rt_strnlen(room_code, ROOM_ID_LEN) : 0;
    if (cod_len == 0)
    {
        room_code = BLE_TALK_DEFAULT_NETWORK_CODE;
        cod_len = (uint8_t)rt_strlen(BLE_TALK_DEFAULT_NETWORK_CODE);
    }

    uint32_t manu_buf_sz = sizeof(sibles_adv_type_manufacturer_data_t) + cod_len + 1;
    if (!g_adv_manufacturer_buf)
        g_adv_manufacturer_buf = (sibles_adv_type_manufacturer_data_t *)rt_malloc(manu_buf_sz);
    else
        g_adv_manufacturer_buf = (sibles_adv_type_manufacturer_data_t *)rt_realloc(g_adv_manufacturer_buf, manu_buf_sz);
    if (!g_adv_manufacturer_buf) return;
    g_adv_manufacturer_buf->company_id = manu_company_id;
    g_adv_manufacturer_buf->data_len = cod_len + 1;
    g_adv_manufacturer_buf->additional_data[0] = cod_len;
    if (cod_len > 0)
        rt_memcpy(&g_adv_manufacturer_buf->additional_data[1], room_code, cod_len);
    para.adv_data.manufacturer_data = g_adv_manufacturer_buf;

    rt_kprintf("update adv room code: %.*s (len=%d)\r\n", cod_len, room_code, cod_len);
    sibles_advertising_update_adv_and_scan_rsp_data(g_app_advertising_context, &para.adv_data, NULL);

}

uint8_t ble_app_sender_is_working(void)
{
    return (g_sender_env.state == APP_SEND_STATE_ADVERTISING);
}

/* Enable advertise via advertising service. */
void ble_app_peri_advertising_init(void)
{
    sibles_advertising_para_t para = {0};
    uint8_t ret;

    char local_name[31] = {0};
    uint16_t manu_company_id = SIG_SIFLI_COMPANY_ID;
    bd_addr_t addr;
    ret = ble_get_public_address(&addr);
    if (ret == HL_ERR_NO_ERROR)
        rt_snprintf(local_name, 31, "SIFLI_APP-%x-%x-%x-%x-%x-%x", addr.addr[0], addr.addr[1], addr.addr[2], addr.addr[3], addr.addr[4], addr.addr[5]);
    else
        memcpy(local_name, DEFAULT_LOCAL_NAME, sizeof(DEFAULT_LOCAL_NAME));

    ble_gap_dev_name_t *dev_name = malloc(sizeof(ble_gap_dev_name_t) + strlen(local_name));
    dev_name->len = strlen(local_name);
    memcpy(dev_name->name, local_name, dev_name->len);
    ble_gap_set_dev_name(dev_name);
    free(dev_name);

    para.own_addr_type = GAPM_STATIC_ADDR;
    para.config.adv_mode = SIBLES_ADV_PERIODIC_MODE;

    para.config.mode_config.periodic_config.duration = 0;
    para.config.mode_config.periodic_config.interval = 0xA0;

    para.config.mode_config.periodic_config.max_skip = 0;
    para.config.mode_config.periodic_config.phy = GAP_PHY_TYPE_LE_1M;
    para.config.mode_config.periodic_config.adv_sid = 0;
    para.config.mode_config.periodic_config.connectable_enable = 0;

    para.config.mode_config.periodic_config.adv_intv_min = 16;
    para.config.mode_config.periodic_config.adv_intv_max = 16;

    para.config.max_tx_pwr = 0x7F;
    /* Advertising will re-start after disconnected. */
    // in multi-connection
    para.config.is_auto_restart = 1;
    /* Scan rsp data is same as advertising data. */
    //para.config.is_rsp_data_duplicate = 1;

    /* Prepare name filed. Due to name is too long to put adv data, put it to rsp data.*/
    para.adv_data.completed_name = rt_malloc(rt_strlen(local_name) + sizeof(sibles_adv_type_name_t));
    para.adv_data.completed_name->name_len = rt_strlen(local_name);
    rt_memcpy(para.adv_data.completed_name->name, local_name, para.adv_data.completed_name->name_len);

    /* Prepare manufacturer filed: embed current room id for receiver filtering. */
    const char *room_code = ble_talk_network_get_room_id();
    rt_kprintf("room code: %s\r\n", room_code);
    uint8_t cod_len = room_code ? (uint8_t)rt_strnlen(room_code, ROOM_ID_LEN) : 0;
    if (cod_len == 0)
    {
        room_code = BLE_TALK_DEFAULT_NETWORK_CODE;
        cod_len = (uint8_t)rt_strlen(BLE_TALK_DEFAULT_NETWORK_CODE);
    }
    para.adv_data.manufacturer_data = rt_malloc(sizeof(sibles_adv_type_manufacturer_data_t) + cod_len + 1);
    para.adv_data.manufacturer_data->company_id = manu_company_id;
    para.adv_data.manufacturer_data->data_len = cod_len + 1;
    para.adv_data.manufacturer_data->additional_data[0] = cod_len;
    if (cod_len > 0)
        rt_memcpy((void *)&para.adv_data.manufacturer_data->additional_data[1], room_code, cod_len);

    para.evt_handler = ble_app_advertising_event;

    uint8_t per_len = BLE_TALK_PERIODIC_ADV_MAX_LEN;
    para.periodic_data = rt_malloc(sizeof(sibles_periodic_adv_t) + per_len);
    memset(para.periodic_data->data, 0, per_len);
    para.periodic_data->len = per_len;

    ret = sibles_advertising_init(g_app_advertising_context, &para);
    RT_ASSERT(ret == SIBLES_ADV_NO_ERR);

    rt_free(para.adv_data.completed_name);
    rt_free(para.adv_data.manufacturer_data);
    rt_free(para.periodic_data);
}

static void ble_app_sender_work_handler(struct rt_work *work, void *work_data)
{
    (void)work;
    (void)work_data;
    if (g_sender_env.state == APP_SEND_STATE_ADV_PREPARE)
    {
        ble_app_peri_advertising_start();
    }
    else if (g_sender_env.state == APP_SEND_STATE_ADVERTISING)
    {
        sibles_advertising_stop(g_app_advertising_context);
#if !BLE_TALK_MIC_ALWAYS_ON
    talk_deinit();
#endif
    }
}

uint8_t app_send_voice_data(uint16_t len, uint8_t *voice_data)
{
    // parameter error
    if (!voice_data)
        return 1;

    sibles_periodic_adv_t *data = rt_malloc(sizeof(sibles_periodic_adv_t) + len);
    memcpy(data->data, voice_data, len);
    data->len = len;
    sibles_advertising_update_periodic_data(g_app_advertising_context, data);
    rt_free(data);

    return 0;
}

uint8_t ble_app_sender_trigger(void)
{
    uint8_t ret = 1;
    if (g_sender_env.state == APP_SNED_STATE_IDLE)
    {
        g_sender_env.state = APP_SEND_STATE_ADV_PREPARE;
        rt_work_submit(&g_sender_env.work.work, 0);
        ret = 0;
    }
    return ret;
}

uint8_t ble_app_sender_stop(void)
{
    if (g_sender_env.state == APP_SEND_STATE_ADV_PREPARE
            || g_sender_env.state == APP_SEND_STATE_ADVERTISING)
    {
        g_sender_env.is_stopping = 1;
        if (g_sender_env.state == APP_SEND_STATE_ADVERTISING)
        {
            rt_work_submit(&g_sender_env.work.work, 0);
        }
    }

    return 0;
}

static uint8_t ble_app_advertising_event(uint8_t event, void *context, void *data)
{
    switch (event)
    {
    case SIBLES_ADV_EVT_ADV_STARTED:
    {
        sibles_adv_evt_startted_t *evt = (sibles_adv_evt_startted_t *)data;
        LOG_I("ADV start resutl %d, mode %d\r\n", evt->status, evt->adv_mode);
        if (evt->status == HL_ERR_NO_ERROR &&
                evt->adv_mode == SIBLES_ADV_MODE_PERIODIC)
        {
            if (g_sender_env.is_stopping)
            {
                sibles_advertising_stop(g_app_advertising_context);
            }
            else
            {
                /* Refresh the broadcast code */
                ble_app_sender_update_adv_room_code();
#if !BLE_TALK_MIC_ALWAYS_ON
                 talk_init(AUDIO_TX);
#endif
                ble_app_scan_restart();
                g_sender_env.state = APP_SEND_STATE_ADVERTISING;
            }
        }
        break;
    }
    case SIBLES_ADV_EVT_ADV_STOPPED:
    {
        sibles_adv_evt_stopped_t *evt = (sibles_adv_evt_stopped_t *)data;
        LOG_I("ADV stopped reason %d, mode %d\r\n", evt->reason, evt->adv_mode);
        g_sender_env.is_stopping = 0;
        g_sender_env.state = APP_SNED_STATE_IDLE;
        break;
    }
    default:
        break;
    }
    return 0;
}

int ble_app_sender_event_handler(uint16_t event_id, uint8_t *data, uint16_t len, uint32_t context)
{
    switch (event_id)
    {
    default:
        break;
    }
    return 0;
}

void ble_app_sender_init(void)
{
    g_sender_env.state = APP_SNED_STATE_IDLE;
    g_sender_env.is_stopping = 0;
    rt_delayed_work_init(&g_sender_env.work, ble_app_sender_work_handler, NULL);
}

int sender(int argc, char *argv[])
{
    if (argc > 1)
    {
        if (strcmp(argv[1], "adv_init") == 0)
        {
            ble_app_peri_advertising_start();
        }
        else if (strcmp(argv[1], "adv_start") == 0)
        {
            sibles_advertising_start(g_app_advertising_context);
        }
        else if (strcmp(argv[1], "adv_stop") == 0)
        {
            sibles_advertising_stop(g_app_advertising_context);
        }
    }

    return 0;
}

#ifdef RT_USING_FINSH
    MSH_CMD_EXPORT(sender, sender cmd);
#endif

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_resources.h>

#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nrf24_soft.h"
#include "walkie_protocol.h"

#define WALKIE_APP_NAME "NRF Walkie"
#define WALKIE_ADC_CHANNEL FuriHalAdcChannel4
#define WALKIE_RADIO_CHANNEL 90u
#define WALKIE_UI_QUEUE_LEN 16u

typedef struct {
    uint8_t data[WALKIE_RX_BUF_SIZE];
    uint16_t head;
    uint16_t tail;
    uint16_t count;
} WalkieRxRing;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriMutex* mutex;
    FuriThread* radio_thread;
    bool running;
    bool ptt_pressed;
    bool tx_mode;
    bool radio_ready;
    bool config_dirty;
    uint8_t node_id;
    uint16_t rx_count;
    uint32_t tx_packets;
    uint32_t rx_packets;
    char status_line[32];
} WalkieApp;

static void walkie_lock(WalkieApp* app) {
    furi_check(furi_mutex_acquire(app->mutex, 1000) == FuriStatusOk);
}

static void walkie_unlock(WalkieApp* app) {
    furi_check(furi_mutex_release(app->mutex) == FuriStatusOk);
}

static void walkie_set_status(WalkieApp* app, const char* status) {
    walkie_lock(app);
    snprintf(app->status_line, sizeof(app->status_line), "%s", status);
    walkie_unlock(app);
}

static void walkie_rx_push(WalkieRxRing* ring, uint8_t value) {
    if(ring->count >= WALKIE_RX_BUF_SIZE) return;

    ring->data[ring->head] = value;
    ring->head = (uint16_t)((ring->head + 1u) % WALKIE_RX_BUF_SIZE);
    ring->count++;
}

static bool walkie_rx_pop(WalkieRxRing* ring, uint8_t* value) {
    if(ring->count == 0u) return false;

    *value = ring->data[ring->tail];
    ring->tail = (uint16_t)((ring->tail + 1u) % WALKIE_RX_BUF_SIZE);
    ring->count--;
    return true;
}

static uint8_t walkie_sample_to_pwm(uint8_t sample) {
    const uint32_t span = 80u;
    return (uint8_t)(10u + ((uint32_t)sample * span) / 255u);
}

static void walkie_audio_output_sample(uint8_t sample) {
    furi_hal_pwm_set_params(
        FuriHalPwmOutputIdTim1PA7, WALKIE_PWM_CARRIER_HZ, walkie_sample_to_pwm(sample));
}

static void walkie_draw_callback(Canvas* canvas, void* context) {
    WalkieApp* app = context;
    uint8_t node_id = 0;
    uint16_t rx_count = 0;
    uint32_t tx_packets = 0;
    uint32_t rx_packets = 0;
    bool ptt = false;
    bool tx_mode = false;
    bool radio_ready = false;
    char status_line[32] = {0};
    char line[32];

    if(furi_mutex_acquire(app->mutex, 25) == FuriStatusOk) {
        node_id = app->node_id;
        rx_count = app->rx_count;
        tx_packets = app->tx_packets;
        rx_packets = app->rx_packets;
        ptt = app->ptt_pressed;
        tx_mode = app->tx_mode;
        radio_ready = app->radio_ready;
        snprintf(status_line, sizeof(status_line), "%s", app->status_line);
        furi_mutex_release(app->mutex);
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, WALKIE_APP_NAME);

    canvas_draw_frame(canvas, 0, 13, 128, 51);

    canvas_set_font(canvas, FontSecondary);
    snprintf(line, sizeof(line), "NODE: %u   MODE: %s", node_id, tx_mode ? "TX" : "RX");
    canvas_draw_str(canvas, 4, 24, line);

    snprintf(line, sizeof(line), "PTT: %s   NRF: %s", ptt ? "HOLD" : "IDLE", radio_ready ? "OK" : "--");
    canvas_draw_str(canvas, 4, 34, line);

    snprintf(
        line,
        sizeof(line),
        "BUF:%u  TX:%lu  RX:%lu",
        rx_count,
        (unsigned long)tx_packets,
        (unsigned long)rx_packets);
    canvas_draw_str(canvas, 4, 44, line);

    canvas_draw_frame(canvas, 83, 18, 40, 24);
    canvas_draw_str_aligned(canvas, 103, 29, AlignCenter, AlignCenter, "ART");
    canvas_draw_str_aligned(canvas, 103, 37, AlignCenter, AlignCenter, "TBD");

    canvas_draw_str(canvas, 4, 54, status_line[0] ? status_line : "L/R node  OK hold PTT");
    canvas_draw_str(canvas, 4, 62, "Back exit");
}

static void walkie_input_callback(InputEvent* event, void* context) {
    WalkieApp* app = context;

    if(event->key == InputKeyOk) {
        if(event->type == InputTypePress) {
            walkie_lock(app);
            app->ptt_pressed = true;
            walkie_unlock(app);
            view_port_update(app->view_port);
        } else if(event->type == InputTypeRelease) {
            walkie_lock(app);
            app->ptt_pressed = false;
            walkie_unlock(app);
            view_port_update(app->view_port);
        }
        return;
    }

    if(event->type == InputTypeShort) {
        (void)furi_message_queue_put(app->input_queue, event, 0);
    }
}

static bool walkie_capture_runtime_state(
    WalkieApp* app,
    bool* running,
    bool* ptt_pressed,
    bool* config_dirty,
    uint8_t* node_id) {
    walkie_lock(app);
    *running = app->running;
    *ptt_pressed = app->ptt_pressed;
    *config_dirty = app->config_dirty;
    *node_id = app->node_id;
    app->config_dirty = false;
    walkie_unlock(app);

    return *running;
}

static void walkie_set_radio_state(WalkieApp* app, bool ready, const char* status) {
    walkie_lock(app);
    app->radio_ready = ready;
    snprintf(app->status_line, sizeof(app->status_line), "%s", status);
    walkie_unlock(app);
}

static void walkie_set_mode(WalkieApp* app, bool tx_mode) {
    walkie_lock(app);
    app->tx_mode = tx_mode;
    walkie_unlock(app);
}

static void walkie_set_rx_count(WalkieApp* app, uint16_t rx_count) {
    walkie_lock(app);
    app->rx_count = rx_count;
    walkie_unlock(app);
}

static void walkie_bump_tx_packets(WalkieApp* app) {
    walkie_lock(app);
    app->tx_packets++;
    walkie_unlock(app);
}

static void walkie_bump_rx_packets(WalkieApp* app) {
    walkie_lock(app);
    app->rx_packets++;
    walkie_unlock(app);
}

static bool walkie_radio_apply_node_id(Nrf24Soft* radio, uint8_t node_id) {
    const uint8_t peer_id = (uint8_t)(1u - (node_id & 1u));
    return nrf24_soft_configure_voice_link(
        radio, walkie_addrs[node_id & 1u], walkie_addrs[peer_id], WALKIE_RADIO_CHANNEL);
}

static void walkie_tx_audio_chunk(
    WalkieApp* app,
    Nrf24Soft* radio,
    FuriHalAdcHandle* adc_handle,
    uint8_t node_id,
    uint8_t* tx_seq) {
    AudioPacket packet = {0};
    packet.seq = (*tx_seq)++;
    packet.nonce = (uint8_t)(furi_get_tick() & 0xFFu);

    for(uint8_t i = 0; i < WALKIE_AUDIO_PAYLOAD; i++) {
        const uint16_t adc = furi_hal_adc_read(adc_handle, WALKIE_ADC_CHANNEL);
        packet.pcm[i] = (uint8_t)(adc >> 4);
        furi_delay_us(WALKIE_SAMPLE_PERIOD_US);
    }

    walkie_crypt_buffer(
        packet.pcm, WALKIE_AUDIO_PAYLOAD, node_id, packet.seq, packet.nonce);

    if(nrf24_soft_write_payload(radio, &packet, sizeof(packet))) {
        walkie_bump_tx_packets(app);
    }
}

static void walkie_rx_audio_step(WalkieApp* app, Nrf24Soft* radio, WalkieRxRing* ring, uint8_t node_id) {
    while(nrf24_soft_data_ready(radio)) {
        AudioPacket packet;
        if(!nrf24_soft_read_payload(radio, &packet, sizeof(packet))) {
            break;
        }

        walkie_crypt_buffer(
            packet.pcm, WALKIE_AUDIO_PAYLOAD, node_id, packet.seq, packet.nonce);

        for(uint8_t i = 0; i < WALKIE_AUDIO_PAYLOAD; i++) {
            walkie_rx_push(ring, packet.pcm[i]);
        }

        walkie_bump_rx_packets(app);
    }

    uint8_t sample = 128u;
    (void)walkie_rx_pop(ring, &sample);
    walkie_audio_output_sample(sample);
    walkie_set_rx_count(app, ring->count);
    furi_delay_us(WALKIE_SAMPLE_PERIOD_US);
}

static int32_t walkie_radio_worker(void* context) {
    WalkieApp* app = context;
    WalkieRxRing ring = {0};
    FuriHalAdcHandle* adc_handle = NULL;
    Nrf24Soft radio = {
        .ce = &gpio_ext_pb2,
        .csn = &gpio_ext_pc0,
        .sck = &gpio_ext_pb3,
        .mosi = &gpio_ext_pc1,
        .miso = &gpio_ext_pa6,
        .listening = false,
    };
    uint8_t active_node_id = 0xFFu;
    uint8_t tx_seq = 0u;
    bool in_tx = false;

    furi_hal_gpio_init(&gpio_ext_pc3, GpioModeAnalog, GpioPullNo, GpioSpeedLow);

    adc_handle = furi_hal_adc_acquire();
    furi_hal_adc_configure_ex(
        adc_handle,
        FuriHalAdcScale2500,
        FuriHalAdcClockSync64,
        FuriHalAdcOversampleNone,
        FuriHalAdcSamplingtime24_5);

    furi_hal_pwm_start(FuriHalPwmOutputIdTim1PA7, WALKIE_PWM_CARRIER_HZ, 50u);
    walkie_audio_output_sample(128u);

    if(!nrf24_soft_init(&radio)) {
        walkie_set_radio_state(app, false, "nRF24 not found");
        while(true) {
            bool running;
            bool ptt_pressed;
            bool config_dirty;
            uint8_t node_id;
            if(!walkie_capture_runtime_state(
                   app, &running, &ptt_pressed, &config_dirty, &node_id)) {
                break;
            }
            UNUSED(ptt_pressed);
            UNUSED(config_dirty);
            UNUSED(node_id);
            furi_delay_ms(100);
        }
        goto cleanup;
    }

    walkie_set_radio_state(app, true, "Radio ready");

    while(true) {
        bool running;
        bool ptt_pressed;
        bool config_dirty;
        uint8_t node_id;

        if(!walkie_capture_runtime_state(app, &running, &ptt_pressed, &config_dirty, &node_id)) {
            break;
        }

        node_id &= 1u;

        if(config_dirty || node_id != active_node_id) {
            if(!walkie_radio_apply_node_id(&radio, node_id)) {
                walkie_set_radio_state(app, false, "Radio config failed");
                furi_delay_ms(100);
                continue;
            }

            nrf24_soft_start_listening(&radio);
            ring.head = 0;
            ring.tail = 0;
            ring.count = 0;
            walkie_set_rx_count(app, 0);
            active_node_id = node_id;
            in_tx = false;
            walkie_set_mode(app, false);
            walkie_set_radio_state(app, true, "Radio ready");
        }

        if(ptt_pressed) {
            if(!in_tx) {
                nrf24_soft_stop_listening(&radio);
                in_tx = true;
                walkie_set_mode(app, true);
                walkie_set_status(app, "TX to peer");
            }
            walkie_tx_audio_chunk(app, &radio, adc_handle, active_node_id, &tx_seq);
        } else {
            if(in_tx) {
                nrf24_soft_start_listening(&radio);
                in_tx = false;
                walkie_set_mode(app, false);
                walkie_set_status(app, "RX listening");
            }
            walkie_rx_audio_step(app, &radio, &ring, active_node_id);
        }

        furi_thread_yield();
    }

cleanup:
    furi_hal_pwm_stop(FuriHalPwmOutputIdTim1PA7);
    if(adc_handle) {
        furi_hal_adc_release(adc_handle);
    }
    nrf24_soft_deinit(&radio);
    walkie_set_mode(app, false);
    walkie_set_radio_state(app, false, "Stopped");
    return 0;
}

static WalkieApp* walkie_app_alloc(void) {
    WalkieApp* app = malloc(sizeof(WalkieApp));
    memset(app, 0, sizeof(WalkieApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    app->input_queue = furi_message_queue_alloc(WALKIE_UI_QUEUE_LEN, sizeof(InputEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->radio_thread = furi_thread_alloc_ex("WalkieRadio", 4096, walkie_radio_worker, app);
    furi_thread_set_priority(app->radio_thread, FuriThreadPriorityLow);
    app->running = true;
    app->node_id = 0u;
    app->config_dirty = true;
    snprintf(app->status_line, sizeof(app->status_line), "Booting...");

    view_port_draw_callback_set(app->view_port, walkie_draw_callback, app);
    view_port_input_callback_set(app->view_port, walkie_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    return app;
}

static void walkie_app_free(WalkieApp* app) {
    if(!app) return;

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t flipper_walkie_app(void* p) {
    UNUSED(p);
    WalkieApp* app = walkie_app_alloc();

    furi_thread_start(app->radio_thread);

    while(true) {
        InputEvent event;
        if(furi_message_queue_get(app->input_queue, &event, 20) == FuriStatusOk) {
            if(event.key == InputKeyBack && event.type == InputTypeShort) {
                walkie_lock(app);
                app->running = false;
                walkie_unlock(app);
                break;
            }

            if(event.type == InputTypeShort && event.key == InputKeyLeft) {
                walkie_lock(app);
                app->node_id = (uint8_t)(app->node_id ? 0u : 1u);
                app->config_dirty = true;
                snprintf(app->status_line, sizeof(app->status_line), "NODE -> %u", app->node_id);
                walkie_unlock(app);
            } else if(event.type == InputTypeShort && event.key == InputKeyRight) {
                walkie_lock(app);
                app->node_id = (uint8_t)(app->node_id ? 0u : 1u);
                app->config_dirty = true;
                snprintf(app->status_line, sizeof(app->status_line), "NODE -> %u", app->node_id);
                walkie_unlock(app);
            }
        }

        view_port_update(app->view_port);
    }

    walkie_lock(app);
    app->running = false;
    app->ptt_pressed = false;
    walkie_unlock(app);

    furi_thread_join(app->radio_thread);
    furi_thread_free(app->radio_thread);
    walkie_app_free(app);
    return 0;
}

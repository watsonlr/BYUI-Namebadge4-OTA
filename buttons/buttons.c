#include "buttons.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"

#define TAG "buttons"

/* ── GPIO assignments ──────────────────────────────────────────────── */
#define GPIO_UP     17
#define GPIO_DOWN   16
#define GPIO_LEFT   14
#define GPIO_RIGHT  15
#define GPIO_A      38
#define GPIO_B      18

#define POLL_MS     10
#define DEBOUNCE_MS 20

static struct {
    gpio_num_t pin;
    button_t   bit;
} BTN_MAP[] = {
    { GPIO_UP,    BTN_UP    },
    { GPIO_DOWN,  BTN_DOWN  },
    { GPIO_LEFT,  BTN_LEFT  },
    { GPIO_RIGHT, BTN_RIGHT },
    { GPIO_A,     BTN_A     },
    { GPIO_B,     BTN_B     },
};
#define BTN_COUNT        (sizeof(BTN_MAP) / sizeof(BTN_MAP[0]))
#define BUTTON_QUEUE_LEN 8

static QueueHandle_t button_event_queue = NULL;

/* ── Public API ────────────────────────────────────────────────────── */

static void button_task(void *arg)
{
    button_t prev_state = BTN_NONE;
    ESP_LOGI(TAG, "button_task started (core %d)", xPortGetCoreID());
    for (;;) {
        button_t curr_state = BTN_NONE;
        for (int i = 0; i < (int)BTN_COUNT; i++) {
            if (gpio_get_level(BTN_MAP[i].pin) == 0) {
                curr_state |= BTN_MAP[i].bit;
            }
        }
        // Detect new presses
        for (int i = 0; i < (int)BTN_COUNT; i++) {
            bool was_pressed = (prev_state & BTN_MAP[i].bit);
            bool is_pressed = (curr_state & BTN_MAP[i].bit);
            if (!was_pressed && is_pressed) {
                // Debounce: wait 10ms, confirm still pressed
                vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
                if (gpio_get_level(BTN_MAP[i].pin) == 0) {
                    button_t event = BTN_MAP[i].bit;
                    ESP_LOGI(TAG, "Button pressed: 0x%02X, posting to queue (core %d)", event, xPortGetCoreID());
                    BaseType_t sent = xQueueSend(button_event_queue, &event, 0);
                    if (sent != pdTRUE) {
                        ESP_LOGW(TAG, "Queue full! Could not post event 0x%02X", event);
                    }
                }
            }
        }
        prev_state = curr_state;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void buttons_init(void)
{
    gpio_force_unhold_all();

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GPIO_UP)    | (1ULL << GPIO_DOWN)  |
                        (1ULL << GPIO_LEFT)   | (1ULL << GPIO_RIGHT) |
                        (1ULL << GPIO_A)      | (1ULL << GPIO_B),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    /* Allow pull-ups to charge before any reads. */
    vTaskDelay(pdMS_TO_TICKS(50));

    // Create event queue
    button_event_queue = xQueueCreate(BUTTON_QUEUE_LEN, sizeof(button_t));
    // Start polling task
    xTaskCreatePinnedToCore(button_task, "button_task", 2048, NULL, 10, NULL, 0);
    ESP_LOGI(TAG, "buttons ready (queue event mode)");
}

button_t buttons_read(void)
{
    button_t state = BTN_NONE;
    for (int i = 0; i < (int)BTN_COUNT; i++) {
        if (gpio_get_level(BTN_MAP[i].pin) == 0) {
            state |= BTN_MAP[i].bit;
        }
    }
    return state;
}

bool buttons_held(button_t mask, uint32_t duration_ms)
{
    uint32_t held_ms = 0;
    while (held_ms < duration_ms) {
        if ((buttons_read() & mask) != mask) return false;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        held_ms += POLL_MS;
    }
    return true;
}

// Flush any queued button events (phantom presses) after boot
void buttons_flush_events(void) {
    button_t event;
    int flushed = 0;
    while (xQueueReceive(button_event_queue, &event, 0) == pdTRUE) {
        flushed++;
    }
    ESP_LOGI(TAG, "buttons_flush_events: flushed %d events after boot", flushed);
}

// Wait for any button event (returns bitmask of pressed button, or BTN_NONE on timeout)
button_t buttons_wait_event(uint32_t timeout_ms)
{
    button_t event = BTN_NONE;
    ESP_LOGI(TAG, "buttons_wait_event: waiting for event (timeout=%u ms, core %d)", timeout_ms, xPortGetCoreID());
    if (xQueueReceive(button_event_queue, &event, timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY) == pdTRUE) {
        ESP_LOGI(TAG, "buttons_wait_event: got event 0x%02X (core %d)", event, xPortGetCoreID());
        return event;
    }
    ESP_LOGI(TAG, "buttons_wait_event: timeout (core %d)", xPortGetCoreID());
    return BTN_NONE;
}

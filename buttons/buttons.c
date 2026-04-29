#include "buttons.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "buttons"

/* ── GPIO assignments ──────────────────────────────────────────────── */
#define GPIO_UP     11
#define GPIO_DOWN   47
#define GPIO_LEFT   21
#define GPIO_RIGHT  10
#define GPIO_A      34
#define GPIO_B      33

#define POLL_MS       10   /* task wakes every 10 ms                     */
#define DEBOUNCE_MS   30   /* button must be held this long to register  */
#define GLITCH_MS      8   /* brief HIGH during press shorter than this is ignored */

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

/* ── Button task ────────────────────────────────────────────────────── *
 *
 * Per-button state machine — no blocking inside the poll loop:
 *
 *   IDLE            button reads HIGH (released)
 *   DEBOUNCING      button went LOW, waiting for DEBOUNCE_MS of continuous low
 *   FIRED           event posted; waiting for a full release before re-arming
 *
 * This eliminates both press-bounce and release-bounce re-triggers.
 * LP-range pads (GPIO 0-21) are sampled only at the top of each poll cycle
 * through gpio_get_level(), which is safe after gpio_config() has run in
 * the app context.  No gpio_get_level() is called inside a spin-wait, so
 * the task can never block on an unexpectedly-sticky LP pad.
 */
typedef enum { STATE_IDLE, STATE_DEBOUNCING, STATE_FIRED } btn_state_t;

static void button_task(void *arg)
{
    btn_state_t state[BTN_COUNT];
    int64_t     press_since[BTN_COUNT];   /* when button went LOW (press side)  */
    int64_t     release_since[BTN_COUNT]; /* when button went HIGH (release side) */

    for (int i = 0; i < (int)BTN_COUNT; i++) {
        state[i]         = STATE_IDLE;
        press_since[i]   = 0;
        release_since[i] = 0;
    }

    ESP_LOGI(TAG, "button_task started (core %d)", xPortGetCoreID());

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        int64_t now = esp_timer_get_time();   /* microseconds */

        for (int i = 0; i < (int)BTN_COUNT; i++) {
            bool low = (gpio_get_level(BTN_MAP[i].pin) == 0);

            switch (state[i]) {
            case STATE_IDLE:
                if (low) {
                    state[i]         = STATE_DEBOUNCING;
                    press_since[i]   = now;
                    release_since[i] = 0;
                }
                break;

            case STATE_DEBOUNCING:
                if (!low) {
                    /* Brief HIGH glitch — only abort if it persists past GLITCH_MS */
                    if (release_since[i] == 0) release_since[i] = now;
                    else if ((now - release_since[i]) >= (int64_t)GLITCH_MS * 1000) {
                        state[i]         = STATE_IDLE;
                        release_since[i] = 0;
                    }
                } else {
                    release_since[i] = 0;  /* back low — clear glitch timer */
                }
                if (low && (now - press_since[i]) >= (int64_t)DEBOUNCE_MS * 1000) {
                    /* Held long enough — fire event */
                    button_t event = BTN_MAP[i].bit;
                    ESP_LOGI(TAG, "Button event: 0x%02X (core %d)", event, xPortGetCoreID());
                    if (xQueueSend(button_event_queue, &event, 0) != pdTRUE)
                        ESP_LOGW(TAG, "Queue full — dropped 0x%02X", event);
                    state[i] = STATE_FIRED;
                }
                break;

            case STATE_FIRED:
                /* Require the button to stay HIGH for DEBOUNCE_MS before
                 * re-arming.  Any LOW bounce resets the release timer so
                 * a second event cannot fire from the same physical press. */
                if (low) {
                    release_since[i] = 0;
                } else {
                    if (release_since[i] == 0) release_since[i] = now;
                    else if ((now - release_since[i]) >= (int64_t)DEBOUNCE_MS * 1000) {
                        state[i]         = STATE_IDLE;
                        release_since[i] = 0;
                    }
                }
                break;
            }
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

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

    button_event_queue = xQueueCreate(BUTTON_QUEUE_LEN, sizeof(button_t));
    xTaskCreatePinnedToCore(button_task, "button_task", 4096, NULL, 10, NULL, 0);
    ESP_LOGI(TAG, "buttons ready");
}

button_t buttons_read(void)
{
    button_t state = BTN_NONE;
    for (int i = 0; i < (int)BTN_COUNT; i++) {
        if (gpio_get_level(BTN_MAP[i].pin) == 0)
            state |= BTN_MAP[i].bit;
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

void buttons_flush_events(void)
{
    button_t event;
    int flushed = 0;
    while (xQueueReceive(button_event_queue, &event, 0) == pdTRUE)
        flushed++;
    ESP_LOGI(TAG, "buttons_flush_events: flushed %d events", flushed);
}

button_t buttons_wait_event(uint32_t timeout_ms)
{
    button_t event = BTN_NONE;
    TickType_t ticks = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    if (xQueueReceive(button_event_queue, &event, ticks) == pdTRUE)
        return event;
    return BTN_NONE;
}

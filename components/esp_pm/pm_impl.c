/*
 * SPDX-FileCopyrightText: 2016-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <sys/param.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_pm.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_clk_tree.h"

#include "esp_private/crosscore_int.h"

#include "soc/rtc.h"
#include "hal/uart_ll.h"
#include "hal/uart_types.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#if CONFIG_XTENSA_TIMER
#include "xtensa/core-macros.h"
#endif

#if SOC_SPI_MEM_SUPPORT_TIME_TUNING
#include "esp_private/mspi_timing_tuning.h"
#endif

#include "esp_private/pm_impl.h"
#include "esp_private/pm_trace.h"
#include "esp_private/esp_timer_private.h"
#include "esp_private/esp_clk.h"
#include "esp_private/sleep_cpu.h"
#include "esp_private/sleep_gpio.h"
#include "esp_private/sleep_modem.h"
#include "esp_sleep.h"
#include <zephyr/kernel.h>

#include "sdkconfig.h"

#ifdef CONFIG_XTENSA_TIMER
/* CCOMPARE update timeout, in CPU cycles. Any value above ~600 cycles will work
 * for the purpose of detecting a deadlock.
 */
#define CCOMPARE_UPDATE_TIMEOUT 1000000

/* When changing CCOMPARE, don't allow changes if the difference is less
 * than this. This is to prevent setting CCOMPARE below CCOUNT.
 */
#define CCOMPARE_MIN_CYCLES_IN_FUTURE 1000
#endif // CONFIG_XTENSA_TIMER

/* When light sleep is used, wake this number of microseconds earlier than
 * the next tick.
 */
#define LIGHT_SLEEP_EARLY_WAKEUP_US 100

#if CONFIG_IDF_TARGET_ESP32
/* Minimal divider at which REF_CLK_FREQ can be obtained */
#define REF_CLK_DIV_MIN 10
#elif CONFIG_IDF_TARGET_ESP32S2
/* Minimal divider at which REF_CLK_FREQ can be obtained */
#define REF_CLK_DIV_MIN 2
#elif CONFIG_IDF_TARGET_ESP32S3
/* Minimal divider at which REF_CLK_FREQ can be obtained */
#define REF_CLK_DIV_MIN 2         // TODO: IDF-5660
#elif CONFIG_IDF_TARGET_ESP32C3
#define REF_CLK_DIV_MIN 2
#elif CONFIG_IDF_TARGET_ESP32C2
#define REF_CLK_DIV_MIN 2
#elif CONFIG_IDF_TARGET_ESP32C6
#define REF_CLK_DIV_MIN 2
#elif CONFIG_IDF_TARGET_ESP32H2
#define REF_CLK_DIV_MIN 2
#endif

#ifdef CONFIG_PM_PROFILING
#define WITH_PROFILING
#endif

#define ENTER_CRITICAL(lock_ptr)    do { *lock_ptr = irq_lock(); } while(0)
#define EXIT_CRITICAL(lock_ptr)     irq_unlock(*lock_ptr);

static int s_switch_lock;
/* The following state variables are protected using s_switch_lock: */
/* Current sleep mode; When switching, contains old mode until switch is complete */
static pm_mode_t s_mode = PM_MODE_CPU_MAX;
/* True when switch is in progress */
static volatile bool s_is_switching;
/* Number of times each mode was locked */
static size_t s_mode_lock_counts[PM_MODE_COUNT];
/* Bit mask of locked modes. BIT(i) is set iff s_mode_lock_counts[i] > 0. */
static uint32_t s_mode_mask;

/* A flag indicating that Idle hook has run on a given CPU;
 * Next interrupt on the same CPU will take s_rtos_lock_handle.
 */
static bool s_core_idle[CONFIG_MP_MAX_NUM_CPUS];

/* When no RTOS tasks are active, these locks are released to allow going into
 * a lower power mode. Used by ISR hook and idle hook.
 */
static esp_pm_lock_handle_t s_rtos_lock_handle[CONFIG_MP_MAX_NUM_CPUS];

/* Lookup table of CPU frequency configs to be used in each mode.
 * Initialized by esp_pm_impl_init and modified by esp_pm_configure.
 */
static rtc_cpu_freq_config_t s_cpu_freq_by_mode[PM_MODE_COUNT];

/* Whether automatic light sleep is enabled */
static bool s_light_sleep_en = false;

/* When configuration is changed, current frequency may not match the
 * newly configured frequency for the current mode. This is an indicator
 * to the mode switch code to get the actual current frequency instead of
 * relying on the current mode.
 */
static bool s_config_changed = false;

#ifdef WITH_PROFILING
/* Time, in microseconds, spent so far in each mode */
static pm_time_t s_time_in_mode[PM_MODE_COUNT];
/* Timestamp, in microseconds, when the mode switch last happened */
static pm_time_t s_last_mode_change_time;
/* User-readable mode names, used by esp_pm_impl_dump_stats */
static const char* s_mode_names[] = {
        "SLEEP",
        "APB_MIN",
        "APB_MAX",
        "CPU_MAX"
};
static uint32_t s_light_sleep_counts, s_light_sleep_reject_counts;
#endif // WITH_PROFILING

#ifdef CONFIG_XTENSA_TIMER
/* Indicates to the ISR hook that CCOMPARE needs to be updated on the given CPU.
 * Used in conjunction with cross-core interrupt to update CCOMPARE on the other CPU.
 */
static volatile bool s_need_update_ccompare[CONFIG_MP_MAX_NUM_CPUS];

/* Divider and multiplier used to adjust (ccompare - ccount) duration.
 * Only set to non-zero values when switch is in progress.
 */
static uint32_t s_ccount_div;
static uint32_t s_ccount_mul;

static void update_ccompare(void);
#endif // CONFIG_XTENSA_TIMER

static const char* TAG = "pm";

static void do_switch(pm_mode_t new_mode);
static void leave_idle(void);
static void on_freq_update(uint32_t old_ticks_per_us, uint32_t ticks_per_us);

pm_mode_t esp_pm_impl_get_mode(esp_pm_lock_type_t type, int arg)
{
    (void) arg;
    if (type == ESP_PM_CPU_FREQ_MAX) {
        return PM_MODE_CPU_MAX;
    } else if (type == ESP_PM_APB_FREQ_MAX) {
        return PM_MODE_APB_MAX;
    } else if (type == ESP_PM_NO_LIGHT_SLEEP) {
        return PM_MODE_APB_MIN;
    } else {
        // unsupported mode
        abort();
    }
}

static esp_err_t esp_pm_sleep_configure(const void *vconfig)
{
    esp_err_t err = ESP_OK;
    const esp_pm_config_t* config = (const esp_pm_config_t*) vconfig;

#if SOC_PM_SUPPORT_CPU_PD
    err = sleep_cpu_configure(config->light_sleep_enable);
    if (err != ESP_OK) {
        return err;
    }
#endif

    err = sleep_modem_configure(config->max_freq_mhz, config->min_freq_mhz, config->light_sleep_enable);
    return err;
}

esp_err_t esp_pm_configure(const void* vconfig)
{
#ifndef CONFIG_PM_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#endif

    const esp_pm_config_t* config = (const esp_pm_config_t*) vconfig;

    int min_freq_mhz = config->min_freq_mhz;
    int max_freq_mhz = config->max_freq_mhz;

    if (min_freq_mhz > max_freq_mhz) {
        return ESP_ERR_INVALID_ARG;
    }

    rtc_cpu_freq_config_t freq_config;
    if (!rtc_clk_cpu_freq_mhz_to_config(min_freq_mhz, &freq_config)) {
        ESP_LOGW(TAG, "invalid min_freq_mhz value (%d)", min_freq_mhz);
        return ESP_ERR_INVALID_ARG;
    }

    int xtal_freq_mhz = esp_clk_xtal_freq() / MHZ(1);
    if (min_freq_mhz < xtal_freq_mhz && min_freq_mhz * MHZ(1) / REF_CLK_FREQ < REF_CLK_DIV_MIN) {
        ESP_LOGW(TAG, "min_freq_mhz should be >= %d", REF_CLK_FREQ * REF_CLK_DIV_MIN / MHZ(1));
        return ESP_ERR_INVALID_ARG;
    }

    if (!rtc_clk_cpu_freq_mhz_to_config(max_freq_mhz, &freq_config)) {
        ESP_LOGW(TAG, "invalid max_freq_mhz value (%d)", max_freq_mhz);
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_IDF_TARGET_ESP32
    int apb_max_freq = max_freq_mhz; /* CPU frequency in APB_MAX mode */
    if (max_freq_mhz == 240) {
        /* We can't switch between 240 and 80/160 without disabling PLL,
         * so use 240MHz CPU frequency when 80MHz APB frequency is requested.
         */
        apb_max_freq = 240;
    } else if (max_freq_mhz == 160 || max_freq_mhz == 80) {
        /* Otherwise, can use 80MHz
         * CPU frequency when 80MHz APB frequency is requested.
         */
        apb_max_freq = 80;
    }
#else
    /* Maximum SOC APB clock frequency is 40 MHz, maximum Modem (WiFi,
     * Bluetooth, etc..) APB clock frequency is 80 MHz */
    int apb_clk_freq = esp_clk_apb_freq() / MHZ(1);
#if CONFIG_ESP_WIFI_ENABLED || CONFIG_BT_ENABLED || CONFIG_IEEE802154_ENABLED
    apb_clk_freq = MAX(apb_clk_freq, MODEM_REQUIRED_MIN_APB_CLK_FREQ / MHZ(1));
#endif
    int apb_max_freq = MIN(max_freq_mhz, apb_clk_freq); /* CPU frequency in APB_MAX mode */
#endif

    apb_max_freq = MAX(apb_max_freq, min_freq_mhz);

    ESP_LOGI(TAG, "Frequency switching config: "
                  "CPU_MAX: %d, APB_MAX: %d, APB_MIN: %d, Light sleep: %s",
                  max_freq_mhz,
                  apb_max_freq,
                  min_freq_mhz,
                  config->light_sleep_enable ? "ENABLED" : "DISABLED");

    ENTER_CRITICAL(&s_switch_lock);

    bool res __attribute__((unused));
    res = rtc_clk_cpu_freq_mhz_to_config(max_freq_mhz, &s_cpu_freq_by_mode[PM_MODE_CPU_MAX]);
    assert(res);
    res = rtc_clk_cpu_freq_mhz_to_config(apb_max_freq, &s_cpu_freq_by_mode[PM_MODE_APB_MAX]);
    assert(res);
    res = rtc_clk_cpu_freq_mhz_to_config(min_freq_mhz, &s_cpu_freq_by_mode[PM_MODE_APB_MIN]);
    assert(res);
    s_cpu_freq_by_mode[PM_MODE_LIGHT_SLEEP] = s_cpu_freq_by_mode[PM_MODE_APB_MIN];
    s_light_sleep_en = config->light_sleep_enable;
    s_config_changed = true;
    EXIT_CRITICAL(&s_switch_lock);

    esp_pm_sleep_configure(config);

    return ESP_OK;
}

esp_err_t esp_pm_get_configuration(void* vconfig)
{
    if (vconfig == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_pm_config_t* config = (esp_pm_config_t*) vconfig;

    ENTER_CRITICAL(&s_switch_lock);
    config->light_sleep_enable = s_light_sleep_en;
    config->max_freq_mhz = s_cpu_freq_by_mode[PM_MODE_CPU_MAX].freq_mhz;
    config->min_freq_mhz = s_cpu_freq_by_mode[PM_MODE_APB_MIN].freq_mhz;
    EXIT_CRITICAL(&s_switch_lock);

    return ESP_OK;
}

static pm_mode_t IRAM_ATTR get_lowest_allowed_mode(void)
{
    /* TODO: optimize using ffs/clz */
    if (s_mode_mask >= BIT(PM_MODE_CPU_MAX)) {
        return PM_MODE_CPU_MAX;
    } else if (s_mode_mask >= BIT(PM_MODE_APB_MAX)) {
        return PM_MODE_APB_MAX;
    } else if (s_mode_mask >= BIT(PM_MODE_APB_MIN) || !s_light_sleep_en) {
        return PM_MODE_APB_MIN;
    } else {
        return PM_MODE_LIGHT_SLEEP;
    }
}

void IRAM_ATTR esp_pm_impl_switch_mode(pm_mode_t mode,
        pm_mode_switch_t lock_or_unlock, pm_time_t now)
{
    bool need_switch = false;
    uint32_t mode_mask = BIT(mode);
    ENTER_CRITICAL(&s_switch_lock);
    uint32_t count;
    if (lock_or_unlock == MODE_LOCK) {
        count = ++s_mode_lock_counts[mode];
    } else {
        count = s_mode_lock_counts[mode]--;
    }
    if (count == 1) {
        if (lock_or_unlock == MODE_LOCK) {
            s_mode_mask |= mode_mask;
        } else {
            s_mode_mask &= ~mode_mask;
        }
        need_switch = true;
    }

    pm_mode_t new_mode = s_mode;
    if (need_switch) {
        new_mode = get_lowest_allowed_mode();
#ifdef WITH_PROFILING
        if (s_last_mode_change_time != 0) {
            pm_time_t diff = now - s_last_mode_change_time;
            s_time_in_mode[s_mode] += diff;
        }
        s_last_mode_change_time = now;
#endif // WITH_PROFILING
    }
    EXIT_CRITICAL(&s_switch_lock);
    if (need_switch) {
        do_switch(new_mode);
    }
}

/**
 * @brief Update clock dividers in esp_timer and adjust CCOMPARE
 * values on both CPUs.
 * @param old_ticks_per_us old CPU frequency
 * @param ticks_per_us new CPU frequency
 */
static void IRAM_ATTR on_freq_update(uint32_t old_ticks_per_us, uint32_t ticks_per_us)
{
    uint32_t old_apb_ticks_per_us = MIN(old_ticks_per_us, 80);
    uint32_t apb_ticks_per_us = MIN(ticks_per_us, 80);
    /* Update APB frequency value used by the timer */
    if (old_apb_ticks_per_us != apb_ticks_per_us) {
        esp_timer_private_update_apb_freq(apb_ticks_per_us);
    }

#ifdef CONFIG_XTENSA_TIMER
#ifdef XT_RTOS_TIMER_INT
    /* Calculate new tick divisor */
    _xt_tick_divisor = ticks_per_us * MHZ(1) / XT_TICK_PER_SEC;
#endif

    int core_id = esp_cpu_get_core_id();
    if (s_rtos_lock_handle[core_id] != NULL) {
        ESP_PM_TRACE_ENTER(CCOMPARE_UPDATE, core_id);
        /* ccount_div and ccount_mul are used in esp_pm_impl_update_ccompare
         * to calculate new CCOMPARE value.
         */
        s_ccount_div = old_ticks_per_us;
        s_ccount_mul = ticks_per_us;

        /* Update CCOMPARE value on this CPU */
        update_ccompare();

#if CONFIG_MP_MAX_NUM_CPUS == 2
        /* Send interrupt to the other CPU to update CCOMPARE value */
        int other_core_id = (core_id == 0) ? 1 : 0;

        s_need_update_ccompare[other_core_id] = true;
        esp_crosscore_int_send_freq_switch(other_core_id);

        int timeout = 0;
        while (s_need_update_ccompare[other_core_id]) {
            if (++timeout == CCOMPARE_UPDATE_TIMEOUT) {
                assert(false && "failed to update CCOMPARE, possible deadlock");
            }
        }
#endif // CONFIG_MP_MAX_NUM_CPUS == 2

        s_ccount_mul = 0;
        s_ccount_div = 0;
        ESP_PM_TRACE_EXIT(CCOMPARE_UPDATE, core_id);
    }
#endif // CONFIG_XTENSA_TIMER
}

/**
 * Perform the switch to new power mode.
 * Currently only changes the CPU frequency and adjusts clock dividers.
 * No light sleep yet.
 * @param new_mode mode to switch to
 */
static void IRAM_ATTR do_switch(pm_mode_t new_mode)
{
    const int core_id = esp_cpu_get_core_id();

    do {
        ENTER_CRITICAL(&s_switch_lock);
        if (!s_is_switching) {
            break;
        }
#ifdef CONFIG_XTENSA_TIMER
        if (s_need_update_ccompare[core_id]) {
            s_need_update_ccompare[core_id] = false;
        }
#endif
        EXIT_CRITICAL(&s_switch_lock);
    } while (true);
    if (new_mode == s_mode) {
        EXIT_CRITICAL(&s_switch_lock);
        return;
    }
    s_is_switching = true;
    bool config_changed = s_config_changed;
    s_config_changed = false;
    EXIT_CRITICAL(&s_switch_lock);

    rtc_cpu_freq_config_t new_config = s_cpu_freq_by_mode[new_mode];
    rtc_cpu_freq_config_t old_config;

    if (!config_changed) {
        old_config = s_cpu_freq_by_mode[s_mode];
    } else {
        rtc_clk_cpu_freq_get_config(&old_config);
    }

    if (new_config.freq_mhz != old_config.freq_mhz) {
        uint32_t old_ticks_per_us = old_config.freq_mhz;
        uint32_t new_ticks_per_us = new_config.freq_mhz;

        bool switch_down = new_ticks_per_us < old_ticks_per_us;

        ESP_PM_TRACE_ENTER(FREQ_SWITCH, core_id);
        if (switch_down) {
            on_freq_update(old_ticks_per_us, new_ticks_per_us);
        }
       if (new_config.source == SOC_CPU_CLK_SRC_PLL) {
            rtc_clk_cpu_freq_set_config_fast(&new_config);
#if SOC_SPI_MEM_SUPPORT_TIME_TUNING
            mspi_timing_change_speed_mode_cache_safe(false);
#endif
        } else {
#if SOC_SPI_MEM_SUPPORT_TIME_TUNING
            mspi_timing_change_speed_mode_cache_safe(true);
#endif
            rtc_clk_cpu_freq_set_config_fast(&new_config);
        }
        if (!switch_down) {
            on_freq_update(old_ticks_per_us, new_ticks_per_us);
        }
        ESP_PM_TRACE_EXIT(FREQ_SWITCH, core_id);
    }

    ENTER_CRITICAL(&s_switch_lock);
    s_mode = new_mode;
    s_is_switching = false;
    EXIT_CRITICAL(&s_switch_lock);
}

#ifdef CONFIG_XTENSA_TIMER
/**
 * @brief Calculate new CCOMPARE value based on s_ccount_{mul,div}
 *
 * Adjusts CCOMPARE value so that the interrupt happens at the same time as it
 * would happen without the frequency change.
 * Assumes that the new_frequency = old_frequency * s_ccount_mul / s_ccount_div.
 */
static void IRAM_ATTR update_ccompare(void)
{
#if CONFIG_PM_UPDATE_CCOMPARE_HLI_WORKAROUND
    /* disable level 4 and below */
    uint32_t irq_status = XTOS_SET_INTLEVEL(XCHAL_DEBUGLEVEL - 2);
#endif
    uint32_t ccount = esp_cpu_get_cycle_count();
    uint32_t ccompare = XTHAL_GET_CCOMPARE(XT_TIMER_INDEX);
    if ((ccompare - CCOMPARE_MIN_CYCLES_IN_FUTURE) - ccount < UINT32_MAX / 2) {
        uint32_t diff = ccompare - ccount;
        uint32_t diff_scaled = (diff * s_ccount_mul + s_ccount_div - 1) / s_ccount_div;
        if (diff_scaled < _xt_tick_divisor) {
            uint32_t new_ccompare = ccount + diff_scaled;
            XTHAL_SET_CCOMPARE(XT_TIMER_INDEX, new_ccompare);
        }
    }
#if CONFIG_PM_UPDATE_CCOMPARE_HLI_WORKAROUND
    XTOS_RESTORE_INTLEVEL(irq_status);
#endif
}
#endif // CONFIG_XTENSA_TIMER

static void IRAM_ATTR leave_idle(void)
{
    int core_id = esp_cpu_get_core_id();
    if (s_core_idle[core_id]) {
        // TODO: possible optimization: raise frequency here first
        esp_pm_lock_acquire(s_rtos_lock_handle[core_id]);
        s_core_idle[core_id] = false;
    }
}

#ifdef WITH_PROFILING
void esp_pm_impl_dump_stats(FILE* out)
{
    pm_time_t time_in_mode[PM_MODE_COUNT];

    ENTER_CRITICAL(&s_switch_lock);
    memcpy(time_in_mode, s_time_in_mode, sizeof(time_in_mode));
    pm_time_t last_mode_change_time = s_last_mode_change_time;
    pm_mode_t cur_mode = s_mode;
    pm_time_t now = pm_get_time();
    bool light_sleep_en = s_light_sleep_en;
    uint32_t light_sleep_counts = s_light_sleep_counts;
    uint32_t light_sleep_reject_counts = s_light_sleep_reject_counts;
    EXIT_CRITICAL(&s_switch_lock);

    time_in_mode[cur_mode] += now - last_mode_change_time;

    fprintf(out, "\nMode stats:\n");
    fprintf(out, "%-8s  %-10s  %-10s  %-10s\n", "Mode", "CPU_freq", "Time(us)", "Time(%)");
    for (int i = 0; i < PM_MODE_COUNT; ++i) {
        if (i == PM_MODE_LIGHT_SLEEP && !light_sleep_en) {
            /* don't display light sleep mode if it's not enabled */
            continue;
        }
        fprintf(out, "%-8s  %-3"PRIu32"M%-7s %-10lld  %-2d%%\n",
                s_mode_names[i],
                s_cpu_freq_by_mode[i].freq_mhz,
                "",                                     //Empty space to align columns
                time_in_mode[i],
                (int) (time_in_mode[i] * 100 / now));
    }
    if (light_sleep_en){
        fprintf(out, "\nSleep stats:\n");
        fprintf(out, "light_sleep_counts:%ld  light_sleep_reject_counts:%ld\n", light_sleep_counts, light_sleep_reject_counts);
    }
}
#endif // WITH_PROFILING

int esp_pm_impl_get_cpu_freq(pm_mode_t mode)
{
    int freq_mhz;
    if (mode >= PM_MODE_LIGHT_SLEEP && mode < PM_MODE_COUNT) {
        ENTER_CRITICAL(&s_switch_lock);
        freq_mhz = s_cpu_freq_by_mode[mode].freq_mhz;
        EXIT_CRITICAL(&s_switch_lock);
    } else {
        abort();
    }
    return freq_mhz;
}

void esp_pm_impl_init(void)
{
#if defined(CONFIG_ESP_CONSOLE_UART)
    //This clock source should be a source which won't be affected by DFS
    uart_sclk_t clk_source = UART_SCLK_DEFAULT;
#if SOC_UART_SUPPORT_REF_TICK
    clk_source = UART_SCLK_REF_TICK;
#elif SOC_UART_SUPPORT_XTAL_CLK
    clk_source = UART_SCLK_XTAL;
#else
    #error "No UART clock source is aware of DFS"
#endif // SOC_UART_SUPPORT_xxx
    while(!uart_ll_is_tx_idle(UART_LL_GET_HW(CONFIG_ESP_CONSOLE_UART_NUM)));
    /* When DFS is enabled, override system setting and use REFTICK as UART clock source */
    uart_ll_set_sclk(UART_LL_GET_HW(CONFIG_ESP_CONSOLE_UART_NUM), clk_source);

    uint32_t sclk_freq;
    esp_err_t err = esp_clk_tree_src_get_freq_hz((soc_module_clk_t)clk_source,
            ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &sclk_freq);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "could not get UART clock frequency");
        return;
    }

    uart_ll_set_baudrate(UART_LL_GET_HW(CONFIG_ESP_CONSOLE_UART_NUM), CONFIG_ESP_CONSOLE_UART_BAUDRATE, sclk_freq);
#endif // CONFIG_ESP_CONSOLE_UART

#ifdef CONFIG_PM_TRACE
    esp_pm_trace_init();
#endif

    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "rtos0",
            &s_rtos_lock_handle[0]));
    ESP_ERROR_CHECK(esp_pm_lock_acquire(s_rtos_lock_handle[0]));

#if CONFIG_MP_MAX_NUM_CPUS == 2
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "rtos1",
            &s_rtos_lock_handle[1]));
    ESP_ERROR_CHECK(esp_pm_lock_acquire(s_rtos_lock_handle[1]));
#endif // CONFIG_MP_MAX_NUM_CPUS == 2

    /* Configure all modes to use the default CPU frequency.
     * This will be modified later by a call to esp_pm_configure.
     */
    rtc_cpu_freq_config_t default_config;
    if (!rtc_clk_cpu_freq_mhz_to_config(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, &default_config)) {
        assert(false && "unsupported frequency");
    }
    for (size_t i = 0; i < PM_MODE_COUNT; ++i) {
        s_cpu_freq_by_mode[i] = default_config;
    }

#ifdef CONFIG_PM_DFS_INIT_AUTO
    int xtal_freq_mhz = esp_clk_xtal_freq() / MHZ(1);
    esp_pm_config_t cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = xtal_freq_mhz,
    };

    esp_pm_configure(&cfg);
#endif //CONFIG_PM_DFS_INIT_AUTO
}

void esp_pm_impl_idle_hook(void)
{
    int core_id = esp_cpu_get_core_id();

    k_sched_lock();
    ENTER_CRITICAL(&s_switch_lock);
    if (!s_core_idle[core_id]) {
        esp_pm_lock_release(s_rtos_lock_handle[core_id]);
        s_core_idle[core_id] = true;
    }
    EXIT_CRITICAL(&s_switch_lock);
    k_sched_unlock();

    ESP_PM_TRACE_ENTER(IDLE, core_id);
}

void IRAM_ATTR esp_pm_impl_isr_hook(void)
{
    int core_id = esp_cpu_get_core_id();
    ESP_PM_TRACE_ENTER(ISR_HOOK, core_id);
    /* Prevent higher level interrupts (than the one this function was called from)
     * from happening in this section, since they will also call into esp_pm_impl_isr_hook.
     */
    ENTER_CRITICAL(&s_switch_lock);
#if defined(CONFIG_XTENSA_TIMER) && (CONFIG_MP_MAX_NUM_CPUS == 2)
    if (s_need_update_ccompare[core_id]) {
        update_ccompare();
        s_need_update_ccompare[core_id] = false;
    } else {
        leave_idle();
    }
#else
    leave_idle();
#endif // CONFIG_XTENSA_TIMER && CONFIG_MP_MAX_NUM_CPUS == 2
    EXIT_CRITICAL(&s_switch_lock);
    ESP_PM_TRACE_EXIT(ISR_HOOK, core_id);
}

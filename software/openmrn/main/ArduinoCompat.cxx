// The Arduino calls the hardware drivers make, over ESP-IDF.
//
// See Arduino.h and Wire.h for what this is and why it exists. Nothing here is
// general: every function is present because Channels, Occupancy, Turnouts,
// IoPins or Expander calls it, and each behaves the way the Arduino-ESP32 3.x
// core behaves in the cases those files exercise.
//
//   Uncommon Models — https://uncommonmodels.com

#include "Arduino.h"
#include "Wire.h"

#include "board.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "freertos_drivers/esp32/Esp32Gpio.hxx"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "compat";

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint32_t micros(void)
{
    return (uint32_t)esp_timer_get_time();
}

void delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void delayMicroseconds(uint32_t us)
{
    // A busy-wait, as on Arduino: these are a few hundred microseconds at most
    // and far shorter than a scheduler tick.
    esp_rom_delay_us(us);
}

// ---------------------------------------------------------------------------
// GPIO
//
// The board's directly-attached digital pins go through OpenMRN's Esp32Gpio
// rather than straight to ESP-IDF, so the pin definitions, the safe power-on
// level and the compile-time checks on pin validity are the library's. That
// needs patch 0005, which teaches Esp32Gpio the C6: without it the ESP32
// fallback rejects GPIO 6, 7 and 9, which are three of the four nFAULT lines.
//
// The dispatch below exists because the hardware drivers index pins at run time
// (PIN_NFAULT[i]) while Esp32Gpio is a compile-time template. Anything not on
// this board's map falls back to a plain gpio_config().
// ---------------------------------------------------------------------------

// Only on the C6, which is the board. These pin numbers are this board's, and
// on another SoC they mean nothing: on the classic ESP32, GPIO6-11 are the
// flash and Esp32Gpio's static_assert rightly refuses them. The fallback target
// therefore uses the plain ESP-IDF path below.
#if defined(CONFIG_IDF_TARGET_ESP32C6)
#define FEMTO_USE_ESP32GPIO 1

// Channel A's direction line; B, C and D are on the expander.
GPIO_PIN(DirA, GpioOutputSafeLow, PIN_DIR_A);

// DRV8874 nFAULT, open drain and active low, so each needs its pull-up.
GPIO_PIN(FaultA, GpioInputPU, 6);
GPIO_PIN(FaultB, GpioInputPU, 7);
GPIO_PIN(FaultC, GpioInputPU, 9);
GPIO_PIN(FaultD, GpioInputPU, 23);
#endif // CONFIG_IDF_TARGET_ESP32C6

void pinMode(int pin, uint8_t mode)
{
#if FEMTO_USE_ESP32GPIO
    // The board's own pins, set up the way OpenMRN defines them.
    if (pin == PIN_DIR_A)
    {
        DirA_Pin::hw_init();
        return;
    }
    if (pin == PIN_NFAULT[0]) { FaultA_Pin::hw_init(); return; }
    if (pin == PIN_NFAULT[1]) { FaultB_Pin::hw_init(); return; }
    if (pin == PIN_NFAULT[2]) { FaultC_Pin::hw_init(); return; }
    if (pin == PIN_NFAULT[3]) { FaultD_Pin::hw_init(); return; }
#endif

    gpio_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.intr_type = GPIO_INTR_DISABLE;

    if (mode == OUTPUT)
    {
        cfg.mode = GPIO_MODE_OUTPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    }
    else
    {
        cfg.mode = GPIO_MODE_INPUT;
        // INPUT_PULLUP is the only pulled variant the drivers ask for; the
        // DRV8874's nFAULT is open drain and needs it.
        cfg.pull_up_en = (mode == INPUT_PULLUP) ? GPIO_PULLUP_ENABLE
                                                : GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    }

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "pinMode(%d, %u) failed: %s", pin, mode,
            esp_err_to_name(err));
    }
}

void digitalWrite(int pin, uint8_t value)
{
#if FEMTO_USE_ESP32GPIO
    if (pin == PIN_DIR_A)
    {
        DirA_Pin::set(value != 0);
        return;
    }
#endif
    gpio_set_level((gpio_num_t)pin, value ? 1 : 0);
}

int digitalRead(int pin)
{
#if FEMTO_USE_ESP32GPIO
    if (pin == PIN_NFAULT[0]) { return FaultA_Pin::get() ? 1 : 0; }
    if (pin == PIN_NFAULT[1]) { return FaultB_Pin::get() ? 1 : 0; }
    if (pin == PIN_NFAULT[2]) { return FaultC_Pin::get() ? 1 : 0; }
    if (pin == PIN_NFAULT[3]) { return FaultD_Pin::get() ? 1 : 0; }
    if (pin == PIN_DIR_A) { return DirA_Pin::instance()->read() ? 1 : 0; }
#endif
    return gpio_get_level((gpio_num_t)pin);
}

// ---------------------------------------------------------------------------
// ADC
//
// The four IPROPI current-sense lines. Channels::currentMilliamps and Occupancy
// both work in raw counts against a 12-bit full scale, so the reading must be
// the raw conversion, not a calibrated millivolt figure.
// ---------------------------------------------------------------------------

static adc_oneshot_unit_handle_t adcUnit[SOC_ADC_PERIPH_NUM];
static bool adcChannelReady[SOC_ADC_PERIPH_NUM][SOC_ADC_MAX_CHANNEL_NUM];

/// Brings up the ADC unit and channel for `pin` the first time it is read.
/// @return true if the pin can be sampled.
static bool adc_prepare(int pin, adc_unit_t *unit, adc_channel_t *channel)
{
    if (adc_oneshot_io_to_channel(pin, unit, channel) != ESP_OK)
    {
        return false;
    }
    if (*unit >= SOC_ADC_PERIPH_NUM || *channel >= SOC_ADC_MAX_CHANNEL_NUM)
    {
        return false;
    }

    if (adcUnit[*unit] == NULL)
    {
        adc_oneshot_unit_init_cfg_t unitCfg;
        memset(&unitCfg, 0, sizeof(unitCfg));
        unitCfg.unit_id = *unit;
        unitCfg.ulp_mode = ADC_ULP_MODE_DISABLE;
        if (adc_oneshot_new_unit(&unitCfg, &adcUnit[*unit]) != ESP_OK)
        {
            adcUnit[*unit] = NULL;
            return false;
        }
    }

    if (!adcChannelReady[*unit][*channel])
    {
        adc_oneshot_chan_cfg_t chanCfg;
        memset(&chanCfg, 0, sizeof(chanCfg));
        // The widest attenuation keeps the whole 0-3.3 V sense range in
        // scale, which is what the 12-bit maths in Channels and Occupancy
        // assumes. ESP-IDF renamed this from _DB_11 to _DB_12 in 5.3; the
        // setting is the same one.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
        chanCfg.atten = ADC_ATTEN_DB_12;
#else
        chanCfg.atten = ADC_ATTEN_DB_11;
#endif
        chanCfg.bitwidth = ADC_BITWIDTH_12;
        if (adc_oneshot_config_channel(adcUnit[*unit], *channel, &chanCfg)
            != ESP_OK)
        {
            return false;
        }
        adcChannelReady[*unit][*channel] = true;
    }
    return true;
}

int analogRead(int pin)
{
    adc_unit_t unit;
    adc_channel_t channel;
    if (!adc_prepare(pin, &unit, &channel))
    {
        return 0;
    }

    int raw = 0;
    if (adc_oneshot_read(adcUnit[unit], channel, &raw) != ESP_OK)
    {
        return 0;
    }
    return raw;
}

// ---------------------------------------------------------------------------
// LEDC PWM
//
// Channels::begin attaches all four block drivers at 16 kHz with 8 bits of
// duty. Channels are handed out in the order attached, all off one timer, which
// is what the Arduino core does when the frequency and resolution match.
// ---------------------------------------------------------------------------

static const ledc_mode_t LEDC_MODE = LEDC_LOW_SPEED_MODE;
static const ledc_timer_t LEDC_TIMER = LEDC_TIMER_0;

struct LedcBinding
{
    int pin;
    ledc_channel_t channel;
};

static LedcBinding ledcBinding[SOC_LEDC_CHANNEL_NUM];
static int ledcCount = 0;
static bool ledcTimerReady = false;

/// @return the LEDC channel bound to `pin`, or -1.
static int ledc_channel_for(int pin)
{
    for (int i = 0; i < ledcCount; ++i)
    {
        if (ledcBinding[i].pin == pin)
        {
            return (int)ledcBinding[i].channel;
        }
    }
    return -1;
}

bool ledcAttach(int pin, uint32_t freq, uint8_t resolution_bits)
{
    if (ledc_channel_for(pin) >= 0)
    {
        return true;
    }
    if (ledcCount >= SOC_LEDC_CHANNEL_NUM)
    {
        ESP_LOGE(TAG, "out of LEDC channels attaching pin %d", pin);
        return false;
    }

    if (!ledcTimerReady)
    {
        ledc_timer_config_t timerCfg;
        memset(&timerCfg, 0, sizeof(timerCfg));
        timerCfg.speed_mode = LEDC_MODE;
        timerCfg.timer_num = LEDC_TIMER;
        timerCfg.duty_resolution = (ledc_timer_bit_t)resolution_bits;
        timerCfg.freq_hz = freq;
        timerCfg.clk_cfg = LEDC_AUTO_CLK;
        esp_err_t err = ledc_timer_config(&timerCfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "LEDC timer %" PRIu32 " Hz / %u bits failed: %s",
                freq, resolution_bits, esp_err_to_name(err));
            return false;
        }
        ledcTimerReady = true;
    }

    ledc_channel_config_t chanCfg;
    memset(&chanCfg, 0, sizeof(chanCfg));
    chanCfg.gpio_num = pin;
    chanCfg.speed_mode = LEDC_MODE;
    chanCfg.channel = (ledc_channel_t)ledcCount;
    chanCfg.timer_sel = LEDC_TIMER;
    chanCfg.intr_type = LEDC_INTR_DISABLE;
    chanCfg.duty = 0;
    chanCfg.hpoint = 0;
    esp_err_t err = ledc_channel_config(&chanCfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "LEDC channel for pin %d failed: %s", pin,
            esp_err_to_name(err));
        return false;
    }

    ledcBinding[ledcCount].pin = pin;
    ledcBinding[ledcCount].channel = (ledc_channel_t)ledcCount;
    ledcCount++;
    return true;
}

bool ledcWrite(int pin, uint32_t duty)
{
    const int channel = ledc_channel_for(pin);
    if (channel < 0)
    {
        return false;
    }
    if (ledc_set_duty(LEDC_MODE, (ledc_channel_t)channel, duty) != ESP_OK)
    {
        return false;
    }
    return ledc_update_duty(LEDC_MODE, (ledc_channel_t)channel) == ESP_OK;
}

// ---------------------------------------------------------------------------
// Print
// ---------------------------------------------------------------------------

Print Console;

int Print::printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = vprintf(format, args);
    va_end(args);
    return n;
}

size_t Print::print(const char *s)
{
    return (size_t)fputs(s, stdout);
}

size_t Print::println(const char *s)
{
    const int n = printf("%s\n", s);
    return n < 0 ? 0 : (size_t)n;
}

// ---------------------------------------------------------------------------
// I2C
// ---------------------------------------------------------------------------

static const i2c_port_t I2C_PORT = I2C_NUM_0;

TwoWire Wire;

TwoWire::TwoWire()
    : txAddress_(0)
    , txLength_(0)
    , txPending_(false)
    , rxLength_(0)
    , rxIndex_(0)
    , timeoutMs_(50)
    , started_(false)
{
    memset(txBuffer_, 0, sizeof(txBuffer_));
    memset(rxBuffer_, 0, sizeof(rxBuffer_));
}

bool TwoWire::begin(int sda, int scl, uint32_t frequency)
{
    i2c_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = I2C_MODE_MASTER;
    cfg.sda_io_num = sda;
    cfg.scl_io_num = scl;
    // The board has its own pull-ups at R2/R3; the internal ones are far too
    // weak for the bus and would only slow the edges.
    cfg.sda_pullup_en = GPIO_PULLUP_DISABLE;
    cfg.scl_pullup_en = GPIO_PULLUP_DISABLE;
    cfg.master.clk_speed = frequency;

    esp_err_t err = i2c_param_config(I2C_PORT, &cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    started_ = true;
    return true;
}

void TwoWire::setTimeOut(uint16_t ms)
{
    timeoutMs_ = ms;
}

void TwoWire::setClock(uint32_t frequency)
{
    // The legacy driver has no clock setter, so the bus is re-configured. The
    // driver stays installed.
    i2c_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = I2C_MODE_MASTER;
    cfg.sda_io_num = PIN_I2C_SDA;
    cfg.scl_io_num = PIN_I2C_SCL;
    cfg.sda_pullup_en = GPIO_PULLUP_DISABLE;
    cfg.scl_pullup_en = GPIO_PULLUP_DISABLE;
    cfg.master.clk_speed = frequency;
    i2c_param_config(I2C_PORT, &cfg);
}

void TwoWire::beginTransmission(uint8_t address)
{
    txAddress_ = address;
    txLength_ = 0;
    txPending_ = false;
}

size_t TwoWire::write(uint8_t value)
{
    if (txLength_ >= BUFFER_SIZE)
    {
        return 0;
    }
    txBuffer_[txLength_++] = value;
    return 1;
}

size_t TwoWire::write(const uint8_t *data, size_t len)
{
    size_t written = 0;
    for (size_t i = 0; i < len; ++i)
    {
        written += write(data[i]);
    }
    return written;
}

uint8_t TwoWire::endTransmission(bool stop)
{
    if (!started_)
    {
        return 4;
    }

    if (!stop)
    {
        // Hold the bytes back: the requestFrom() that follows turns them into
        // the write half of a write-then-read with a repeated start, which is
        // what the MCP23018 expects for a register read.
        txPending_ = true;
        return 0;
    }

    esp_err_t err = i2c_master_write_to_device(I2C_PORT, txAddress_, txBuffer_,
        txLength_, pdMS_TO_TICKS(timeoutMs_));
    txLength_ = 0;
    txPending_ = false;
    // Wire reports 0 for success and 2 for an address NACK; anything non-zero
    // is treated as a failure by the callers, so the distinction does not
    // matter beyond that.
    return (err == ESP_OK) ? 0 : 2;
}

size_t TwoWire::requestFrom(int address, int len)
{
    rxLength_ = 0;
    rxIndex_ = 0;
    if (!started_ || len <= 0 || (size_t)len > BUFFER_SIZE)
    {
        txPending_ = false;
        return 0;
    }

    esp_err_t err;
    if (txPending_)
    {
        err = i2c_master_write_read_device(I2C_PORT, (uint8_t)address,
            txBuffer_, txLength_, rxBuffer_, (size_t)len,
            pdMS_TO_TICKS(timeoutMs_));
        txPending_ = false;
        txLength_ = 0;
    }
    else
    {
        err = i2c_master_read_from_device(I2C_PORT, (uint8_t)address,
            rxBuffer_, (size_t)len, pdMS_TO_TICKS(timeoutMs_));
    }

    if (err != ESP_OK)
    {
        return 0;
    }
    rxLength_ = (size_t)len;
    return rxLength_;
}

int TwoWire::available()
{
    return (int)(rxLength_ - rxIndex_);
}

int TwoWire::read()
{
    if (rxIndex_ >= rxLength_)
    {
        return -1;
    }
    return rxBuffer_[rxIndex_++];
}

// ---------------------------------------------------------------------------
// The hardware timer, over gptimer
//
// See Arduino.h. Only what DCCSource uses is implemented, and only in the way
// it uses it: one timer, counting microseconds, with a one-shot alarm re-armed
// from inside the interrupt handler.
//
// The alarm is re-armed by the handler itself, which gptimer explicitly allows:
// gptimer_set_alarm_action, gptimer_start, gptimer_stop and
// gptimer_set_raw_count are all documented as callable from interrupt context.
//
// NOT interrupt-safe against a disabled flash cache. CONFIG_GPTIMER_ISR_IRAM_SAFE
// would require the callback and everything it reaches to be resident in RAM,
// and DCCSource's handler reaches static helpers and memcpy in DCCSource.cpp,
// which is not annotated for IRAM. The consequence is that nothing may write
// flash while the DCC source is running; see the interlock in FemtoController
// and the README. Annotating DCCSource.cpp would lift that restriction: it is
// open work, not a constraint.
// ---------------------------------------------------------------------------

/// The one timer. Arduino hands back an opaque pointer, so this stands in for
/// it; DCCSource only ever passes it straight back.
struct hw_timer_t
{
    gptimer_handle_t handle;
    void (*callback)(void);
    bool running;
};

static hw_timer_t s_timer = { nullptr, nullptr, false };

static bool IRAM_ATTR timer_on_alarm(gptimer_handle_t,
    const gptimer_alarm_event_data_t *, void *)
{
    if (s_timer.callback != nullptr)
    {
        s_timer.callback();
    }
    // No task was woken: the handler does its work inline, as Arduino's does.
    return false;
}

hw_timer_t *timerBegin(uint32_t frequency)
{
    if (s_timer.handle != nullptr)
    {
        return &s_timer;        // only one is ever asked for
    }

    gptimer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    cfg.direction = GPTIMER_COUNT_UP;
    cfg.resolution_hz = frequency;

    if (gptimer_new_timer(&cfg, &s_timer.handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "no free hardware timer for the DCC waveform");
        s_timer.handle = nullptr;
        return nullptr;
    }
    s_timer.callback = nullptr;
    s_timer.running = false;
    return &s_timer;
}

void timerAttachInterrupt(hw_timer_t *timer, void (*fn)(void))
{
    if (timer == nullptr || timer->handle == nullptr)
    {
        return;
    }
    timer->callback = fn;

    gptimer_event_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_alarm = timer_on_alarm;
    ESP_ERROR_CHECK(
        gptimer_register_event_callbacks(timer->handle, &cbs, nullptr));
    ESP_ERROR_CHECK(gptimer_enable(timer->handle));
    ESP_ERROR_CHECK(gptimer_start(timer->handle));
    timer->running = true;
}

void timerDetachInterrupt(hw_timer_t *timer)
{
    if (timer == nullptr || timer->handle == nullptr)
    {
        return;
    }
    if (timer->running)
    {
        gptimer_stop(timer->handle);
        timer->running = false;
    }
    gptimer_disable(timer->handle);
    timer->callback = nullptr;
}

void timerAlarm(hw_timer_t *timer, uint64_t ticks, bool autoreload,
    uint64_t reload_count)
{
    if (timer == nullptr || timer->handle == nullptr)
    {
        return;
    }
    gptimer_alarm_config_t alarm;
    memset(&alarm, 0, sizeof(alarm));
    alarm.alarm_count = ticks;
    alarm.reload_count = reload_count;
    alarm.flags.auto_reload_on_alarm = autoreload ? 1 : 0;
    gptimer_set_alarm_action(timer->handle, &alarm);
}

void timerWrite(hw_timer_t *timer, uint64_t value)
{
    if (timer == nullptr || timer->handle == nullptr)
    {
        return;
    }
    gptimer_set_raw_count(timer->handle, value);
}

void timerEnd(hw_timer_t *timer)
{
    if (timer == nullptr || timer->handle == nullptr)
    {
        return;
    }
    if (timer->running)
    {
        gptimer_stop(timer->handle);
        timer->running = false;
    }
    gptimer_disable(timer->handle);
    gptimer_del_timer(timer->handle);
    timer->handle = nullptr;
    timer->callback = nullptr;
}

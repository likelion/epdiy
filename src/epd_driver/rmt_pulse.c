#include "rmt_pulse.h"
#include "esp_system.h"
#include "soc/rmt_struct.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "esp_private/periph_ctrl.h"
#else
#include "driver/periph_ctrl.h"
#endif

#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "driver/gpio.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "esp32/rom/gpio.h"
#include "soc/gpio_periph.h"
#else
#include "rom/gpio.h"
#endif
#include "esp_intr_alloc.h"

static intr_handle_t gRMT_intr_handle = NULL;

/* Channel 1 is used (matching original code) */
#define RMT_CHANNEL 1

// keep track of whether the current pulse is ongoing
volatile bool rmt_tx_done = true;

/**
 * Remote peripheral interrupt. Used to signal when transmission is done.
 */
static void IRAM_ATTR rmt_interrupt_handler(void *arg) {
  rmt_tx_done = true;
  RMT.int_clr.val = RMT.int_st.val;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
/* IDF v5 removed rmt_item32_t, rmt_mem_t, and RMTMEM from soc/rmt_struct.h.
 * Define them here — the hardware layout hasn't changed. */
typedef struct {
    union {
        struct {
            uint32_t duration0 :15;
            uint32_t level0 :1;
            uint32_t duration1 :15;
            uint32_t level1 :1;
        };
        uint32_t val;
    };
} rmt_item32_t;

typedef volatile struct {
    struct {
        rmt_item32_t data32[64];
    } chan[8];
} rmt_block_mem_t;

/* RMT channel memory is at a fixed address on ESP32 */
#define RMTMEM (*(rmt_block_mem_t *)0x3FF56800)
#endif

void rmt_pulse_init(gpio_num_t pin) {

  /* Enable RMT peripheral clock */
  periph_module_enable(PERIPH_RMT_MODULE);

  /* Use RMTMEM instead of FIFO */
  RMT.apb_conf.fifo_mask = 1;

  /* Configure channel via registers directly — replaces rmt_config() */
  RMT.conf_ch[RMT_CHANNEL].conf0.div_cnt = 8;       /* 80MHz / 8 = 10MHz → 0.1us resolution */
  RMT.conf_ch[RMT_CHANNEL].conf0.mem_size = 2;       /* 2 memory blocks */
  RMT.conf_ch[RMT_CHANNEL].conf0.carrier_en = 0;
  RMT.conf_ch[RMT_CHANNEL].conf0.carrier_out_lv = 0;
  RMT.conf_ch[RMT_CHANNEL].conf0.mem_pd = 0;
  RMT.conf_ch[RMT_CHANNEL].conf0.clk_en = 1;

  RMT.conf_ch[RMT_CHANNEL].conf1.tx_start = 0;
  RMT.conf_ch[RMT_CHANNEL].conf1.rx_en = 0;
  RMT.conf_ch[RMT_CHANNEL].conf1.mem_owner = 0;      /* TX owns memory */
  RMT.conf_ch[RMT_CHANNEL].conf1.tx_conti_mode = 0;
  RMT.conf_ch[RMT_CHANNEL].conf1.ref_always_on = 1;  /* Use APB clock */
  RMT.conf_ch[RMT_CHANNEL].conf1.idle_out_lv = 0;    /* Idle low */
  RMT.conf_ch[RMT_CHANNEL].conf1.idle_out_en = 1;    /* Enable idle output */

  /* Route GPIO to RMT channel 1 output signal */
  PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);
  gpio_set_direction(pin, GPIO_MODE_OUTPUT);
  gpio_matrix_out(pin, RMT_SIG_OUT0_IDX + RMT_CHANNEL, false, false);

  /* Register interrupt — replaces rmt_set_tx_intr_en() */
  esp_intr_alloc(ETS_RMT_INTR_SOURCE, ESP_INTR_FLAG_LEVEL3,
                 rmt_interrupt_handler, 0, &gRMT_intr_handle);

  /* Enable TX end interrupt for channel 1 (bit 3 = ch1_tx_end) */
  RMT.int_ena.val |= (1 << (RMT_CHANNEL * 3));
}

void IRAM_ATTR pulse_ckv_ticks(uint16_t high_time_ticks,
                               uint16_t low_time_ticks, bool wait) {
  while (!rmt_tx_done) {
  };
  volatile rmt_item32_t *rmt_mem_ptr =
      &(RMTMEM.chan[RMT_CHANNEL].data32[0]);
  if (high_time_ticks > 0) {
    rmt_mem_ptr->level0 = 1;
    rmt_mem_ptr->duration0 = high_time_ticks;
    rmt_mem_ptr->level1 = 0;
    rmt_mem_ptr->duration1 = low_time_ticks;
  } else {
    rmt_mem_ptr->level0 = 1;
    rmt_mem_ptr->duration0 = low_time_ticks;
    rmt_mem_ptr->level1 = 0;
    rmt_mem_ptr->duration1 = 0;
  }
  RMTMEM.chan[RMT_CHANNEL].data32[1].val = 0;
  rmt_tx_done = false;
  RMT.conf_ch[RMT_CHANNEL].conf1.mem_rd_rst = 1;
  RMT.conf_ch[RMT_CHANNEL].conf1.mem_owner = 0;  /* TX owns memory */
  RMT.conf_ch[RMT_CHANNEL].conf1.tx_start = 1;
  while (wait && !rmt_tx_done) {
  };
}

void IRAM_ATTR pulse_ckv_us(uint16_t high_time_us, uint16_t low_time_us,
                            bool wait) {
  pulse_ckv_ticks(10 * high_time_us, 10 * low_time_us, wait);
}

bool IRAM_ATTR rmt_busy() { return !rmt_tx_done; }

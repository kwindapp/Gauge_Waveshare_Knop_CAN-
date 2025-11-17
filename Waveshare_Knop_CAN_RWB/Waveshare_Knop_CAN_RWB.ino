#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "lcd_config.h"
#include "ui.h"

#include <esp_now.h>
#include <esp_wifi.h>   // needed for wifi_tx_info_t on new ESP32 core
#include <WiFi.h>

// --- ESP-NOW PACKET FROM DASH (MUST MATCH SENDER!) ---
typedef struct {
  uint16_t rpm;
  float batt;
  float motor;
  float dk;
  float gp;
  uint8_t funk;
} DashPacket;

DashPacket lastPacket;

static const char *TAG = "encoder";

static lv_obj_t * meter;
static lv_meter_indicator_t * needle;

// values:
// 0 = rpm (0–8000)
// we keep array form but only use value[0]
uint16_t value[4] = {1000, 20, 120, 0};

SemaphoreHandle_t mutex;

/* ===================== METER DRAW EVENT (LABELS 0–8) ===================== */
static void meter_draw_event_cb(lv_event_t * e)
{
    lv_obj_draw_part_dsc_t * dsc = lv_event_get_draw_part_dsc(e);

    /* Only care about meter tick labels */
    if(dsc->class_p != &lv_meter_class) return;
    if(dsc->type != LV_METER_DRAW_PART_TICK) return;

    /* dsc->value is the real value (0..8000), we want text 0..8 */
    static char buf[8];
    int label = (int)(dsc->value / 1000);   // 0..8
    if(label < 0)   label = 0;
    if(label > 8)   label = 8;

    lv_snprintf(buf, sizeof(buf), "%d", label);
    dsc->text = buf;  // override label text
}

// ===================== ESP-NOW RECV CALLBACK =====================
// new ESP-NOW API on ESP32 core 3.x: use esp_now_recv_info_t
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
  if (len < (int)sizeof(DashPacket)) return;  // sanity check

  DashPacket pkt;
  memcpy(&pkt, incomingData, sizeof(DashPacket));

  if (mutex && xSemaphoreTake(mutex, 0) == pdTRUE) {

    // clamp RPM 0–8000
    uint16_t rpm = pkt.rpm;
    if (rpm > 8000) rpm = 8000;
    value[0] = rpm;

    lastPacket = pkt;  // keep full packet if later you want more

    xSemaphoreGive(mutex);
  }
}

// ===================== RPM METER ONLY =====================
void lv_example_meter_1(void)
{
    extern lv_obj_t *ui_Screen1;

    /* Create the meter (use global 'meter') */
    meter = lv_meter_create(ui_Screen1);
    lv_obj_center(meter);
    lv_obj_set_size(meter, 200, 200);
    lv_obj_set_pos(meter, -71, 0);
    lv_obj_set_style_bg_opa(meter, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(meter, 0, LV_PART_MAIN);

/* Change center pivot circle color */
lv_obj_set_style_bg_color(meter, lv_color_hex(0xD8D5D5), LV_PART_INDICATOR);
lv_obj_set_style_bg_opa(meter, LV_OPA_COVER, LV_PART_INDICATOR);

/* Optional: outline of the center circle */
lv_obj_set_style_outline_color(meter, lv_color_hex(0x4F4C4A), LV_PART_INDICATOR);
lv_obj_set_style_outline_width(meter, 4, LV_PART_INDICATOR);

/* Optional: change its size (radius) */
lv_obj_set_style_size(meter, 18, LV_PART_INDICATOR);   // diameter of center circle
lv_obj_set_style_radius(meter, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);




    /* Add draw event to override labels (0..8) */
    lv_obj_add_event_cb(meter, meter_draw_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    /* Add scale */
    lv_meter_scale_t * scale = lv_meter_add_scale(meter);

    /* Range: 0–8000 rpm, 270° sweep, starting at 135° */
    lv_meter_set_scale_range(meter, scale, 0, 8000, 270, 135);

    /* Minor ticks: 41 ticks = 0, 200, 400, ... 8000 */
    lv_meter_set_scale_ticks(meter, scale,
                             41,                                // tick count
                             3,                                 // thickness
                             10,                                // length
                             lv_palette_main(LV_PALETTE_GREY)); // color

    /* Major ticks: every 5th minor tick -> 0,1000,...8000 (labels overridden to 0..8) */
    lv_meter_set_scale_major_ticks(meter, scale,
                                   5,                 // nth: every 5th minor is major
                                   5,                 // thickness
                                   15,                // length
                                   lv_color_white(),  // color
                                   10);               // label gap

    lv_meter_indicator_t * indic;

    /* Blue at low RPM 0–2000 */
    indic = lv_meter_add_arc(meter, scale, 3, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
    lv_meter_set_indicator_start_value(meter, indic, 0);
    lv_meter_set_indicator_end_value(meter, indic, 2000);

    /* Red at high RPM 6000–8000 */
    indic = lv_meter_add_arc(meter, scale, 3, lv_palette_main(LV_PALETTE_RED), 0);
    lv_meter_set_indicator_start_value(meter, indic, 6000);
    lv_meter_set_indicator_end_value(meter, indic, 8000);

    /* Needle (use global 'needle') */
    needle = lv_meter_add_needle_line(meter, scale,
                                      8,                     // width
                                      lv_color_hex(0xF56F27),
                                      -15);                  // r_mod

    /* Start at 0 RPM */
    lv_meter_set_indicator_value(meter, needle, 0);
}

// ===================== LVGL UPDATE TASK (RPM ONLY) =====================
static void example_lvgl_port_task(void *arg)
{
  for(;;)
  {
    lv_timer_handler();

    if (xSemaphoreTake(mutex, portMAX_DELAY)) {

        // RPM label
        lv_label_set_text(ui_power, (String(value[0]) + " rpm").c_str());
        lv_meter_set_indicator_value(meter, needle, value[0]);
        lv_arc_set_value(ui_Arc1, value[0]);   // RPM arc (0–8000)

        xSemaphoreGive(mutex);
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void setup()
{
  mutex = xSemaphoreCreateMutex();
  Serial.begin(115200);
  Touch_Init();
  lcd_lvgl_Init();          // LVGL+UI init

  // ====== ONLY CREATE RPM METER ======
  lv_example_meter_1();

  // ====== HIDE EVERYTHING ELSE FROM THE UI ======
  // (in case your SquareLine/GUI has other labels/arcs)
  lv_obj_add_flag(ui_power1, LV_OBJ_FLAG_HIDDEN);  // temp label
  lv_obj_add_flag(ui_power2, LV_OBJ_FLAG_HIDDEN);  // blue label
  lv_obj_add_flag(ui_power3, LV_OBJ_FLAG_HIDDEN);  // batt label

  lv_obj_add_flag(ui_Arc2,   LV_OBJ_FLAG_HIDDEN);  // temp arc
  lv_obj_add_flag(ui_Arc3,   LV_OBJ_FLAG_HIDDEN);  // batt arc
  lv_obj_add_flag(ui_Arc4,   LV_OBJ_FLAG_HIDDEN);  // blue arc

  lv_obj_add_flag(ui_colorPNL, LV_OBJ_FLAG_HIDDEN); // color preview panel if not wanted

  // Only RPM arc needs correct range
  lv_arc_set_range(ui_Arc1, 0, 8000);   // RPM

  lcd_bl_pwm_bsp_init(40); // brightness up to 255

  // ========== ESP-NOW RX INIT ==========
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // only receive – no send, so no peer needed
  esp_now_register_recv_cb(OnDataRecv);

  // ========== LVGL TASK ==========
  xTaskCreate(example_lvgl_port_task, "LVGL",
              EXAMPLE_LVGL_TASK_STACK_SIZE,
              NULL,
              EXAMPLE_LVGL_TASK_PRIORITY,
              NULL);
}

void loop()
{
  // nothing here; everything runs in tasks
}

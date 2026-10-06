#include <algorithm>
#include <vector>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include "custom_lcd_display.h"
#include "lcd_display.h"
#include "esp_lvgl_port.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "config.h"
#include "board.h"
#include "lvgl_theme.h"
#include <material_symbols.h>

namespace {
constexpr int kFaceWidth = 160;
constexpr int kFaceHeight = 120;
constexpr uint16_t kInk = 0x0000;
constexpr uint16_t kPaper = 0xffff;

void Plot(uint16_t* pixels, int x, int y, int radius = 0) {
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy <= radius * radius) {
                const int px = x + dx;
                const int py = y + dy;
                if (px >= 0 && px < kFaceWidth && py >= 0 && py < kFaceHeight) {
                    pixels[py * kFaceWidth + px] = kInk;
                }
            }
        }
    }
}

bool IsFaceEmotion(const char* emotion) {
    return emotion && (strcmp(emotion, "neutral") == 0 ||
        strcmp(emotion, "robot_2") == 0 || strcmp(emotion, "happy") == 0 ||
        strcmp(emotion, "sad") == 0 || strcmp(emotion, "surprised") == 0 ||
        strcmp(emotion, "sleepy") == 0);
}
}

void CustomLcdDisplay::Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    assert(disp != NULL);
    CustomLcdDisplay *Disp = (CustomLcdDisplay *)lv_display_get_user_data(disp);
    uint16_t *buffer = (uint16_t *)color_p;
  	for(int y = area->y1; y <= area->y2; y++)
  	{
  	 	for(int x = area->x1; x <= area->x2; x++) 
  	 	{
  	 	   	uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
  	 	   	Disp->RLCD_SetPixel(x,y,color);
  	 	   	buffer++;
  	 	}
  	}
  	Disp->RLCD_Display();
	lv_disp_flush_ready(disp);
}

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
esp_lcd_panel_handle_t panel,
int width, 
int height, 
int offset_x, 
int offset_y,
bool mirror_x, 
bool mirror_y, 
bool swap_xy,
spi_display_config_t spiconfig,
spi_host_device_t spi_host) : LcdDisplay(panel_io, panel, width, height),
mosi_(spiconfig.mosi),
scl_(spiconfig.scl), 
dc_(spiconfig.dc), 
cs_(spiconfig.cs), 
rst_(spiconfig.rst), 
width_(width), 
height_(height)
{
	ESP_LOGI(TAG, "Initialize SPI");
	esp_err_t        ret;
    spi_bus_config_t buscfg   = {};
    int              transfer = width_ * height_;
    buscfg.miso_io_num                   = -1;
    buscfg.mosi_io_num                   = mosi_;
    buscfg.sclk_io_num                   = scl_;
    buscfg.quadwp_io_num                 = -1;
    buscfg.quadhd_io_num                 = -1;
    buscfg.max_transfer_sz               = transfer;
    ret                                  = spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = static_cast<gpio_num_t>(dc_);
    io_config.cs_gpio_num = static_cast<gpio_num_t>(cs_);
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 7;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host, &io_config, &io_handle));
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_conf.mode          = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask  = (0x1ULL << rst_);
    gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    Set_ResetIOLevel(1);

    DisplayLen                = transfer >> 3; //(1byte 8ipex)
    DispBuffer                = (uint8_t *) heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM);
    assert(DispBuffer);
	PixelIndexLUT = (uint16_t (*)[300])heap_caps_malloc(transfer * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
	PixelBitLUT   = (uint8_t (*)[300])heap_caps_malloc(transfer * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    assert(PixelIndexLUT);
    assert(PixelBitLUT);
    if(width_ == 400) {
        InitLandscapeLUT();
    } else {
        InitPortraitLUT();
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    display_ = lv_display_create(width, height); /* 以水平和垂直分辨率（像素）进行基本初始化 */
    lv_display_set_flush_cb(display_, Lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
	size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;
	uint8_t *lvgl_buffer1 = (uint8_t *) heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    assert(lvgl_buffer1);
	lv_display_set_buffers(display_, lvgl_buffer1, NULL, lvgl_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "RLCD init");
    RLCD_Init();

    lvgl_port_unlock();
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

CustomLcdDisplay::~CustomLcdDisplay() {
    DisplayLockGuard lock(this);
    if (lock) {
        ClearTemporaryBitmapLocked();
        if (face_view_) {
            lv_obj_delete(face_view_);
            face_view_ = nullptr;
        }
        if (face_pixels_) {
            heap_caps_free(face_pixels_);
            face_pixels_ = nullptr;
        }
    }
}

void CustomLcdDisplay::SetupUI() {
    if (setup_ui_called_) return;
    DisplayLockGuard lock(this);
    if (!lock || setup_ui_called_) return;
    Display::SetupUI();

    auto* theme = static_cast<LvglTheme*>(current_theme_);
    auto* text_font = theme->text_font()->font();
    auto* icon_font = theme->icon_font()->font();
    auto* screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);

    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, width_, height_);
    lv_obj_set_pos(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_scrollbar_mode(container_, LV_SCROLLBAR_MODE_OFF);

    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, width_, 43);
    lv_obj_set_pos(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_border_width(top_bar_, 2, 0);
    lv_obj_set_style_border_side(top_bar_, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    status_label_ = lv_label_create(top_bar_);
    lv_obj_set_size(status_label_, 276, 28);
    lv_obj_set_pos(status_label_, 14, 9);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(status_label_, "启动中");

    notification_label_ = lv_label_create(top_bar_);
    lv_obj_set_size(notification_label_, 276, 28);
    lv_obj_set_pos(notification_label_, 14, 9);
    lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    network_label_ = lv_label_create(top_bar_);
    lv_obj_set_pos(network_label_, 302, 8);
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_label_set_text(network_label_, "");

    mute_label_ = lv_label_create(top_bar_);
    lv_obj_set_pos(mute_label_, 330, 8);
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_label_set_text(mute_label_, "");

    battery_label_ = lv_label_create(top_bar_);
    lv_obj_set_pos(battery_label_, 361, 8);
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_label_set_text(battery_label_, "");

    agent_stage_ = lv_obj_create(container_);
    lv_obj_set_size(agent_stage_, width_ - 24, 166);
    lv_obj_set_pos(agent_stage_, 12, 50);
    lv_obj_set_style_pad_all(agent_stage_, 0, 0);
    lv_obj_set_style_radius(agent_stage_, 12, 0);
    lv_obj_set_style_border_width(agent_stage_, 2, 0);
    lv_obj_set_scrollbar_mode(agent_stage_, LV_SCROLLBAR_MODE_OFF);

    emoji_box_ = lv_obj_create(agent_stage_);
    lv_obj_set_size(emoji_box_, 180, 126);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(emoji_box_, LV_SCROLLBAR_MODE_OFF);

    face_pixels_ = static_cast<uint16_t*>(heap_caps_malloc(
        kFaceWidth * kFaceHeight * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (face_pixels_) {
        face_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
        face_image_.header.cf = LV_COLOR_FORMAT_RGB565;
        face_image_.header.w = kFaceWidth;
        face_image_.header.h = kFaceHeight;
        face_image_.header.stride = kFaceWidth * sizeof(uint16_t);
        face_image_.data_size = kFaceWidth * kFaceHeight * sizeof(uint16_t);
        face_image_.data = reinterpret_cast<uint8_t*>(face_pixels_);
        face_view_ = lv_image_create(emoji_box_);
        lv_image_set_src(face_view_, &face_image_);
        lv_obj_center(face_view_);
        DrawFaceLocked("neutral");
    }

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, theme->large_icon_font()->font(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_center(emoji_label_);
    if (face_view_) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    emoji_image_ = lv_image_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    preview_image_ = lv_image_create(agent_stage_);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    bottom_bar_ = lv_obj_create(container_);
    lv_obj_set_size(bottom_bar_, width_, 76);
    lv_obj_set_pos(bottom_bar_, 0, height_ - 76);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_border_width(bottom_bar_, 2, 0);
    lv_obj_set_style_border_side(bottom_bar_, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);

    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_obj_set_size(chat_message_label_, width_ - 32, 56);
    lv_obj_set_pos(chat_message_label_, 16, 10);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    UpdateConversationLocked();

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_size(low_battery_popup_, width_ - 36, 48);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, "电量低，请充电");
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    ApplyPaletteLocked();
}

void CustomLcdDisplay::ApplyPaletteLocked() {
    const lv_color_t paper = lv_color_white();
    const lv_color_t ink = lv_color_black();
    lv_obj_set_style_bg_color(lv_screen_active(), paper, 0);
    lv_obj_set_style_text_color(lv_screen_active(), ink, 0);
    for (auto* panel : {container_, top_bar_, agent_stage_, bottom_bar_}) {
        if (!panel) continue;
        lv_obj_set_style_bg_color(panel, paper, 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(panel, ink, 0);
        lv_obj_set_style_text_color(panel, ink, 0);
    }
    for (auto* label : {network_label_, status_label_, notification_label_, mute_label_,
                        battery_label_, chat_message_label_, emoji_label_}) {
        if (label) lv_obj_set_style_text_color(label, ink, 0);
    }
    if (low_battery_popup_) {
        lv_obj_set_style_bg_color(low_battery_popup_, ink, 0);
        lv_obj_set_style_border_width(low_battery_popup_, 0, 0);
        lv_obj_set_style_radius(low_battery_popup_, 8, 0);
        lv_obj_set_style_text_color(low_battery_label_, paper, 0);
    }
}

void CustomLcdDisplay::SetTheme(Theme* theme) {
    LcdDisplay::SetTheme(theme);
    DisplayLockGuard lock(this);
    if (lock && setup_ui_called_) ApplyPaletteLocked();
}

void CustomLcdDisplay::DrawFaceLocked(const char* emotion) {
    if (!face_pixels_ || !face_view_) return;
    std::fill_n(face_pixels_, kFaceWidth * kFaceHeight, kPaper);
    // Thick circular silhouette and separate eyes remain legible on this 1-bit panel.
    for (int y = 10; y <= 110; ++y) {
        for (int x = 30; x <= 130; ++x) {
            const int dx = x - 80;
            const int dy = y - 60;
            const int d2 = dx * dx + dy * dy;
            if (d2 >= 47 * 47 && d2 <= 50 * 50) Plot(face_pixels_, x, y);
        }
    }
    const bool sleepy = strcmp(emotion, "sleepy") == 0;
    for (int x : {60, 100}) {
        if (sleepy) {
            for (int dx = -8; dx <= 8; ++dx) Plot(face_pixels_, x + dx, 47, 2);
        } else {
            Plot(face_pixels_, x, 47, 6);
        }
    }
    if (strcmp(emotion, "surprised") == 0) {
        for (int y = 71; y <= 96; ++y) {
            for (int x = 69; x <= 91; ++x) {
                const int dx = x - 80;
                const int dy = y - 83;
                const int d = dx * dx * 144 + dy * dy * 121;
                if (d >= 12000 && d <= 17500) Plot(face_pixels_, x, y, 1);
            }
        }
    } else if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "sad") == 0) {
        const bool happy = strcmp(emotion, "happy") == 0;
        for (int x = 55; x <= 105; ++x) {
            const int dx = x - 80;
            const int curve = (625 - dx * dx) * 13 / 625;
            Plot(face_pixels_, x, happy ? 77 + curve : 90 - curve, 2);
        }
    } else {
        for (int x = 61; x <= 99; ++x) Plot(face_pixels_, x, 82, 2);
    }
    lv_obj_invalidate(face_view_);
}

void CustomLcdDisplay::UpdateConversationLocked() {
    if (!chat_message_label_) return;
    lv_label_set_text(chat_message_label_, subtitles_hidden_ ? "" : chat_content_.c_str());
}

void CustomLcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!lock) return;
    (void)role;
    chat_content_ = content ? content : "";
    UpdateConversationLocked();
}

void CustomLcdDisplay::ClearChatMessages() {
    SetChatMessage("system", "");
}

void CustomLcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    if (!lock) return;
    subtitles_hidden_ = hide;
    UpdateConversationLocked();
}

void CustomLcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (!lock || !preview_image_) return;
    if (!image) {
        esp_timer_stop(preview_timer_);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(preview_image_, nullptr);
        preview_image_cached_.reset();
        if (!temporary_bitmap_active_.load()) lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        if (gif_controller_) gif_controller_->Start();
        return;
    }
    ClearTemporaryBitmapLocked();
    preview_image_cached_ = std::move(image);
    auto* descriptor = preview_image_cached_->image_dsc();
    if (!descriptor || !descriptor->header.w || !descriptor->header.h) {
        preview_image_cached_.reset();
        return;
    }
    const int max_width = width_ - 68;
    const int max_height = 140;
    const int scale = std::min({256, 256 * max_width / descriptor->header.w,
                               256 * max_height / descriptor->header.h});
    lv_image_set_src(preview_image_, descriptor);
    lv_image_set_scale(preview_image_, scale);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    if (gif_controller_) gif_controller_->Stop();
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void CustomLcdDisplay::ClearTemporaryBitmapLocked() {
    if (temporary_bitmap_timer_) {
        lv_timer_delete(temporary_bitmap_timer_);
        temporary_bitmap_timer_ = nullptr;
    }
    if (temporary_bitmap_) {
        lv_obj_delete(temporary_bitmap_);
        temporary_bitmap_ = nullptr;
    }
    if (temporary_bitmap_image_.data) {
        heap_caps_free(const_cast<uint8_t*>(temporary_bitmap_image_.data));
        temporary_bitmap_image_ = {};
    }
    temporary_bitmap_active_.store(false);
    if (emoji_box_ && preview_image_ &&
        lv_obj_has_flag(preview_image_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CustomLcdDisplay::SetEmotion(const char* emotion) {
    {
        DisplayLockGuard lock(this);
        if (lock && emotion && current_emotion_ != emotion) {
            current_emotion_ = emotion;
            ClearTemporaryBitmapLocked();
        }
        if (lock && face_view_ && IsFaceEmotion(emotion)) {
            DrawFaceLocked(emotion);
            lv_obj_remove_flag(face_view_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        if (lock && face_view_) lv_obj_add_flag(face_view_, LV_OBJ_FLAG_HIDDEN);
    }
    LcdDisplay::SetEmotion(emotion);
}

bool CustomLcdDisplay::ShowTemporaryBitmap(const std::vector<uint8_t>& bits, int width,
                                            int height, uint32_t duration_ms) {
    const size_t stride = (width + 7) / 8;
    if (!agent_stage_ || width < 1 || width > 160 || height < 1 || height > 120 ||
        bits.size() != stride * height || duration_ms < 100 || duration_ms > 60000) {
        return false;
    }
    auto pixels = static_cast<uint16_t*>(heap_caps_malloc(width * height * sizeof(uint16_t),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels) return false;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            pixels[y * width + x] = (bits[y * stride + x / 8] & (0x80 >> (x % 8)))
                                          ? 0xffff : 0x0000;
        }
    }
    DisplayLockGuard lock(this);
    if (!lock) {
        heap_caps_free(pixels);
        return false;
    }
    esp_timer_stop(preview_timer_);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(preview_image_, nullptr);
    preview_image_cached_.reset();
    ClearTemporaryBitmapLocked();
    temporary_bitmap_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
    temporary_bitmap_image_.header.cf = LV_COLOR_FORMAT_RGB565;
    temporary_bitmap_image_.header.w = width;
    temporary_bitmap_image_.header.h = height;
    temporary_bitmap_image_.header.stride = width * sizeof(uint16_t);
    temporary_bitmap_image_.data_size = width * height * sizeof(uint16_t);
    temporary_bitmap_image_.data = reinterpret_cast<uint8_t*>(pixels);
    temporary_bitmap_ = lv_image_create(agent_stage_);
    if (!temporary_bitmap_) {
        ClearTemporaryBitmapLocked();
        return false;
    }
    lv_image_set_src(temporary_bitmap_, &temporary_bitmap_image_);
    lv_obj_align(temporary_bitmap_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(temporary_bitmap_);
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    temporary_bitmap_timer_ = lv_timer_create([](lv_timer_t* timer) {
        auto* display = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        display->ClearTemporaryBitmapLocked();
    }, duration_ms, this);
    if (!temporary_bitmap_timer_) {
        ClearTemporaryBitmapLocked();
        return false;
    }
    temporary_bitmap_active_.store(true);
    return true;
}

void CustomLcdDisplay::InitPortraitLUT() {
    uint16_t W4 = width_ >> 2;
    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t byte_y = y >> 1;
        uint8_t  local_y = y & 1;
        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 2;
            uint8_t  local_x = x & 3;

            uint32_t index = byte_y * W4 + byte_x;
            uint8_t bit = 7 - ((local_x << 1) | local_y);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void CustomLcdDisplay::InitLandscapeLUT() {
    uint16_t H4 = height_ >> 2;
    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t inv_y = height_ - 1 - y;
        uint16_t block_y = inv_y >> 2;
        uint8_t  local_y  = inv_y & 3;
        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 1;
            uint8_t  local_x = x & 1;

            uint32_t index = byte_x * H4 + block_y;
            uint8_t bit = 7 - ((local_y << 1) | local_x);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void CustomLcdDisplay::Set_ResetIOLevel(uint8_t level) {
    gpio_set_level((gpio_num_t) rst_, level ? 1 : 0);
}

void CustomLcdDisplay::RLCD_SendCommand(uint8_t Reg) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, Reg, NULL, 0));
}

void CustomLcdDisplay::RLCD_SendData(uint8_t Data) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, -1, &Data, 1));
}

void CustomLcdDisplay::RLCD_Sendbuffera(uint8_t *Data, int len) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_handle, -1, Data, len));
}

void CustomLcdDisplay::RLCD_Reset(void) {
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    Set_ResetIOLevel(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

void CustomLcdDisplay::RLCD_ColorClear(uint8_t color) {
    memset(DispBuffer, color, DisplayLen);
}

void CustomLcdDisplay::RLCD_Init() {
    RLCD_Reset();

    RLCD_SendCommand(0xD6);  // NVM Load Control
	RLCD_SendData(0x17);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xD1); //Booster Enable
	RLCD_SendData(0x01);

	RLCD_SendCommand(0xC0); //Gate Voltage Control
	RLCD_SendData(0x11);   
	RLCD_SendData(0x04);   

	RLCD_SendCommand(0xC1); //VSHP Setting
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);

	RLCD_SendCommand(0xC2);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xC4);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);

	RLCD_SendCommand(0xC5);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xD8);
	RLCD_SendData(0x80);
	RLCD_SendData(0xE9);

	RLCD_SendCommand(0xB2);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xB3);
	RLCD_SendData(0xE5);
	RLCD_SendData(0xF6);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0xB4);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0x62);
	RLCD_SendData(0x32);
	RLCD_SendData(0x03);
	RLCD_SendData(0x1F);

	RLCD_SendCommand(0xB7);
	RLCD_SendData(0x13);

	RLCD_SendCommand(0xB0);
	RLCD_SendData(0x64);

	RLCD_SendCommand(0x11); 
	vTaskDelay(pdMS_TO_TICKS(200));     
	RLCD_SendCommand(0xC9);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0x36);
	RLCD_SendData(0x48); 

	RLCD_SendCommand(0x3A);
	RLCD_SendData(0x11); 

	RLCD_SendCommand(0xB9);
	RLCD_SendData(0x20);

	RLCD_SendCommand(0xB8);
	RLCD_SendData(0x29);

	RLCD_SendCommand(0x21);

	RLCD_SendCommand(0x2A); 
	RLCD_SendData(0x12);
	RLCD_SendData(0x2A);

	RLCD_SendCommand(0x2B); 
	RLCD_SendData(0x00);
	RLCD_SendData(0xC7);

	RLCD_SendCommand(0x35);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0xD0);
	RLCD_SendData(0xFF);

	RLCD_SendCommand(0x38);
	RLCD_SendCommand(0x29);

    RLCD_ColorClear(ColorWhite);
}

void CustomLcdDisplay::RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color) {
    uint32_t idx = PixelIndexLUT[x][y];
    uint8_t  mask = PixelBitLUT[x][y];

    uint8_t *p = &DispBuffer[idx];

    if (color)
        *p |= mask;
    else
        *p &= ~mask;
}

void CustomLcdDisplay::RLCD_Display() {
    RLCD_SendCommand(0x2A);     // Column Address Set
  	RLCD_SendData(0x12);
  	RLCD_SendData(0x2A);

  	RLCD_SendCommand(0x2B);     // Page Address Set
  	RLCD_SendData(0x00);
  	RLCD_SendData(0xC7);

  	RLCD_SendCommand(0x2c);     // Page Address Set

	RLCD_Sendbuffera(DispBuffer,DisplayLen);
}

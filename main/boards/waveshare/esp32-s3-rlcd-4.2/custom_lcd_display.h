#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <driver/gpio.h>
#include <atomic>
#include <string>
#include <vector>
#include "lcd_display.h"

enum ColorSelection {
    ColorBlack = 0,    
    ColorWhite = 0xff
};

typedef struct {
    uint8_t mosi;
    uint8_t scl;
    uint8_t dc;
    uint8_t cs;
    uint8_t rst;
} spi_display_config_t;

class CustomLcdDisplay : public LcdDisplay {
private:
    lv_obj_t* agent_stage_ = nullptr;
    lv_obj_t* face_view_ = nullptr;
    lv_obj_t* role_label_ = nullptr;
    uint16_t* face_pixels_ = nullptr;
    lv_img_dsc_t face_image_{};
    std::string chat_role_ = "system";
    std::string chat_content_;
    bool subtitles_hidden_ = false;
    lv_obj_t* temporary_bitmap_ = nullptr;
    lv_timer_t* temporary_bitmap_timer_ = nullptr;
    lv_img_dsc_t temporary_bitmap_image_{};
    std::atomic<bool> temporary_bitmap_active_{false};
    std::string current_emotion_;
    void ClearTemporaryBitmapLocked();
    void DrawFaceLocked(const char* emotion);
    void UpdateConversationLocked();
    void ApplyPaletteLocked();
    esp_lcd_panel_io_handle_t io_handle = NULL;
    uint32_t            i2c_data_pdMS_TICKS = 0;
    uint32_t            i2c_done_pdMS_TICKS = 0;
    const char         *TAG                 = "CustomDisplay";
    int                 mosi_;
    int                 scl_;
    int                 dc_;
    int                 cs_;
    int                 rst_;
    int                 width_;
    int                 height_;
    uint8_t            *DispBuffer = NULL;
    int                 DisplayLen;
	uint16_t (*PixelIndexLUT)[300];
	uint8_t  (*PixelBitLUT  )[300];
	void InitPortraitLUT();
	void InitLandscapeLUT();
    void Set_ResetIOLevel(uint8_t level);
    void RLCD_SendCommand(uint8_t Reg);
    void RLCD_SendData(uint8_t Data);
    void RLCD_Sendbuffera(uint8_t *Data, int len);
    void RLCD_Reset(void);
    static void Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);

public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,spi_display_config_t spiconfig,spi_host_device_t spi_host = SPI3_HOST);
    ~CustomLcdDisplay();
    void SetupUI() override;
    void SetTheme(Theme* theme) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetHideSubtitle(bool hide) override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    bool IsMonochrome() const override { return true; }
    bool ShowTemporaryBitmap(const std::vector<uint8_t>& bits, int width, int height,
                             uint32_t duration_ms);
    bool TemporaryBitmapActive() const { return temporary_bitmap_active_.load(); }
    void RLCD_Init();
    void RLCD_ColorClear(uint8_t color);
    void RLCD_Display();
	void RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color);
};

#endif

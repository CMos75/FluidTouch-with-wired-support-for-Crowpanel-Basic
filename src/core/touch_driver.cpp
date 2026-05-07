#include "core/touch_driver.h"
#include "core/display_driver.h"
#include "core/power_manager.h"

// Static touch point data
static struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
    bool was_pressed;  // Track previous state for edge detection
} touchPoint = {0, 0, false, false};

// Static LCD instance pointer (set during init)
static LGFX *lcd_instance = nullptr;

// Debug counter for periodic logging
static uint32_t debug_counter = 0;

// Constructor
TouchDriver::TouchDriver() : indev(nullptr) {
}

// Initialize touch controller
bool TouchDriver::init(LGFX *lcd) {
    Serial.println("Initializing touch controller...");
    
    // Store LCD instance for touch reading
    lcd_instance = lcd;
    
    Serial.printf("Touch I2C pins: SDA=%d, SCL=%d (managed by LovyanGFX)\n", TOUCH_SDA, TOUCH_SCL);
    
    // Verify GT911 is present on I2C bus
    Serial.println("TouchDriver: Scanning I2C bus for GT911...");
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(100000);
    
    bool found = false;
    for (int addr = 0x14; addr <= 0x5D; addr += (addr == 0x14 ? 0x49 : 0xFF)) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  GT911 responding at 0x%02X\n", addr);
            found = true;
        }
    }
    
    if (!found) {
        Serial.println("WARNING: GT911 NOT found on I2C bus!");
    }
    
    Serial.println("Touch Controller: GT911 initialized by LovyanGFX");
    
    // Register touch controller with LVGL
    // The my_touchpad_read callback will call LovyanGFX's getTouch() method
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);
    
    Serial.println("Touch driver registered with LVGL");
    return true;
}

// LVGL touchpad read callback
void TouchDriver::my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
    if (!lcd_instance) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    
    uint16_t x, y;
    
    // Use LovyanGFX's getTouch() method
    if (lcd_instance->getTouch(&x, &y)) {
        touchPoint.x = x;
        touchPoint.y = y;
        touchPoint.pressed = true;
        Serial.printf("TouchDriver: Touch detected at (%d, %d)\n", x, y);
    } else {
        touchPoint.pressed = false;
    }
    
    // Periodic debug logging (every ~1000 calls - about once per 5 seconds)
    debug_counter++;
    if (debug_counter % 1000 == 0) {
        Serial.printf("TouchDriver: Callback called %lu times, getTouch returned: %s\n", 
                     (unsigned long)debug_counter, touchPoint.pressed ? "PRESSED" : "RELEASED");
    }
    
    // Detect touch press edge (transition from not pressed to pressed)
    if (touchPoint.pressed && !touchPoint.was_pressed) {
        Serial.println("TouchDriver: Touch press EDGE detected - calling PowerManager::onUserActivity()");
        // Notify power manager of user activity
        PowerManager::onUserActivity();
    }
    touchPoint.was_pressed = touchPoint.pressed;
    
    if (touchPoint.pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = touchPoint.x;
        data->point.y = touchPoint.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/*
WS2812FX.cpp - Library for WS2812 LED effects.

Harm Aldick - 2016
www.aldick.org

Ported to esp-open-rtos by PCSaito - 2018
www.github.com/pcsaito

FEATURES
* A lot of blinken modes and counting

LICENSE

The MIT License (MIT)

Copyright (c) 2016  Harm Aldick

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.


CHANGELOG

2016-05-28   Initial beta release
2016-06-03   Code cleanup, minor improvements, new modes
2016-06-04   2 new fx, fixed setColor (now also resets _mode_color)
2017-02-02   removed "blackout" on mode, speed or color-change
2018-04-24   ported to esp-open-rtos to use in esp-homekit-demo
*/

#include "WS2812FX.h"
#include <math.h>

#include <freertos/FreeRTOS.h>
#include "freertos/task.h"

#include <esp_log.h>
#include "esp_event.h"	//	for usleep
#include <string.h>

#include "driver/rmt.h"

#define CALL_MODE(n) _mode[n]();

#define RMT_TX_CHANNEL 						RMT_CHANNEL_0
#define WS2812_GPIO 						22
#define WS2812_LED_NUMBER					32
#define WS2812_TIMEOUT						100

static const char *TAG = "ws2812_FX";

typedef enum {
  PIXEL_RGB = 12,
  PIXEL_RGBW = 16
} pixeltype_t;

uint8_t _mode_index = DEFAULT_MODE;
uint8_t _speed = DEFAULT_SPEED;
uint8_t _brightness = 0;
uint8_t _target_brightness = 0;
bool _running = false;
bool _inverted = false;
bool _slow_start = false;

uint16_t _led_count = 0;

uint32_t _color = DEFAULT_COLOR;
uint32_t _mode_color = DEFAULT_COLOR;

uint32_t _mode_delay = 100;
uint32_t _counter_mode_call = 0;
uint32_t _counter_mode_step = 0;
uint32_t _mode_last_call_time = 0;
	  
uint8_t get_random_wheel_index(uint8_t);

mode _mode[MODE_COUNT];

// ws2812_pixel_t *pixels;

led_strip_t *strip;

// pixel_settings_t px;

//Helpers
uint32_t color32(uint8_t r, uint8_t g, uint8_t b) {
	return ((uint32_t)r << 16) | ((uint32_t)g <<  8) | b;
}

uint32_t constrain(uint32_t amt, uint32_t low, uint32_t high) {
	return (amt < low) ? low : ((amt > high) ? high : amt);
}

float fconstrain(float amt, float low, float high) {
	return (amt < low) ? low : ((amt > high) ? high : amt);
}

uint32_t randomInRange(uint32_t min, uint32_t max) {
	if (min < max) {
		uint32_t randomValue = rand() % (max - min);
		return randomValue + min;
	} else if (min == max) {
		return min;
	}
	return 0;
}

long map(long x, long in_min, long in_max, long out_min, long out_max) {
	return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

static uint16_t min(uint16_t a, uint16_t b) {
    return (a > b) ? b : a;
}

static uint32_t max(uint32_t a, uint32_t b) {
    return (a > b) ? a : b;
}

//LED Adapter
void WS2812_show(void) {
	ESP_ERROR_CHECK(strip->refresh(strip, WS2812_TIMEOUT));
}

void WS2812_setPixelColor(uint16_t n, uint8_t r, uint8_t g, uint8_t b) {
	if (_inverted) { 
		n = (_led_count - 1) - n; 
	}

	uint8_t red = map(r, 0, BRIGHTNESS_MAX, BRIGHTNESS_MIN, _brightness);
	uint8_t green = map(g, 0, BRIGHTNESS_MAX, BRIGHTNESS_MIN, _brightness);
	uint8_t blue = map(b, 0, BRIGHTNESS_MAX, BRIGHTNESS_MIN, _brightness);
	
	ESP_ERROR_CHECK(strip->set_pixel(strip, n, (uint32_t)red, (uint32_t)green, (uint32_t)blue));
}

void WS2812_setPixelColor32(uint16_t n, uint32_t c) {
	uint8_t r = (uint8_t)(c >> 16);
	uint8_t g = (uint8_t)(c >>  8);
	uint8_t b = (uint8_t)c;
	
	WS2812_setPixelColor(n, r, g, b);
}

uint32_t WS2812_getPixelColor(uint16_t n) {
	uint8_t red = 0;
	uint8_t green = 0;
	uint8_t blue = 0;

	ESP_ERROR_CHECK(strip->get_pixel(strip, n, &red, &green, &blue));

	return color32(red, green, blue);
}

void WS2812_clear() {
    ESP_ERROR_CHECK(strip->clear(strip, WS2812_TIMEOUT));
}

void WS2812_init(uint16_t pixel_count) {
	_led_count = WS2812_LED_NUMBER;

    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(WS2812_GPIO, RMT_TX_CHANNEL);
    // set counter clock to 40MHz
    config.clk_div = 2;

    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));
	
    // install ws2812 driver
    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(WS2812_LED_NUMBER, (led_strip_dev_t)config.channel);
    strip = led_strip_new_rmt_ws2812(&strip_config);
    if (!strip) {
        ESP_LOGE(TAG, "install WS2812 driver failed");
    }

	WS2812_clear();
}

//WS2812FX
void WS2812FX_init(uint16_t pixel_count) {
	WS2812_init(pixel_count);
	xTaskCreate(WS2812FX_service, "fxService", 2048, NULL, 2, NULL);
	WS2812FX_initModes();
	WS2812FX_start();
}

void WS2812FX_service(void *_args) {
	uint32_t now = 0;
	
	while (true) {
		if(_running) {
			//printf("_brightness : _target_brightness %ld : %ld \n", _brightness, _target_brightness);
			
            if (_slow_start) {
			    if ((_brightness < _target_brightness)) {
                    uint8_t new_brightness = (BRIGHTNESS_FILTER * _brightness) + ((1.0-BRIGHTNESS_FILTER) * _target_brightness);
                    float soft_start = fconstrain((float)(_brightness * 4) / (float)BRIGHTNESS_MAX, 0.1, 1.0);
                    uint8_t delta = (new_brightness - _brightness) * soft_start;
	                _brightness = _brightness + constrain(delta, 1, delta);
	            } else {
	                _brightness = (BRIGHTNESS_FILTER * _brightness) + ((1.0-BRIGHTNESS_FILTER) * _target_brightness);
	            }
            } else {
                _brightness = _target_brightness;
            }
		
			now = xTaskGetTickCount() * portTICK_PERIOD_MS;
			if(now - _mode_last_call_time > _mode_delay) {
				_counter_mode_call++;
				_mode_last_call_time = now;
				CALL_MODE(_mode_index);
				
				//gpio_toggle(LED_INBUILT_GPIO); //led indicator
			}  
		}
		vTaskDelay(33 / portTICK_PERIOD_MS);
	}
}

void WS2812FX_start() {
	_counter_mode_call = 0;
	_counter_mode_step = 0;
	_running = true;
}

void WS2812FX_stop() {
	_running = false;
}

void WS2812FX_setMode360(float m) {
	//printf("WS2812FX_setMode360: %f", m);
	uint8_t mode = map((uint16_t)m, 0, 360, 0, MODE_COUNT-1);
	//printf("WS2812FX_setMode: %d", mode);
	WS2812FX_setMode(mode);
}

void WS2812FX_setMode(uint8_t m) {
	_counter_mode_call = 0;
	_counter_mode_step = 0;
	_mode_index = constrain(m, 0, MODE_COUNT-1);
	_mode_color = _color;
}

void WS2812FX_setSpeed(uint8_t s) {
	_counter_mode_call = 0;
	_counter_mode_step = 0;
	_speed = constrain(s, SPEED_MIN, SPEED_MAX);
}

void WS2812FX_setColor(uint8_t r, uint8_t g, uint8_t b) {
	WS2812FX_setColor32(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
}

void WS2812FX_setColor32(uint32_t c) {
	_color = c;
	_counter_mode_call = 0;
	_counter_mode_step = 0;
	_mode_color = _color;
}

void WS2812FX_setBrightness(uint8_t b) {
	_target_brightness = constrain(b, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
	//printf("WS2812FX_setBrightness: %ld \n", _target_brightness);
}

void WS2812FX_forceBrightness(uint8_t b) {
	_target_brightness = constrain(b, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
	_brightness = _target_brightness;
}

bool WS2812FX_isRunning() {
	return _running;
}

uint8_t WS2812FX_getMode(void) {
	return _mode_index;
}

uint8_t WS2812FX_getSpeed(void) {
	return _speed;
}

uint8_t WS2812FX_getBrightness(void) {
	return _target_brightness;
}

uint16_t WS2812FX_getLength(void) {
	return _led_count;
}

uint8_t WS2812FX_getModeCount(void) {
	return MODE_COUNT;
}

uint32_t WS2812FX_getColor(void) {
	return _color;
}

void WS2812FX_setInverted(bool inverted) {
	_inverted = inverted;
}

void WS2812FX_setSlowStart(bool slow_start) {
	_slow_start = slow_start;
}

/* #####################################################
#
#  Color and Blinken Functions
#
##################################################### */

/*
* Turns everything off. Doh.
*/
void WS2812FX_strip_off() {
	WS2812_clear();
}

/*
* Put a value 0 to 255 in to get a color value.
* The colours are a transition r -> g -> b -> back to r
* Inspired by the Adafruit examples.
*/
uint32_t WS2812FX_color_wheel(uint8_t pos) {
	pos = 255 - pos;
	if(pos < 85) {
		return ((uint32_t)(255 - pos * 3) << 16) | ((uint32_t)(0) << 8) | (pos * 3);
	} else if(pos < 170) {
		pos -= 85;
		return ((uint32_t)(0) << 16) | ((uint32_t)(pos * 3) << 8) | (255 - pos * 3);
	} else {
		pos -= 170;
		return ((uint32_t)(pos * 3) << 16) | ((uint32_t)(255 - pos * 3) << 8) | (0);
	}
}


/*
* Returns a new, random wheel index with a minimum distance of 42 from pos.
*/
uint8_t WS2812FX_get_random_wheel_index(uint8_t pos) {
	uint8_t r = 0;
	uint8_t x = 0;
	uint8_t y = 0;
	uint8_t d = 0;

	while(d < 42) {
		r = randomInRange(0, 256);
		x = abs(pos - r);
		y = 255 - x;
		d = min(x, y);
	}

	return r;
}


/*
* No blinking. Just plain old static light.
*/
void WS2812FX_mode_static(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, _color);
	}
	WS2812_show();

	_mode_delay = 50;
}


/*
* Normal blinking. 50% on/off time.
*/
void WS2812FX_mode_blink(void) {
	if(_counter_mode_call % 2 == 1) {
		for(uint16_t i=0; i < _led_count; i++) {
			WS2812_setPixelColor32(i, _color);
		}
		WS2812_show();
	} else {
		WS2812FX_strip_off();
	}

	_mode_delay = 100 + ((1986 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
}


/*
* Lights all LEDs after each other up. Then turns them in
* that order off. Repeat.
*/
void WS2812FX_mode_color_wipe(void) {
	if(_counter_mode_step < _led_count) {
		WS2812_setPixelColor32(_counter_mode_step, _color);
	} else {
		WS2812_setPixelColor32(_counter_mode_step - _led_count, 0);
	}
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % (_led_count * 2);

	_mode_delay = 5 + ((50 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* Turns all LEDs after each other to a random color.
* Then starts over with another color.
*/
void WS2812FX_mode_color_wipe_random(void) {
	if(_counter_mode_step == 0) {
		_mode_color = WS2812FX_get_random_wheel_index(_mode_color);
	}

	WS2812_setPixelColor32(_counter_mode_step, WS2812FX_color_wheel(_mode_color));
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % _led_count;

	_mode_delay = 5 + ((50 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* Lights all LEDs in one random color up. Then switches them
* to the next random color.
*/
void WS2812FX_mode_random_color(void) {
	_mode_color = WS2812FX_get_random_wheel_index(_mode_color);

	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, WS2812FX_color_wheel(_mode_color));
	}

	WS2812_show();
	_mode_delay = 100 + ((5000 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
}


/*
* Lights every LED in a random color. Changes one random LED after the other
* to another random color.
*/
void WS2812FX_mode_single_dynamic(void) {
	if(_counter_mode_call == 0) {
		for(uint16_t i=0; i < _led_count; i++) {
			WS2812_setPixelColor32(i, WS2812FX_color_wheel(randomInRange(0, 256)));
		}
	}

	WS2812_setPixelColor32(randomInRange(0, _led_count), WS2812FX_color_wheel(randomInRange(0, 256)));
	WS2812_show();
	_mode_delay = 10 + ((5000 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
}


/*
* Lights every LED in a random color. Changes all LED at the same time
* to new random colors.
*/
void WS2812FX_mode_multi_dynamic(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, WS2812FX_color_wheel(randomInRange(0, 256)));
	}
	WS2812_show();
	_mode_delay = 100 + ((5000 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
}


/*
* Does the "standby-breathing" of well known i-Devices. Fixed Speed.
* Use mode "fade" if you like to have something similar with a different speed.
*/
void WS2812FX_mode_breath(void) {
	//                                      0    1    2   3   4   5   6    7   8   9  10  11   12   13   14   15   16    // step
	uint16_t breath_delay_steps[] =     {   7,   9,  13, 15, 16, 17, 18, 930, 19, 18, 15, 13,   9,   7,   4,   5,  10 }; // magic numbers for breathing LED
	uint8_t breath_brightness_steps[] = { 150, 125, 100, 75, 50, 25, 16,  15, 16, 25, 50, 75, 100, 125, 150, 220, 255 }; // even more magic numbers!

	if(_counter_mode_call == 0) {
		_mode_color = breath_brightness_steps[0] + 1;
	}

	uint8_t breath_brightness = _mode_color; // we use _mode_color to store the brightness

	if(_counter_mode_step < 8) {
		breath_brightness--;
	} else {
		breath_brightness++;
	}

	// update index of current delay when target brightness is reached, start over after the last step
	if(breath_brightness == breath_brightness_steps[_counter_mode_step]) {
		_counter_mode_step = (_counter_mode_step + 1) % (sizeof(breath_brightness_steps)/sizeof(uint8_t));
	}

	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, _color);           // set all LEDs to selected color
	}
	int b = map(breath_brightness, 0, 255, 0, _brightness);  // keep brightness below brightness set by user
	WS2812FX_forceBrightness(b);                     // set new brightness to leds
	WS2812_show();

	_mode_color = breath_brightness;                         // we use _mode_color to store the brightness
	_mode_delay = breath_delay_steps[_counter_mode_step];
}


/*
* Fades the LEDs on and (almost) off again.
*/
void WS2812FX_mode_fade(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, _color);
	}

	int b = _counter_mode_step - 127;
	b = 255 - (abs(b) * 2);
	b = map(b, 0, 255, min(25, _brightness), _brightness);
	WS2812FX_forceBrightness(b);
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % 256;
	_mode_delay = 5 + ((15 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
}


/*
* Runs a single pixel back and forth.
*/
void WS2812FX_mode_scan(void) {
	if(_counter_mode_step > (_led_count*2) - 2) {
		_counter_mode_step = 0;
	}
	_counter_mode_step++;

	int i = _counter_mode_step - (_led_count - 1);
	i = abs(i);

	WS2812_clear();
	WS2812_setPixelColor32(abs(i), _color);
	WS2812_show();

	_mode_delay = 10 + ((30 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* Runs two pixel back and forth in opposite directions.
*/
void WS2812FX_mode_dual_scan(void) {
	if(_counter_mode_step > (_led_count*2) - 2) {
		_counter_mode_step = 0;
	}
	_counter_mode_step++;

	int i = _counter_mode_step - (_led_count - 1);
	i = abs(i);

	WS2812_clear();
	WS2812_setPixelColor32(i, _color);
	WS2812_setPixelColor32(_led_count - (i+1), _color);
	WS2812_show();

	_mode_delay = 10 + ((30 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* Cycles all LEDs at once through a rainbow.
*/
void WS2812FX_mode_rainbow(void) {
	uint32_t color = WS2812FX_color_wheel(_counter_mode_step);
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, color);
	}
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % 256;

	_mode_delay = 1 + ((100 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
}


/*
* Cycles a rainbow over the entire string of LEDs.
*/
void WS2812FX_mode_rainbow_cycle(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, WS2812FX_color_wheel(((i * 256 / _led_count) + _counter_mode_step) % 256));
	}
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % 256;

	_mode_delay = 1 + ((50 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
}


/*
* Theatre-style crawling lights.
* Inspired by the Adafruit examples.
*/
void WS2812FX_mode_theater_chase(void) {
	uint8_t j = _counter_mode_call % 6;
	if(j % 2 == 0) {
		for(uint16_t i=0; i < _led_count; i=i+3) {
			WS2812_setPixelColor32(i+(j/2), _color);
		}
		WS2812_show();
		_mode_delay = 50 + ((500 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
	} else {
		for(uint16_t i=0; i < _led_count; i=i+3) {
			WS2812_setPixelColor32(i+(j/2), 0);
		}
		_mode_delay = 1;
	}
}


/*
* Theatre-style crawling lights with rainbow effect.
* Inspired by the Adafruit examples.
*/
void WS2812FX_mode_theater_chase_rainbow(void) {
	uint8_t j = _counter_mode_call % 6;
	if(j % 2 == 0) {
		for(uint16_t i=0; i < _led_count; i=i+3) {
			WS2812_setPixelColor32(i+(j/2), WS2812FX_color_wheel((i+_counter_mode_step) % 256));
		}
		WS2812_show();
		_mode_delay = 50 + ((500 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
	} else {
		for(uint16_t i=0; i < _led_count; i=i+3) {
			WS2812_setPixelColor32(i+(j/2), 0);
		}
		_mode_delay = 1;
	}
	_counter_mode_step = (_counter_mode_step + 1) % 256;
}


/*
* Running lights effect with smooth sine transition.
*/
void WS2812FX_mode_running_lights(void) {
	uint8_t r = ((_color >> 16) & 0xFF);
	uint8_t g = ((_color >> 8) & 0xFF);
	uint8_t b = (_color & 0xFF);

	for(uint16_t i=0; i < _led_count; i++) {
		int s = (sin(i+_counter_mode_call) * 127) + 128;
		WS2812_setPixelColor(i, (((uint32_t)(r * s)) / 255), (((uint32_t)(g * s)) / 255), (((uint32_t)(b * s)) / 255));
	}

	WS2812_show();

	_mode_delay = 35 + ((350 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
}


/*
* Blink several LEDs on, reset, repeat.
* Inspired by www.tweaking4all.com/hardware/arduino/adruino-led-strip-effects/
*/
void WS2812FX_mode_twinkle(void) {
	if(_counter_mode_step == 0) {
		WS2812FX_strip_off();
		uint32_t min_leds = max(1, _led_count/5); // make sure, at least one LED is on
		uint32_t max_leds = max(1, _led_count/2); // make sure, at least one LED is on
		_counter_mode_step = randomInRange(min_leds, max_leds);
	}

	WS2812_setPixelColor32(randomInRange(0, _led_count), _mode_color);
	WS2812_show();

	_counter_mode_step--;
	_mode_delay = 50 + ((1986 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
}


/*
* Blink several LEDs in random colors on, reset, repeat.
* Inspired by www.tweaking4all.com/hardware/arduino/adruino-led-strip-effects/
*/
void WS2812FX_mode_twinkle_random(void) {
	_mode_color = WS2812FX_color_wheel(randomInRange(0, 256));
	WS2812FX_mode_twinkle();
}


/*
* Blink several LEDs on, fading out.
*/
void WS2812FX_mode_twinkle_fade(void) {

	for(uint16_t i=0; i < _led_count; i++) {
		uint32_t px_rgb = WS2812_getPixelColor(i);

		uint8_t px_r = (px_rgb & 0x00FF0000) >> 16;
		uint8_t px_g = (px_rgb & 0x0000FF00) >>  8;
		uint8_t px_b = (px_rgb & 0x000000FF) >>  0;

		// fade out (divide by 2)
		px_r = px_r >> 1;
		px_g = px_g >> 1;
		px_b = px_b >> 1;

		WS2812_setPixelColor(i, px_r, px_g, px_b);
	}

	if(randomInRange(0, 3) == 0) {
		WS2812_setPixelColor32(randomInRange(0, _led_count), _mode_color);
	}

	WS2812_show();

	_mode_delay = 100 + ((100 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
}


/*
* Blink several LEDs in random colors on, fading out.
*/
void WS2812FX_mode_twinkle_fade_random(void) {
	_mode_color = WS2812FX_color_wheel(randomInRange(0, 256));
	WS2812FX_mode_twinkle_fade();
}


/*
* Blinks one LED at a time.
* Inspired by www.tweaking4all.com/hardware/arduino/adruino-led-strip-effects/
*/
void WS2812FX_mode_sparkle(void) {
	WS2812_clear();
	WS2812_setPixelColor32(randomInRange(0, _led_count),_color);
	WS2812_show();
	_mode_delay = 10 + ((200 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
}


/*
* Lights all LEDs in the _color. Flashes single white pixels randomly.
* Inspired by www.tweaking4all.com/hardware/arduino/adruino-led-strip-effects/
*/
void WS2812FX_mode_flash_sparkle(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, _color);
	}

	if(randomInRange(0, 10) == 7) {
		WS2812_setPixelColor(randomInRange(0, _led_count), 255, 255, 255);
		_mode_delay = 20;
	} else {
		_mode_delay = 20 + ((200 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
	}

	WS2812_show();
}


/*
* Like flash sparkle. With more flash.
* Inspired by www.tweaking4all.com/hardware/arduino/adruino-led-strip-effects/
*/
void WS2812FX_mode_hyper_sparkle(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, _color);
	}

	if(randomInRange(0, 10) < 4) {
		for(uint16_t i=0; i < max(1, _led_count/3); i++) {
			WS2812_setPixelColor(randomInRange(0, _led_count), 255, 255, 255);
		}
		_mode_delay = 20;
	} else {
		_mode_delay = 15 + ((120 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
	}

	WS2812_show();
}


/*
* Classic Strobe effect.
*/
void WS2812FX_mode_strobe(void) {
	if(_counter_mode_call % 2 == 0) {
		for(uint16_t i=0; i < _led_count; i++) {
			WS2812_setPixelColor32(i, _color);
		}
		_mode_delay = 20;
	} else {
		for(uint16_t i=0; i < _led_count; i++) {
			WS2812_setPixelColor32(i, 0);
		}
		_mode_delay = 50 + ((1986 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
	}
	WS2812_show();
}


/*
* Strobe effect with different strobe count and pause, controled by _speed.
*/
void WS2812FX_mode_multi_strobe(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, 0);
	}

	if(_counter_mode_step < (2 * ((_speed / 10) + 1))) {
		if(_counter_mode_step % 2 == 0) {
			for(uint16_t i=0; i < _led_count; i++) {
				WS2812_setPixelColor32(i, _color);
			}
			_mode_delay = 20;
		} else {
			_mode_delay = 50;
		}

	} else {
		_mode_delay = 100 + ((9 - (_speed % 10)) * 125);
	}

	WS2812_show();
	_counter_mode_step = (_counter_mode_step + 1) % ((2 * ((_speed / 10) + 1)) + 1);
}


/*
* Classic Strobe effect. Cycling through the rainbow.
*/
void WS2812FX_mode_strobe_rainbow(void) {
	if(_counter_mode_call % 2 == 0) {
		for(uint16_t i=0; i < _led_count; i++) {
			WS2812_setPixelColor32(i, WS2812FX_color_wheel(_counter_mode_call % 256));
		}
		_mode_delay = 20;
	} else {
		for(uint16_t i=0; i < _led_count; i++) {
			WS2812_setPixelColor32(i, 0);
		}
		_mode_delay = 50 + ((1986 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
	}
	WS2812_show();
}


/*
* Classic Blink effect. Cycling through the rainbow.
*/
void WS2812FX_mode_blink_rainbow(void) {
	if(_counter_mode_call % 2 == 1) {
		for(uint16_t i=0; i < _led_count; i++) {
			WS2812_setPixelColor32(i, WS2812FX_color_wheel(_counter_mode_call % 256));
		}
		WS2812_show();
	} else {
		WS2812FX_strip_off();
	}

	_mode_delay = 100 + ((1986 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
}


/*
* _color running on white.
*/
void WS2812FX_mode_chase_white(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor(i, 255, 255, 255);
	}

	uint16_t n = _counter_mode_step;
	uint16_t m = (_counter_mode_step + 1) % _led_count;
	WS2812_setPixelColor32(n, _color);
	WS2812_setPixelColor32(m, _color);
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % _led_count;
	_mode_delay = 10 + ((30 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* White running on _color.
*/
void WS2812FX_mode_chase_color(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, _color);
	}

	uint16_t n = _counter_mode_step;
	uint16_t m = (_counter_mode_step + 1) % _led_count;
	WS2812_setPixelColor(n, 255, 255, 255);
	WS2812_setPixelColor(m, 255, 255, 255);
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % _led_count;
	_mode_delay = 10 + ((30 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* White running followed by random color.
*/
void WS2812FX_mode_chase_random(void) {
	if(_counter_mode_step == 0) {
		WS2812_setPixelColor32(_led_count-1, WS2812FX_color_wheel(_mode_color));
		_mode_color = WS2812FX_get_random_wheel_index(_mode_color);
	}

	for(uint16_t i=0; i < _counter_mode_step; i++) {
		WS2812_setPixelColor32(i, WS2812FX_color_wheel(_mode_color));
	}

	uint16_t n = _counter_mode_step;
	uint16_t m = (_counter_mode_step + 1) % _led_count;
	WS2812_setPixelColor(n, 255, 255, 255);
	WS2812_setPixelColor(m, 255, 255, 255);

	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % _led_count;
	_mode_delay = 10 + ((30 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* White running on rainbow.
*/
void WS2812FX_mode_chase_rainbow(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, WS2812FX_color_wheel(((i * 256 / _led_count) + (_counter_mode_call % 256)) % 256));
	}

	uint16_t n = _counter_mode_step;
	uint16_t m = (_counter_mode_step + 1) % _led_count;
	WS2812_setPixelColor(n, 255, 255, 255);
	WS2812_setPixelColor(m, 255, 255, 255);
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % _led_count;
	_mode_delay = 10 + ((30 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* White flashes running on _color.
*/
void WS2812FX_mode_chase_flash(void) {
	const static uint8_t flash_count = 4;
	uint8_t flash_step = _counter_mode_call % ((flash_count * 2) + 1);

	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, _color);
	}

	if(flash_step < (flash_count * 2)) {
		if(flash_step % 2 == 0) {
			uint16_t n = _counter_mode_step;
			uint16_t m = (_counter_mode_step + 1) % _led_count;
			WS2812_setPixelColor(n, 255, 255, 255);
			WS2812_setPixelColor(m, 255, 255, 255);
			_mode_delay = 20;
		} else {
			_mode_delay = 30;
		}
	} else {
		_counter_mode_step = (_counter_mode_step + 1) % _led_count;
		_mode_delay = 10 + ((30 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
	}

	WS2812_show();
}


/*
* White flashes running, followed by random color.
*/
void WS2812FX_mode_chase_flash_random(void) {
	const static uint8_t flash_count = 4;
	uint8_t flash_step = _counter_mode_call % ((flash_count * 2) + 1);

	for(uint16_t i=0; i < _counter_mode_step; i++) {
		WS2812_setPixelColor32(i, WS2812FX_color_wheel(_mode_color));
	}

	if(flash_step < (flash_count * 2)) {
		uint16_t n = _counter_mode_step;
		uint16_t m = (_counter_mode_step + 1) % _led_count;
		if(flash_step % 2 == 0) {
			WS2812_setPixelColor(n, 255, 255, 255);
			WS2812_setPixelColor(m, 255, 255, 255);
			_mode_delay = 20;
		} else {
			WS2812_setPixelColor32(n, WS2812FX_color_wheel(_mode_color));
			WS2812_setPixelColor(m, 0, 0, 0);
			_mode_delay = 30;
		}
	} else {
		_counter_mode_step = (_counter_mode_step + 1) % _led_count;
		_mode_delay = 1 + ((10 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);

		if(_counter_mode_step == 0) {
			_mode_color = WS2812FX_get_random_wheel_index(_mode_color);
		}
	}

	WS2812_show();
}


/*
* Rainbow running on white.
*/
void WS2812FX_mode_chase_rainbow_white(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor(i, 255, 255, 255);
	}

	uint16_t n = _counter_mode_step;
	uint16_t m = (_counter_mode_step + 1) % _led_count;
	WS2812_setPixelColor32(n, WS2812FX_color_wheel(((n * 256 / _led_count) + (_counter_mode_call % 256)) % 256));
	WS2812_setPixelColor32(m, WS2812FX_color_wheel(((m * 256 / _led_count) + (_counter_mode_call % 256)) % 256));
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % _led_count;
	_mode_delay = 10 + ((30 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* Black running on _color.
*/
void WS2812FX_mode_chase_blackout(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, _color);
	}

	uint16_t n = _counter_mode_step;
	uint16_t m = (_counter_mode_step + 1) % _led_count;
	WS2812_setPixelColor(n, 0, 0, 0);
	WS2812_setPixelColor(m, 0, 0, 0);
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % _led_count;
	_mode_delay = 10 + ((30 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* Black running on rainbow.
*/
void WS2812FX_mode_chase_blackout_rainbow(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		WS2812_setPixelColor32(i, WS2812FX_color_wheel(((i * 256 / _led_count) + (_counter_mode_call % 256)) % 256));
	}

	uint16_t n = _counter_mode_step;
	uint16_t m = (_counter_mode_step + 1) % _led_count;
	WS2812_setPixelColor(n, 0, 0, 0);
	WS2812_setPixelColor(m, 0, 0, 0);
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % _led_count;
	_mode_delay = 10 + ((30 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* Random color intruduced alternating from start and end of strip.
*/
void WS2812FX_mode_color_sweep_random(void) {
	if(_counter_mode_step == 0 || _counter_mode_step == _led_count) {
		_mode_color = WS2812FX_get_random_wheel_index(_mode_color);
	}

	if(_counter_mode_step < _led_count) {
		WS2812_setPixelColor32(_counter_mode_step, WS2812FX_color_wheel(_mode_color));
	} else {
		WS2812_setPixelColor32((_led_count * 2) - _counter_mode_step - 1, WS2812FX_color_wheel(_mode_color));
	}
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % (_led_count * 2);
	_mode_delay = 5 + ((50 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* Alternating color/white pixels running.
*/
void WS2812FX_mode_running_color(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		if((i + _counter_mode_step) % 4 < 2) {
			WS2812_setPixelColor32(i, _mode_color);
		} else {
			WS2812_setPixelColor(i, 255, 255, 255);
		}
	}
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % 4;
	_mode_delay = 10 + ((30 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* Alternating red/blue pixels running.
*/
void WS2812FX_mode_running_red_blue(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		if((i + _counter_mode_step) % 4 < 2) {
			WS2812_setPixelColor(i, 255, 0, 0);
		} else {
			WS2812_setPixelColor(i, 0, 0, 255);
		}
	}
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % 4;
	_mode_delay = 100 + ((100 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* Random colored pixels running.
*/
void WS2812FX_mode_running_random(void) {
	for(uint16_t i=_led_count-1; i > 0; i--) {
		WS2812_setPixelColor32(i, WS2812_getPixelColor(i-1));
	}

	if(_counter_mode_step == 0) {
		_mode_color = WS2812FX_get_random_wheel_index(_mode_color);
		WS2812_setPixelColor32(0, WS2812FX_color_wheel(_mode_color));
	}

	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % 2;

	_mode_delay = 50 + ((50 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* K.I.T.T.
*/
void WS2812FX_mode_larson_scanner(void) {

	for(uint16_t i=0; i < _led_count; i++) {
		uint32_t px_rgb = WS2812_getPixelColor(i);

		uint8_t px_r = (px_rgb & 0x00FF0000) >> 16;
		uint8_t px_g = (px_rgb & 0x0000FF00) >>  8;
		uint8_t px_b = (px_rgb & 0x000000FF) >>  0;

		// fade out (divide by 2)
		px_r = px_r >> 1;
		px_g = px_g >> 1;
		px_b = px_b >> 1;

		WS2812_setPixelColor(i, px_r, px_g, px_b);
	}

	uint16_t pos = 0;

	if(_counter_mode_step < _led_count) {
		pos = _counter_mode_step;
	} else {
		pos = (_led_count * 2) - _counter_mode_step - 2;
	}

	WS2812_setPixelColor32(pos, _color);
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % ((_led_count * 2) - 2);
	_mode_delay = 10 + ((10 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* Fireing comets from one end.
*/
void WS2812FX_mode_comet(void) {

	for(uint16_t i=0; i < _led_count; i++) {
		uint32_t px_rgb = WS2812_getPixelColor(i);

		uint8_t px_r = (px_rgb & 0x00FF0000) >> 16;
		uint8_t px_g = (px_rgb & 0x0000FF00) >>  8;
		uint8_t px_b = (px_rgb & 0x000000FF) >>  0;

		// fade out (divide by 2)
		px_r = px_r >> 1;
		px_g = px_g >> 1;
		px_b = px_b >> 1;

		WS2812_setPixelColor(i, px_r, px_g, px_b);
	}

	WS2812_setPixelColor32(_counter_mode_step, _color);
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % _led_count;
	_mode_delay = 10 + ((10 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* Firework sparks.
*/
void WS2812FX_mode_fireworks(void) {
	uint32_t px_rgb = 0;
	uint8_t px_r = 0;
	uint8_t px_g = 0;
	uint8_t px_b = 0;

	for(uint16_t i=0; i < _led_count; i++) {
		px_rgb = WS2812_getPixelColor(i);

		px_r = (px_rgb & 0x00FF0000) >> 16;
		px_g = (px_rgb & 0x0000FF00) >>  8;
		px_b = (px_rgb & 0x000000FF) >>  0;

		// fade out (divide by 2)
		px_r = px_r >> 1;
		px_g = px_g >> 1;
		px_b = px_b >> 1;

		WS2812_setPixelColor(i, px_r, px_g, px_b);
	}

	// first LED has only one neighbour
	px_r = (((WS2812_getPixelColor(1) & 0x00FF0000) >> 16) >> 1) + ((WS2812_getPixelColor(0) & 0x00FF0000) >> 16);
	px_g = (((WS2812_getPixelColor(1) & 0x0000FF00) >>  8) >> 1) + ((WS2812_getPixelColor(0) & 0x0000FF00) >>  8);
	px_b = (((WS2812_getPixelColor(1) & 0x000000FF) >>  0) >> 1) + ((WS2812_getPixelColor(0) & 0x000000FF) >>  0);
	WS2812_setPixelColor(0, px_r, px_g, px_b);

	// set brightness(i) = ((brightness(i-1)/2 + brightness(i+1)) / 2) + brightness(i)
	for(uint16_t i=1; i < _led_count-1; i++) {
		px_r = ((
			(((WS2812_getPixelColor(i-1) & 0x00FF0000) >> 16) >> 1) +
				(((WS2812_getPixelColor(i+1) & 0x00FF0000) >> 16) >> 0) ) >> 1) +
					(((WS2812_getPixelColor(i  ) & 0x00FF0000) >> 16) >> 0);

		px_g = ((
			(((WS2812_getPixelColor(i-1) & 0x0000FF00) >> 8) >> 1) +
				(((WS2812_getPixelColor(i+1) & 0x0000FF00) >> 8) >> 0) ) >> 1) +
					(((WS2812_getPixelColor(i  ) & 0x0000FF00) >> 8) >> 0);

		px_b = ((
			(((WS2812_getPixelColor(i-1) & 0x000000FF) >> 0) >> 1) +
				(((WS2812_getPixelColor(i+1) & 0x000000FF) >> 0) >> 0) ) >> 1) +
					(((WS2812_getPixelColor(i  ) & 0x000000FF) >> 0) >> 0);

		WS2812_setPixelColor(i, px_r, px_g, px_b);
	}

	// last LED has only one neighbour
	px_r = (((WS2812_getPixelColor(_led_count-2) & 0x00FF0000) >> 16) >> 2) + ((WS2812_getPixelColor(_led_count-1) & 0x00FF0000) >> 16);
	px_g = (((WS2812_getPixelColor(_led_count-2) & 0x0000FF00) >>  8) >> 2) + ((WS2812_getPixelColor(_led_count-1) & 0x0000FF00) >>  8);
	px_b = (((WS2812_getPixelColor(_led_count-2) & 0x000000FF) >>  0) >> 2) + ((WS2812_getPixelColor(_led_count-1) & 0x000000FF) >>  0);
	WS2812_setPixelColor(_led_count-1, px_r, px_g, px_b);

	for(uint16_t i=0; i<max(1,_led_count/20); i++) {
		if(randomInRange(0, 10) == 0) {
			WS2812_setPixelColor32(randomInRange(0, _led_count), _mode_color);
		}
	}

	WS2812_show();

	_mode_delay = 20 + ((20 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}


/*
* Random colored firework sparks.
*/
void WS2812FX_mode_fireworks_random(void) {
	_mode_color = WS2812FX_color_wheel(randomInRange(0, 256));
	WS2812FX_mode_fireworks();
}


/*
* Alternating red/green pixels running.
*/
void WS2812FX_mode_merry_christmas(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		if((i + _counter_mode_step) % 4 < 2) {
			WS2812_setPixelColor(i, 255, 0, 0);
		} else {
			WS2812_setPixelColor(i, 0, 255, 0);
		}
	}
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % 4;
	_mode_delay = 100 + ((100 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}

/*
* Alternating red/green pixels running.
*/
void WS2812FX_mode_halloween(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		if((i + _counter_mode_step) % 4 < 2) {
			WS2812_setPixelColor(i, 255, 0, 130);
		} else {
			WS2812_setPixelColor(i, 255, 50, 0);
		}
	}
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % 4;
	_mode_delay = 100 + ((100 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}

/*
* Random flickering.
*/
void WS2812FX_mode_fire_flicker(void) {
	WS2812FX_mode_fire_flicker_int(3);
}

/*
* Random flickering, less intesity.
*/
void WS2812FX_mode_fire_flicker_soft(void) {
	WS2812FX_mode_fire_flicker_int(6);
}

void WS2812FX_mode_fire_flicker_intense(void) {
	WS2812FX_mode_fire_flicker_int(1.7);
}

void WS2812FX_mode_fire_flicker_int(int rev_intensity)
{
	uint8_t p_r = (_color & 0x00FF0000) >> 16;
	uint8_t p_g = (_color & 0x0000FF00) >>  8;
	uint8_t p_b = (_color & 0x000000FF) >>  0;
	uint8_t flicker_val = max(p_r,max(p_g, p_b))/rev_intensity;
	for(uint16_t i=0; i < _led_count; i++)
	{
		int flicker = randomInRange(0, flicker_val);
		int r1 = p_r-flicker;
		int g1 = p_g-flicker;
		int b1 = p_b-flicker;
		if(g1<0) g1=0;
		if(r1<0) r1=0;
		if(b1<0) b1=0;
		WS2812_setPixelColor(i,r1,g1, b1);
	}
	WS2812_show();
	_mode_delay = 10 + ((500 * (uint32_t)(SPEED_MAX - _speed)) / SPEED_MAX);
}

/*
* Lights all LEDs after each other up starting from the outer edges and
* finishing in the middle. Then turns them in reverse order off. Repeat.
*/
void WS2812FX_mode_dual_color_wipe_in_out(void) {
	int end = _led_count - _counter_mode_step - 1;
	bool odd = (_led_count % 2);
	int mid = odd ? ((_led_count / 2) + 1) : (_led_count / 2);
	if (_counter_mode_step < mid) {
		WS2812_setPixelColor32(_counter_mode_step, _color);
		WS2812_setPixelColor32(end, _color);
	}
	else {
		if (odd) {
			// If odd, we need to 'double count' the center LED (once to turn it on,
			// once to turn it off). So trail one behind after the middle LED.
			WS2812_setPixelColor32(_counter_mode_step - 1, 0);
			WS2812_setPixelColor32(end + 1, 0);
		} else {
			WS2812_setPixelColor32(_counter_mode_step, 0);
			WS2812_setPixelColor32(end, 0);
		}
	}

	_counter_mode_step++;
	if (odd) {
		if (_counter_mode_step > _led_count) {
			_counter_mode_step = 0;
		}
	} else {
		if (_counter_mode_step >= _led_count) {
			_counter_mode_step = 0;
		}
	}

	WS2812_show();

	_mode_delay = 5 + ((50 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}

/*
* Lights all LEDs after each other up starting from the outer edges and
* finishing in the middle. Then turns them in that order off. Repeat.
*/
void WS2812FX_mode_dual_color_wipe_in_in(void) {
	bool odd = (_led_count % 2);
	int mid = _led_count / 2;
	if (odd) {
		if (_counter_mode_step <= mid) {
			WS2812_setPixelColor32(_counter_mode_step, _color);
			WS2812_setPixelColor32(_led_count - _counter_mode_step - 1, _color);
		} else {
			int i = _counter_mode_step - mid;
			WS2812_setPixelColor32(i - 1, 0);
			WS2812_setPixelColor32(_led_count - i, 0);
		}
	} else {
		if (_counter_mode_step < mid) {
			WS2812_setPixelColor32(_counter_mode_step, _color);
			WS2812_setPixelColor32(_led_count - _counter_mode_step - 1, _color);
		} else {
			int i = _counter_mode_step - mid;
			WS2812_setPixelColor32(i, 0);
			WS2812_setPixelColor32(_led_count - i - 1, 0);
		}
	}

	_counter_mode_step++;
	if (odd) {
		if (_counter_mode_step > _led_count) {
			_counter_mode_step = 0;
		}
	} else {
		if (_counter_mode_step >= _led_count) {
			_counter_mode_step = 0;
		}
	}

	WS2812_show();

	_mode_delay = 5 + ((50 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}

/*
* Lights all LEDs after each other up starting from the middle and
* finishing at the edges. Then turns them in that order off. Repeat.
*/
void WS2812FX_mode_dual_color_wipe_out_out(void) {
	int end = _led_count - _counter_mode_step - 1;
	bool odd = (_led_count % 2);
	int mid = _led_count / 2;

	if (odd) {
		if (_counter_mode_step <= mid) {
			WS2812_setPixelColor32(mid + _counter_mode_step, _color);
			WS2812_setPixelColor32(mid - _counter_mode_step, _color);
		} else {
			WS2812_setPixelColor32(_counter_mode_step - 1, 0);
			WS2812_setPixelColor32(end + 1, 0);
		}
	} else {
		if (_counter_mode_step < mid) {
			WS2812_setPixelColor32(mid - _counter_mode_step - 1, _color);
			WS2812_setPixelColor32(mid + _counter_mode_step, _color);
		} else {
			WS2812_setPixelColor32(_counter_mode_step, 0);
			WS2812_setPixelColor32(end, 0);
		}
	}

	_counter_mode_step++;
	if (odd) {
		if (_counter_mode_step > _led_count) {
			_counter_mode_step = 0;
		}
	} else {
		if (_counter_mode_step >= _led_count) {
			_counter_mode_step = 0;
		}
	}

	WS2812_show();

	_mode_delay = 5 + ((50 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}

/*
* Lights all LEDs after each other up starting from the middle and
* finishing at the edges. Then turns them in reverse order off. Repeat.
*/
void WS2812FX_mode_dual_color_wipe_out_in(void) {
	bool odd = (_led_count % 2);
	int mid = _led_count / 2;

	if (odd) {
		if (_counter_mode_step <= mid) {
			WS2812_setPixelColor32(mid + _counter_mode_step, _color);
			WS2812_setPixelColor32(mid - _counter_mode_step, _color);
		} else {
			int i = _counter_mode_step - mid;
			WS2812_setPixelColor32(i - 1, 0);
			WS2812_setPixelColor32(_led_count - i, 0);
		}
	} else {
		if (_counter_mode_step < mid) {
			WS2812_setPixelColor32(mid - _counter_mode_step - 1, _color);
			WS2812_setPixelColor32(mid + _counter_mode_step, _color);
		} else {
			int i = _counter_mode_step - mid;
			WS2812_setPixelColor32(i, 0);
			WS2812_setPixelColor32(_led_count - i - 1, 0);
		}
	}

	_counter_mode_step++;
	if (odd) {
		if (_counter_mode_step > _led_count) {
			_counter_mode_step = 0;
		}
	} else {
		if (_counter_mode_step >= _led_count) {
			_counter_mode_step = 0;
		}
	}

	WS2812_show();

	_mode_delay = 5 + ((50 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}

/*
* Alternating white/red/black pixels running.
*/
void WS2812FX_mode_circus_combustus(void) {
	for(uint16_t i=0; i < _led_count; i++) {
		if((i + _counter_mode_step) % 6 < 2) {
			WS2812_setPixelColor(i, 255, 0, 0);
		} else if((i + _counter_mode_step) % 6 < 4){
			WS2812_setPixelColor(i, 255, 255, 255);
		} else {
			WS2812_setPixelColor(i, 0, 0, 0);
		}
	}
	WS2812_show();

	_counter_mode_step = (_counter_mode_step + 1) % 6;
	_mode_delay = 100 + ((100 * (uint32_t)(SPEED_MAX - _speed)) / _led_count);
}

void WS2812FX_initModes() {
	_mode[FX_MODE_STATIC]                  = &WS2812FX_mode_static;
	_mode[FX_MODE_BLINK]                   = &WS2812FX_mode_blink;
	_mode[FX_MODE_BREATH]                  = &WS2812FX_mode_breath;
	_mode[FX_MODE_COLOR_WIPE]              = &WS2812FX_mode_color_wipe;
	_mode[FX_MODE_COLOR_WIPE_RANDOM]       = &WS2812FX_mode_color_wipe_random;
	_mode[FX_MODE_RANDOM_COLOR]            = &WS2812FX_mode_random_color;
	_mode[FX_MODE_SINGLE_DYNAMIC]          = &WS2812FX_mode_single_dynamic;
	_mode[FX_MODE_MULTI_DYNAMIC]           = &WS2812FX_mode_multi_dynamic;
	_mode[FX_MODE_RAINBOW]                 = &WS2812FX_mode_rainbow;
	_mode[FX_MODE_RAINBOW_CYCLE]           = &WS2812FX_mode_rainbow_cycle;
	_mode[FX_MODE_SCAN]                    = &WS2812FX_mode_scan;
	_mode[FX_MODE_DUAL_SCAN]               = &WS2812FX_mode_dual_scan;
	_mode[FX_MODE_FADE]                    = &WS2812FX_mode_fade;
	_mode[FX_MODE_THEATER_CHASE]           = &WS2812FX_mode_theater_chase;
	_mode[FX_MODE_THEATER_CHASE_RAINBOW]   = &WS2812FX_mode_theater_chase_rainbow;
	_mode[FX_MODE_RUNNING_LIGHTS]          = &WS2812FX_mode_running_lights;
	_mode[FX_MODE_TWINKLE]                 = &WS2812FX_mode_twinkle;
	_mode[FX_MODE_TWINKLE_RANDOM]          = &WS2812FX_mode_twinkle_random;
	_mode[FX_MODE_TWINKLE_FADE]            = &WS2812FX_mode_twinkle_fade;
	_mode[FX_MODE_TWINKLE_FADE_RANDOM]     = &WS2812FX_mode_twinkle_fade_random;
	_mode[FX_MODE_SPARKLE]                 = &WS2812FX_mode_sparkle;
	_mode[FX_MODE_FLASH_SPARKLE]           = &WS2812FX_mode_flash_sparkle;
	_mode[FX_MODE_HYPER_SPARKLE]           = &WS2812FX_mode_hyper_sparkle;
	_mode[FX_MODE_STROBE]                  = &WS2812FX_mode_strobe;
	_mode[FX_MODE_STROBE_RAINBOW]          = &WS2812FX_mode_strobe_rainbow;
	_mode[FX_MODE_MULTI_STROBE]            = &WS2812FX_mode_multi_strobe;
	_mode[FX_MODE_BLINK_RAINBOW]           = &WS2812FX_mode_blink_rainbow;
	_mode[FX_MODE_CHASE_WHITE]             = &WS2812FX_mode_chase_white;
	_mode[FX_MODE_CHASE_COLOR]             = &WS2812FX_mode_chase_color;
	_mode[FX_MODE_CHASE_RANDOM]            = &WS2812FX_mode_chase_random;
	_mode[FX_MODE_CHASE_RAINBOW]           = &WS2812FX_mode_chase_rainbow;
	_mode[FX_MODE_CHASE_FLASH]             = &WS2812FX_mode_chase_flash;
	_mode[FX_MODE_CHASE_FLASH_RANDOM]      = &WS2812FX_mode_chase_flash_random;
	_mode[FX_MODE_CHASE_RAINBOW_WHITE]     = &WS2812FX_mode_chase_rainbow_white;
	_mode[FX_MODE_CHASE_BLACKOUT]          = &WS2812FX_mode_chase_blackout;
	_mode[FX_MODE_CHASE_BLACKOUT_RAINBOW]  = &WS2812FX_mode_chase_blackout_rainbow;
	_mode[FX_MODE_COLOR_SWEEP_RANDOM]      = &WS2812FX_mode_color_sweep_random;
	_mode[FX_MODE_RUNNING_COLOR]           = &WS2812FX_mode_running_color;
	_mode[FX_MODE_RUNNING_RED_BLUE]        = &WS2812FX_mode_running_red_blue;
	_mode[FX_MODE_RUNNING_RANDOM]          = &WS2812FX_mode_running_random;
	_mode[FX_MODE_LARSON_SCANNER]          = &WS2812FX_mode_larson_scanner;
	_mode[FX_MODE_COMET]                   = &WS2812FX_mode_comet;
	_mode[FX_MODE_FIREWORKS]               = &WS2812FX_mode_fireworks;
	_mode[FX_MODE_FIREWORKS_RANDOM]        = &WS2812FX_mode_fireworks_random;
	_mode[FX_MODE_MERRY_CHRISTMAS]         = &WS2812FX_mode_merry_christmas;
	_mode[FX_MODE_FIRE_FLICKER]            = &WS2812FX_mode_fire_flicker;
	_mode[FX_MODE_FIRE_FLICKER_SOFT]       = &WS2812FX_mode_fire_flicker_soft;
	_mode[FX_MODE_FIRE_FLICKER_INTENSE]    = &WS2812FX_mode_fire_flicker_intense;
	_mode[FX_MODE_DUAL_COLOR_WIPE_IN_OUT]  = &WS2812FX_mode_dual_color_wipe_in_out;
	_mode[FX_MODE_DUAL_COLOR_WIPE_IN_IN]   = &WS2812FX_mode_dual_color_wipe_in_in;
	_mode[FX_MODE_DUAL_COLOR_WIPE_OUT_OUT] = &WS2812FX_mode_dual_color_wipe_out_out;
	_mode[FX_MODE_DUAL_COLOR_WIPE_OUT_IN]  = &WS2812FX_mode_dual_color_wipe_out_in;
	_mode[FX_MODE_CIRCUS_COMBUSTUS]        = &WS2812FX_mode_circus_combustus;
	_mode[FX_MODE_HALLOWEEN]               = &WS2812FX_mode_halloween;
}
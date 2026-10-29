#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "driver/dac_continuous.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tap.h"

#define WIDTH 320
#define HEIGHT 240
#define MAX_FILES 96
#define NAME_SIZE 256
#define PAGE_ROWS 5
#define SAMPLE_RATE 250000
#define AUDIO_BUFFER 2048
#define TOUCH_MIN 200
#define TOUCH_MAX 3900
#define TOUCH_PRESSURE 400
#define LCD_DC GPIO_NUM_2
#define LCD_BL GPIO_NUM_21
#define TOUCH_CS GPIO_NUM_33
#define TOUCH_CLK GPIO_NUM_25
#define TOUCH_MOSI GPIO_NUM_32
#define TOUCH_MISO GPIO_NUM_39
#define TOUCH_IRQ GPIO_NUM_36
#define CALIBRATION_FILE "/sdcard/calibration.json"

#define INK 0x18e3
#define PAPER 0xef5b
#define WHITE 0xffff
#define MUTED 0x94b2
#define GREEN 0x35b1
#define RED 0xf248
#define YELLOW 0xff07

static const char *TAG = "tape";
static spi_device_handle_t display;
static spi_device_handle_t touch_spi;
static uint8_t *frame;
static uint16_t *stripe;
static const uint16_t palette[] = {INK, PAPER, WHITE, MUTED, GREEN, RED, YELLOW};
static char files[MAX_FILES][NAME_SIZE];
static unsigned file_count;
static unsigned selected;
static unsigned page;
static bool truncated_list;
static bool sd_mounted;
static bool flash_mounted;
static const char *library_root = "/sdcard";
static bool cassette_view;
static sdmmc_card_t *sd_card;
static atomic_bool cancel_playback;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;

// screen = coefficient[0] * raw_x + coefficient[1] * raw_y + coefficient[2]
typedef struct {
	float x[3];
	float y[3];
} touch_calibration_t;

static const int calibration_targets[3][2] = {{32, 32}, {288, 120}, {160, 208}};
static touch_calibration_t calibration = {
	.x = {0, (float)(WIDTH - 1) / (TOUCH_MAX - TOUCH_MIN),
		  -(float)TOUCH_MIN * (WIDTH - 1) / (TOUCH_MAX - TOUCH_MIN)},
	.y = {-(float)(HEIGHT - 1) / (TOUCH_MAX - TOUCH_MIN), 0,
		  (float)TOUCH_MAX * (HEIGHT - 1) / (TOUCH_MAX - TOUCH_MIN)}
};

typedef struct {
	bool playing;
	unsigned percent;
	unsigned block;
	char status[40];
} player_state_t;

static player_state_t player = {.status = "READY"};

static const uint8_t letters[][5] = {
	{0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
	{0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c},
	{0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
	{0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f},
	{0x41,0x41,0x7f,0x41,0x41}, {0x20,0x40,0x41,0x3f,0x01},
	{0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
	{0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
	{0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06},
	{0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
	{0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
	{0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
	{0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
	{0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43}
};
static const uint8_t digits[][5] = {
	{0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
	{0x62,0x51,0x49,0x49,0x46}, {0x22,0x41,0x49,0x49,0x36},
	{0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
	{0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
	{0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e}
};

static void rect(int left, int top, int width, int height, uint16_t color)
{
	int right = left + width;
	int bottom = top + height;
	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right > WIDTH) right = WIDTH;
	if (bottom > HEIGHT) bottom = HEIGHT;
	uint8_t palette_index = 0;
	for (unsigned index = 0; index < sizeof(palette) / sizeof(palette[0]); ++index) {
		if (palette[index] == color) palette_index = index;
	}
	for (int row = top; row < bottom; ++row) {
		for (int column = left; column < right; ++column) {
			frame[row * WIDTH + column] = palette_index;
		}
	}
}

static void text(int left, int top, const char *value, int scale, uint16_t color, int max_chars)
{
	for (int index = 0; value[index] && index < max_chars; ++index) {
		unsigned char symbol = toupper((unsigned char)value[index]);
		uint8_t glyph[5] = {0};
		if (symbol >= 'A' && symbol <= 'Z') memcpy(glyph, letters[symbol - 'A'], 5);
		else if (symbol >= '0' && symbol <= '9') memcpy(glyph, digits[symbol - '0'], 5);
		else if (symbol == '.') glyph[2] = 0x60;
		else if (symbol == ':') glyph[2] = 0x36;
		else if (symbol == '-') memset(glyph, 0x08, 5);
		else if (symbol == '_') memset(glyph, 0x40, 5);
		else if (symbol == '/') {
			for (int column = 0; column < 5; ++column) glyph[column] = 0x40 >> column;
		} else if (symbol == '%') {
			glyph[0] = 0x63; glyph[1] = 0x13; glyph[2] = 0x08;
			glyph[3] = 0x64; glyph[4] = 0x63;
		} else if (symbol != ' ') {
			glyph[0] = 0x02; glyph[1] = 0x01; glyph[2] = 0x59;
			glyph[3] = 0x09; glyph[4] = 0x06;
		}
		for (int column = 0; column < 5; ++column) {
			for (int row = 0; row < 7; ++row) {
				if (glyph[column] & (1 << row)) {
					rect(left + (index * 6 + column) * scale, top + row * scale,
						 scale, scale, color);
				}
			}
		}
	}
}

static void line(int x_start, int y_start, int x_end, int y_end, uint16_t color)
{
	int delta_x = abs(x_end - x_start);
	int step_x = x_start < x_end ? 1 : -1;
	int delta_y = -abs(y_end - y_start);
	int step_y = y_start < y_end ? 1 : -1;
	int error = delta_x + delta_y;
	for (;;) {
		rect(x_start, y_start, 1, 1, color);
		if (x_start == x_end && y_start == y_end) break;
		int doubled = 2 * error;
		if (doubled >= delta_y) { error += delta_y; x_start += step_x; }
		if (doubled <= delta_x) { error += delta_x; y_start += step_y; }
	}
}

static void disc(int center_x, int center_y, int radius, uint16_t color)
{
	for (int row = -radius; row <= radius; ++row) {
		for (int column = -radius; column <= radius; ++column) {
			if (row * row + column * column <= radius * radius) {
				rect(center_x + column, center_y + row, 1, 1, color);
			}
		}
	}
}

static void lcd_command(uint8_t command, const uint8_t *data, size_t length)
{
	gpio_set_level(LCD_DC, 0);
	spi_transaction_t transaction = {.length = 8, .tx_buffer = &command};
	ESP_ERROR_CHECK(spi_device_polling_transmit(display, &transaction));
	if (length) {
		gpio_set_level(LCD_DC, 1);
		transaction.length = length * 8;
		transaction.tx_buffer = data;
		ESP_ERROR_CHECK(spi_device_polling_transmit(display, &transaction));
	}
}

static void display_init(void)
{
	gpio_set_direction(LCD_DC, GPIO_MODE_OUTPUT);
	gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
	gpio_set_level(LCD_BL, 0);
	spi_bus_config_t bus = {
		.mosi_io_num = 13, .miso_io_num = 12, .sclk_io_num = 14,
		.quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = WIDTH * 16 * 2
	};
	ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
	spi_device_interface_config_t config = {
		.clock_speed_hz = 40000000, .spics_io_num = 15, .queue_size = 1
	};
	ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &config, &display));
	lcd_command(0x01, NULL, 0);
	vTaskDelay(pdMS_TO_TICKS(150));
	const uint8_t power[] = {0x23};
	const uint8_t power2[] = {0x10};
	const uint8_t vcom[] = {0x3e, 0x28};
	const uint8_t orientation[] = {0x28};
	const uint8_t format[] = {0x55};
	const uint8_t rate[] = {0x00, 0x18};
	lcd_command(0xc0, power, sizeof(power));
	lcd_command(0xc1, power2, sizeof(power2));
	lcd_command(0xc5, vcom, sizeof(vcom));
	lcd_command(0x36, orientation, sizeof(orientation));
	lcd_command(0x3a, format, sizeof(format));
	lcd_command(0xb1, rate, sizeof(rate));
	lcd_command(0x11, NULL, 0);
	vTaskDelay(pdMS_TO_TICKS(120));
	lcd_command(0x29, NULL, 0);
	frame = heap_caps_malloc(WIDTH * HEIGHT, MALLOC_CAP_8BIT);
	stripe = heap_caps_malloc(WIDTH * 16 * 2, MALLOC_CAP_DMA);
	ESP_ERROR_CHECK(frame && stripe ? ESP_OK : ESP_ERR_NO_MEM);
	gpio_set_level(LCD_BL, 1);
}

static void display_flush(void)
{
	const uint8_t columns[] = {0, 0, 1, 63};
	const uint8_t rows[] = {0, 0, 0, 239};
	lcd_command(0x2a, columns, sizeof(columns));
	lcd_command(0x2b, rows, sizeof(rows));
	lcd_command(0x2c, NULL, 0);
	gpio_set_level(LCD_DC, 1);
	for (int row = 0; row < HEIGHT; row += 16) {
		for (int index = 0; index < WIDTH * 16; ++index) {
			uint16_t color = palette[frame[row * WIDTH + index]];
			stripe[index] = (color >> 8) | (color << 8);
		}
		spi_transaction_t transaction = {.length = WIDTH * 16 * 16, .tx_buffer = stripe};
		ESP_ERROR_CHECK(spi_device_polling_transmit(display, &transaction));
	}
}

static void inputs_init(void)
{
	gpio_config_t outputs = {
		.pin_bit_mask = (1ULL << TOUCH_CS) | (1ULL << TOUCH_CLK) | (1ULL << TOUCH_MOSI),
		.mode = GPIO_MODE_OUTPUT
	};
	ESP_ERROR_CHECK(gpio_config(&outputs));
	gpio_set_level(TOUCH_CS, 1);
	gpio_config_t inputs = {
		.pin_bit_mask = (1ULL << TOUCH_MISO) | (1ULL << TOUCH_IRQ),
		.mode = GPIO_MODE_INPUT
	};
	ESP_ERROR_CHECK(gpio_config(&inputs));
}

// Without a microSD card the SD SPI host is idle, so touch moves onto it. Wokwi's
// XPT2046 model only answers the SPI peripheral, not bit-banged GPIO.
static void touch_use_spi_host(void)
{
	esp_err_t result = spi_bus_free(SPI3_HOST);
	if (result != ESP_OK) {
		ESP_LOGW(TAG, "Touch keeps software SPI: %s", esp_err_to_name(result));
		return;
	}
	spi_bus_config_t bus = {
		.mosi_io_num = TOUCH_MOSI, .miso_io_num = TOUCH_MISO, .sclk_io_num = TOUCH_CLK,
		.quadwp_io_num = -1, .quadhd_io_num = -1
	};
	ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED));
	spi_device_interface_config_t device = {
		.clock_speed_hz = 2000000, .spics_io_num = TOUCH_CS, .queue_size = 1
	};
	ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &device, &touch_spi));
	ESP_LOGI(TAG, "Touch uses the SPI3 host");
}

static unsigned touch_read(uint8_t command)
{
	if (touch_spi) {
		spi_transaction_t transaction = {
			.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA, .length = 24,
			.tx_data = {command}
		};
		ESP_ERROR_CHECK(spi_device_polling_transmit(touch_spi, &transaction));
		return ((transaction.rx_data[1] << 8 | transaction.rx_data[2]) >> 3) & 4095;
	}
	gpio_set_level(TOUCH_CS, 0);
	for (unsigned bit = 0; bit < 8; ++bit) {
		gpio_set_level(TOUCH_MOSI, (command >> (7 - bit)) & 1);
		gpio_set_level(TOUCH_CLK, 1);
		esp_rom_delay_us(2);
		gpio_set_level(TOUCH_CLK, 0);
		esp_rom_delay_us(2);
	}
	unsigned result = 0;
	for (unsigned bit = 0; bit < 16; ++bit) {
		// MISO changes on the falling edge; sample it just before the rising edge.
		result = (result << 1) | gpio_get_level(TOUCH_MISO);
		gpio_set_level(TOUCH_CLK, 1);
		esp_rom_delay_us(2);
		gpio_set_level(TOUCH_CLK, 0);
		esp_rom_delay_us(2);
	}
	gpio_set_level(TOUCH_CS, 1);
	return (result >> 3) & 4095;
}

typedef struct {
	int irq;
	unsigned z1;
	unsigned z2;
	unsigned x;
	unsigned y;
} touch_inputs_t;

static touch_inputs_t touch_inputs(void)
{
	return (touch_inputs_t){
		.irq = gpio_get_level(TOUCH_IRQ), .z1 = touch_read(0xb0), .z2 = touch_read(0xc0),
		.x = touch_read(0xd0), .y = touch_read(0x90)
	};
}

// A press is PENIRQ low or enough pressure, so it works whichever of the two
// the Wokwi model drives.
static bool touch_inputs_pressed(const touch_inputs_t *inputs)
{
	bool pressed = !inputs->irq || inputs->z1 + 4095 - inputs->z2 >= TOUCH_PRESSURE;
	return pressed && inputs->x > 0 && inputs->x < 4095 && inputs->y > 0 && inputs->y < 4095;
}

static bool touch_raw(unsigned *raw_x, unsigned *raw_y)
{
	touch_inputs_t inputs = touch_inputs();
	*raw_x = inputs.x;
	*raw_y = inputs.y;
	return touch_inputs_pressed(&inputs);
}

static bool touch_pressed(void)
{
	unsigned raw_x;
	unsigned raw_y;
	return touch_raw(&raw_x, &raw_y);
}

static int clamp(float value, int limit)
{
	if (value < 0) return 0;
	return value > limit - 1 ? limit - 1 : (int)value;
}

static bool touch_position(int *screen_x, int *screen_y)
{
	unsigned raw_x;
	unsigned raw_y;
	if (!touch_raw(&raw_x, &raw_y)) return false;
	*screen_x = clamp(calibration.x[0] * raw_x + calibration.x[1] * raw_y + calibration.x[2], WIDTH);
	*screen_y = clamp(calibration.y[0] * raw_x + calibration.y[1] * raw_y + calibration.y[2], HEIGHT);
	return true;
}

static void wait_touch_release(void)
{
	while (touch_pressed()) vTaskDelay(pdMS_TO_TICKS(10));
	vTaskDelay(pdMS_TO_TICKS(50));
}

// Averages the raw readings of the current press until release.
static bool touch_sample(unsigned *raw_x, unsigned *raw_y)
{
	vTaskDelay(pdMS_TO_TICKS(30));
	unsigned count = 0;
	unsigned sum_x = 0;
	unsigned sum_y = 0;
	while (count < 16 && touch_raw(raw_x, raw_y)) {
		sum_x += *raw_x;
		sum_y += *raw_y;
		++count;
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	wait_touch_release();
	if (count < 2) return false;
	*raw_x = sum_x / count;
	*raw_y = sum_y / count;
	return true;
}

// Solves the affine map from three raw points to their screen targets (Cramer's rule).
static bool solve_calibration(const float raw[3][2], touch_calibration_t *result)
{
	float determinant = raw[0][0] * (raw[1][1] - raw[2][1]) - raw[0][1] * (raw[1][0] - raw[2][0])
						+ (raw[1][0] * raw[2][1] - raw[2][0] * raw[1][1]);
	if (fabsf(determinant) < 10000) return false;
	for (unsigned axis = 0; axis < 2; ++axis) {
		float s0 = calibration_targets[0][axis];
		float s1 = calibration_targets[1][axis];
		float s2 = calibration_targets[2][axis];
		float *out = axis ? result->y : result->x;
		out[0] = (s0 * (raw[1][1] - raw[2][1]) - raw[0][1] * (s1 - s2)
				  + (s1 * raw[2][1] - s2 * raw[1][1])) / determinant;
		out[1] = (raw[0][0] * (s1 - s2) - s0 * (raw[1][0] - raw[2][0])
				  + (raw[1][0] * s2 - raw[2][0] * s1)) / determinant;
		out[2] = (raw[0][0] * (raw[1][1] * s2 - raw[2][1] * s1)
				  - raw[0][1] * (raw[1][0] * s2 - raw[2][0] * s1)
				  + s0 * (raw[1][0] * raw[2][1] - raw[2][0] * raw[1][1])) / determinant;
	}
	return true;
}

static bool calibration_valid(const touch_calibration_t *candidate)
{
	for (unsigned index = 0; index < 3; ++index) {
		if (!isfinite(candidate->x[index]) || !isfinite(candidate->y[index])) return false;
	}
	return fabsf(candidate->x[0] * candidate->y[1] - candidate->x[1] * candidate->y[0]) > 1e-6f;
}

static bool read_coefficients(const cJSON *root, const char *key, float values[3])
{
	const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, key);
	if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != 3) return false;
	for (unsigned index = 0; index < 3; ++index) {
		const cJSON *item = cJSON_GetArrayItem(array, index);
		if (!cJSON_IsNumber(item)) return false;
		values[index] = item->valuedouble;
	}
	return true;
}

static bool load_calibration(void)
{
	FILE *file = fopen(CALIBRATION_FILE, "r");
	if (!file) return false;
	char json[512];
	size_t length = fread(json, 1, sizeof(json) - 1, file);
	fclose(file);
	json[length] = 0;
	cJSON *root = cJSON_Parse(json);
	touch_calibration_t loaded;
	bool ok = root && read_coefficients(root, "x", loaded.x) && read_coefficients(root, "y", loaded.y)
			  && calibration_valid(&loaded);
	cJSON_Delete(root);
	if (!ok) {
		ESP_LOGW(TAG, "Ignoring invalid %s", CALIBRATION_FILE);
		return false;
	}
	calibration = loaded;
	ESP_LOGI(TAG, "Loaded %s", CALIBRATION_FILE);
	return true;
}

static bool save_calibration(const float raw[3][2])
{
	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "version", 1);
	cJSON_AddNumberToObject(root, "width", WIDTH);
	cJSON_AddNumberToObject(root, "height", HEIGHT);
	cJSON_AddItemToObject(root, "x", cJSON_CreateFloatArray(calibration.x, 3));
	cJSON_AddItemToObject(root, "y", cJSON_CreateFloatArray(calibration.y, 3));
	cJSON *points = cJSON_AddArrayToObject(root, "points");
	for (unsigned index = 0; index < 3; ++index) {
		cJSON *point = cJSON_CreateObject();
		cJSON_AddNumberToObject(point, "screen_x", calibration_targets[index][0]);
		cJSON_AddNumberToObject(point, "screen_y", calibration_targets[index][1]);
		cJSON_AddNumberToObject(point, "raw_x", raw[index][0]);
		cJSON_AddNumberToObject(point, "raw_y", raw[index][1]);
		cJSON_AddItemToArray(points, point);
	}
	char *json = cJSON_Print(root);
	cJSON_Delete(root);
	FILE *file = json ? fopen(CALIBRATION_FILE, "w") : NULL;
	bool ok = file && fputs(json, file) >= 0;
	if (file) ok = fclose(file) == 0 && ok;
	cJSON_free(json);
	if (!ok) ESP_LOGW(TAG, "Could not write %s", CALIBRATION_FILE);
	return ok;
}

static bool mount_sd(void)
{
	if (sd_mounted) return true;
	if (touch_spi) return false;
	sdmmc_host_t host = SDSPI_HOST_DEFAULT();
	host.slot = SPI3_HOST;
	host.max_freq_khz = 10000;
	sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
	slot.host_id = SPI3_HOST;
	slot.gpio_cs = GPIO_NUM_5;
	esp_vfs_fat_mount_config_t mount = {
		.format_if_mount_failed = false, .max_files = 4, .allocation_unit_size = 16384
	};
	esp_err_t result = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &sd_card);
	if (result != ESP_OK) {
		ESP_LOGW(TAG, "SD mount: %s", esp_err_to_name(result));
		return false;
	}
	sd_mounted = true;
	return true;
}

static bool mount_flash(void)
{
	if (flash_mounted) return true;
	esp_vfs_fat_mount_config_t mount = {.format_if_mount_failed = false, .max_files = 4};
	esp_err_t result = esp_vfs_fat_spiflash_mount_ro("/flash", "storage", &mount);
	if (result != ESP_OK) {
		ESP_LOGW(TAG, "Flash mount: %s", esp_err_to_name(result));
		return false;
	}
	flash_mounted = true;
	return true;
}

static int compare_names(const void *left, const void *right)
{
	return strcasecmp(left, right);
}

static void scan_files(void)
{
	file_count = 0;
	page = 0;
	selected = 0;
	truncated_list = false;
	if (mount_sd()) library_root = "/sdcard";
	else if (mount_flash()) library_root = "/flash";
	else return;
	DIR *directory = opendir(library_root);
	if (!directory) {
		if (sd_mounted) {
			esp_vfs_fat_sdcard_unmount("/sdcard", sd_card);
			sd_mounted = false;
		}
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(directory))) {
		size_t length = strlen(entry->d_name);
		if (length < 5 || length >= NAME_SIZE || strcasecmp(entry->d_name + length - 4, ".tap")) {
			continue;
		}
		char path[NAME_SIZE + 9];
		snprintf(path, sizeof(path), "%s/%s", library_root, entry->d_name);
		struct stat info;
		if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) continue;
		if (file_count == MAX_FILES) {
			truncated_list = true;
			break;
		}
		memcpy(files[file_count++], entry->d_name, length + 1);
	}
	closedir(directory);
	qsort(files, file_count, sizeof(files[0]), compare_names);
}

typedef struct {
	dac_continuous_handle_t dac;
	uint8_t samples[AUDIO_BUFFER];
	size_t used;
	uint64_t remainder;
	bool high;
	bool failed;
} audio_t;

static bool audio_flush(audio_t *audio)
{
	if (!audio->used) return true;
	size_t loaded = 0;
	esp_err_t result = dac_continuous_write(audio->dac, audio->samples, audio->used, &loaded, 1000);
	if (result != ESP_OK || loaded != audio->used) {
		audio->failed = true;
		return false;
	}
	audio->used = 0;
	return true;
}

static bool audio_pulse(uint32_t tstates, bool silence, void *context)
{
	audio_t *audio = context;
	if (atomic_load(&cancel_playback)) return false;
	audio->remainder += (uint64_t)tstates * SAMPLE_RATE;
	unsigned count = audio->remainder / 3500000;
	audio->remainder %= 3500000;
	uint8_t value = silence ? 128 : (audio->high ? 208 : 48);
	while (count) {
		size_t available = AUDIO_BUFFER - audio->used;
		size_t chunk = count < available ? count : available;
		memset(audio->samples + audio->used, value, chunk);
		audio->used += chunk;
		count -= chunk;
		if (audio->used == AUDIO_BUFFER && !audio_flush(audio)) return false;
		if (atomic_load(&cancel_playback)) return false;
	}
	audio->high = silence ? false : !audio->high;
	return true;
}

static void playback_progress(long position, long total, unsigned block, void *context)
{
	(void)context;
	portENTER_CRITICAL(&state_lock);
	player.percent = total ? (uint64_t)position * 100 / total : 0;
	player.block = block;
	portEXIT_CRITICAL(&state_lock);
}

static void playback_task(void *argument)
{
	(void)argument;
	char path[NAME_SIZE + 9];
	snprintf(path, sizeof(path), "%s/%s", library_root, files[selected]);
	FILE *file = fopen(path, "rb");
	audio_t *audio = calloc(1, sizeof(*audio));
	tap_result_t result = file ? TAP_NO_MEMORY : TAP_IO_ERROR;
	bool enabled = false;
	bool audio_error = false;
	if (file && audio) {
		dac_continuous_config_t config = {
			.chan_mask = DAC_CHANNEL_MASK_CH1, .desc_num = 8, .buf_size = 2048,
			.freq_hz = SAMPLE_RATE, .offset = 0,
			.clk_src = DAC_DIGI_CLK_SRC_DEFAULT, .chan_mode = DAC_CHANNEL_MODE_SIMUL
		};
		esp_err_t error = dac_continuous_new_channels(&config, &audio->dac);
		if (error == ESP_OK) {
			error = dac_continuous_enable(audio->dac);
			enabled = error == ESP_OK;
		}
		if (enabled) {
			result = tap_play(file, audio_pulse, playback_progress, audio);
			if (!audio->failed && !atomic_load(&cancel_playback)) {
				memset(audio->samples + audio->used, 128, AUDIO_BUFFER - audio->used);
				audio->used = AUDIO_BUFFER;
				audio_flush(audio);
				vTaskDelay(pdMS_TO_TICKS(50));
			}
			audio_error = audio->failed;
		} else {
			ESP_LOGE(TAG, "DAC setup: %s", esp_err_to_name(error));
			audio_error = true;
		}
	}
	if (file) fclose(file);
	if (audio) {
		if (enabled) dac_continuous_disable(audio->dac);
		if (audio->dac) dac_continuous_del_channels(audio->dac);
		free(audio);
	}
	portENTER_CRITICAL(&state_lock);
	player.playing = false;
	snprintf(player.status, sizeof(player.status), "%s",
			 audio_error ? "AUDIO OUTPUT ERROR" : tap_result_text(result));
	portEXIT_CRITICAL(&state_lock);
	vTaskDelete(NULL);
}

static player_state_t player_snapshot(void)
{
	portENTER_CRITICAL(&state_lock);
	player_state_t state = player;
	portEXIT_CRITICAL(&state_lock);
	return state;
}

static void start_playback(void)
{
	if (!file_count || player_snapshot().playing) return;
	atomic_store(&cancel_playback, false);
	cassette_view = true;
	portENTER_CRITICAL(&state_lock);
	player = (player_state_t){.playing = true, .status = "PLAYING"};
	portEXIT_CRITICAL(&state_lock);
	if (xTaskCreatePinnedToCore(playback_task, "tap_audio", 6144, NULL, 5, NULL, 0) != pdPASS) {
		portENTER_CRITICAL(&state_lock);
		player.playing = false;
		strcpy(player.status, "NOT ENOUGH MEMORY");
		portEXIT_CRITICAL(&state_lock);
	}
}

static void arrow(int left, int top, bool right, uint16_t color)
{
	for (int offset = 0; offset < 9; ++offset) {
		rect(left + (right ? offset : 8 - offset), top + offset, 1, 17 - 2 * offset, color);
	}
}

static void render_browser(void)
{
	char label[48];
	rect(0, 0, WIDTH, HEIGHT, PAPER);
	rect(0, 0, WIDTH, 43, INK);
	text(12, 9, "TAPE LIBRARY", 2, WHITE, 24);
	text(13, 30, "ZX SPECTRUM", 1, GREEN, 25);
	text(249, 30, sd_mounted ? "SD / TAP" : "FLASH / TAP", 1, WHITE, 11);
	if (!file_count) {
		text(24, 83, sd_mounted ? "NO TAP FILES" : "SD NOT READY", 2, INK, 23);
		text(24, 116, sd_mounted ? "ROOT / *.TAP" : "FAT32 MICROSD", 1, INK, 40);
	}
	for (unsigned row = 0; row < PAGE_ROWS; ++row) {
		unsigned index = page * PAGE_ROWS + row;
		if (index >= file_count) break;
		int top = 50 + row * 29;
		bool active = selected == index;
		rect(8, top, 304, 26, active ? INK : WHITE);
		text(15, top + 9, "TAP", 1, active ? GREEN : INK, 3);
		size_t length = strlen(files[index]);
		size_t name_length = length - 4;
		char name[38];
		size_t visible = name_length < 36 ? name_length : 36;
		memcpy(name, files[index], visible);
		name[visible] = 0;
		if (name_length > 36) memcpy(name + 33, "...", 3);
		text(45, top + 9, name, 1, active ? WHITE : INK, 36);
		arrow(293, top + 5, true, active ? GREEN : MUTED);
	}
	rect(0, 202, WIDTH, 38, INK);
	arrow(17, 213, false, WHITE);
	arrow(291, 213, true, WHITE);
	snprintf(label, sizeof(label), "%u/%u", page + 1,
			 file_count ? (file_count + PAGE_ROWS - 1) / PAGE_ROWS : 1);
	text(49, 217, label, 1, WHITE, 8);
	text(127, 217, "RESCAN", 1, GREEN, 10);
	if (truncated_list) text(211, 217, "96 MAX", 1, YELLOW, 8);
}

static void reel(int center_x, unsigned phase)
{
	static const int offsets[8][2] = {
		{0,-14}, {10,-10}, {14,0}, {10,10}, {0,14}, {-10,10}, {-14,0}, {-10,-10}
	};
	disc(center_x, 119, 26, INK);
	disc(center_x, 119, 18, PAPER);
	for (unsigned spoke = 0; spoke < 4; ++spoke) {
		unsigned index = (phase + spoke * 2) % 8;
		line(center_x, 119, center_x + offsets[index][0], 119 + offsets[index][1], INK);
	}
	disc(center_x, 119, 6, INK);
	disc(center_x, 119, 2, MUTED);
}

static void render_cassette(unsigned animation)
{
	player_state_t state = player_snapshot();
	char label[48];
	rect(0, 0, WIDTH, HEIGHT, PAPER);
	text(14, 11, "ZX SPECTRUM", 1, INK, 22);
	text(227, 11, "TAPE / 48K", 1, INK, 15);
	rect(15, 32, 290, 146, INK);
	rect(20, 37, 280, 136, MUTED);
	rect(29, 44, 262, 44, PAPER);
	rect(29, 44, 5, 44, RED);
	size_t length = strlen(files[selected]);
	if (length <= 40) text(41, 58, files[selected], 1, INK, 40);
	else {
		unsigned offset = state.playing ? (animation / 4) % (length + 8) : 0;
		char scrolling[41] = {0};
		for (unsigned index = 0; index < 40; ++index) {
			unsigned position = (offset + index) % (length + 8);
			scrolling[index] = position < length ? files[selected][position] : ' ';
		}
		text(41, 58, scrolling, 1, INK, 40);
	}
	text(41, 74, "A", 1, INK, 1);
	text(215, 74, "NORMAL BIAS", 1, INK, 12);
	rect(49, 93, 222, 53, INK);
	rect(115, 103, 90, 30, PAPER);
	unsigned phase = state.playing ? animation % 8 : 0;
	reel(88, phase);
	reel(232, phase);
	rect(136, 108, 48, 20, INK);
	text(144, 115, "48K", 1, WHITE, 3);
	line(91, 157, 229, 157, INK);
	line(91, 157, 79, 172, INK);
	line(229, 157, 241, 172, INK);
	disc(37, 157, 3, INK);
	disc(283, 157, 3, INK);
	rect(17, 184, 286, 3, MUTED);
	rect(17, 184, 286 * state.percent / 100, 3, GREEN);
	snprintf(label, sizeof(label), "%s", state.status);
	text(17, 194, label, 1, state.playing ? INK : RED, 31);
	snprintf(label, sizeof(label), "%u%% B%u", state.percent, state.block);
	text(235, 194, label, 1, INK, 13);
	rect(0, 211, WIDTH, 29, INK);
	arrow(22, 217, false, WHITE);
	text(43, 221, "FILES", 1, WHITE, 5);
	if (state.playing) rect(183, 219, 12, 12, RED);
	else arrow(184, 216, true, GREEN);
	text(210, 221, state.playing ? "STOP" : "REPLAY", 1, WHITE, 8);
}

static void handle_tap(int screen_x, int screen_y)
{
	if (cassette_view) {
		player_state_t state = player_snapshot();
		if (screen_y >= 208 && screen_x < 140) {
			atomic_store(&cancel_playback, true);
			if (!state.playing) cassette_view = false;
		} else if (screen_y >= 208 && screen_x >= 140) {
			if (state.playing) atomic_store(&cancel_playback, true);
			else start_playback();
		}
		return;
	}
	if (screen_y >= 202) {
		if (screen_x < 95 && page) --page;
		else if (screen_x > 240 && (page + 1) * PAGE_ROWS < file_count) ++page;
		else if (screen_x >= 95 && screen_x <= 240) scan_files();
		selected = page * PAGE_ROWS;
	} else if (screen_y >= 50 && screen_y < 195) {
		unsigned index = page * PAGE_ROWS + (screen_y - 50) / 29;
		if (index < file_count) {
			selected = index;
			start_playback();
		}
	}
}

static void render_calibration(unsigned step, const char *message, uint16_t color)
{
	char label[16];
	rect(0, 0, WIDTH, HEIGHT, PAPER);
	text(94, 84, "TOUCH SETUP", 2, INK, 11);
	text((WIDTH - (int)strlen(message) * 6) / 2, 110, message, 1, color, 30);
	if (step >= 3) return;
	snprintf(label, sizeof(label), "POINT %u/3", step + 1);
	text(130, 128, label, 1, MUTED, 10);
	int x = calibration_targets[step][0];
	int y = calibration_targets[step][1];
	rect(x - 12, y, 25, 1, RED);
	rect(x, y - 12, 1, 25, RED);
	rect(x - 2, y - 2, 5, 5, INK);
}

// Shows the controller readings so touch problems can be diagnosed on screen.
static void render_touch_inputs(const touch_inputs_t *inputs)
{
	char label[48];
	snprintf(label, sizeof(label), "IRQ %d Z1 %u Z2 %u X %u Y %u",
			 inputs->irq, inputs->z1, inputs->z2, inputs->x, inputs->y);
	rect(0, 146, WIDTH, 15, PAPER);
	text((WIDTH - (int)strlen(label) * 6) / 2, 150, label, 1, MUTED, 40);
	ESP_LOGI(TAG, "Touch: %s", label);
}

static void calibrate_touch(void)
{
	float raw[3][2];
	wait_touch_release();
	for (unsigned step = 0; step < 3;) {
		render_calibration(step, "TAP THE CENTRE OF THE CROSS", INK);
		// Poll without redrawing so short presses are not missed; redraw only on change.
		touch_inputs_t shown = {.irq = -1};
		for (;;) {
			touch_inputs_t inputs = touch_inputs();
			if (memcmp(&inputs, &shown, sizeof(inputs))) {
				render_touch_inputs(&inputs);
				display_flush();
				shown = inputs;
			}
			if (touch_inputs_pressed(&inputs)) break;
			vTaskDelay(pdMS_TO_TICKS(10));
		}
		unsigned raw_x;
		unsigned raw_y;
		if (!touch_sample(&raw_x, &raw_y)) {
			ESP_LOGW(TAG, "Touch released too quickly");
			continue;
		}
		raw[step][0] = raw_x;
		raw[step][1] = raw_y;
		ESP_LOGI(TAG, "Calibration point %u: raw %u, %u", step + 1, raw_x, raw_y);
		if (++step < 3) continue;
		touch_calibration_t solved;
		if (!solve_calibration(raw, &solved) || !calibration_valid(&solved)) {
			render_calibration(3, "POINTS TOO CLOSE - TRY AGAIN", RED);
			display_flush();
			vTaskDelay(pdMS_TO_TICKS(1500));
			step = 0;
			continue;
		}
		calibration = solved;
	}
	bool saved = sd_mounted && save_calibration(raw);
	render_calibration(3, saved ? "SAVED TO SD CARD" : "NOT SAVED - NO SD CARD",
					   saved ? GREEN : RED);
	display_flush();
	vTaskDelay(pdMS_TO_TICKS(1500));
}

static void ui_task(void *argument)
{
	(void)argument;
	bool touch_held = false;
	bool return_to_files = false;
	unsigned animation = 0;
	TickType_t last_draw = 0;
	for (;;) {
		int screen_x = 0;
		int screen_y = 0;
		bool touched = touch_position(&screen_x, &screen_y);
		if (touched && !touch_held) {
			if (cassette_view && screen_y >= 208 && screen_x < 140) return_to_files = true;
			handle_tap(screen_x, screen_y);
		}
		if (return_to_files && !player_snapshot().playing) {
			cassette_view = false;
			return_to_files = false;
		}
		touch_held = touched;
		TickType_t now = xTaskGetTickCount();
		if (now - last_draw >= pdMS_TO_TICKS(125) || !last_draw) {
			if (cassette_view) render_cassette(animation++);
			else render_browser();
			display_flush();
			last_draw = now;
		}
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}

void app_main(void)
{
	inputs_init();
	display_init();
	rect(0, 0, WIDTH, HEIGHT, PAPER);
	display_flush();
	spi_bus_config_t sd_bus = {
		.mosi_io_num = 23, .miso_io_num = 19, .sclk_io_num = 18,
		.quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096
	};
	ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &sd_bus, SPI_DMA_CH_AUTO));
	// Read the SD card first; calibrate only when it has no calibration file.
	scan_files();
	if (!sd_mounted) touch_use_spi_host();
	if (!sd_mounted || !load_calibration()) calibrate_touch();
	ESP_ERROR_CHECK(xTaskCreatePinnedToCore(ui_task, "tape_ui", 6144, NULL, 3, NULL, 1) == pdPASS
					? ESP_OK : ESP_ERR_NO_MEM);
}
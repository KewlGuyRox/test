// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * YSPEED YS-GPS-12832 userspace ST7789 dashboard.
 *
 * Hardware facts this file depends on (verified from the vendor DTB, the
 * captured /proc snapshots and the vendor LVGL binary):
 *
 *   SPI      : hardware MT7621 SPI, CS1 held asserted, 24 MHz -> spidev0.1
 *   LCD D/C  : GPIO16   (SoC pin offset, not a sysfs number)
 *   LCD RESET: GPIO14
 *   BACKLIGHT: GPIO19
 *   Panel    : 240x320, ST7789, mounted at rotation 270 in the enclosure
 *
 * GPIO offsets are converted to sysfs numbers at runtime by reading
 * /sys/class/gpio/gpiochipN/base. The vendor kernel used base 480 (so pin 16
 * was 496); mainline 6.12 uses base 0 (pin 16 is 16). Hardcoding either one
 * breaks the other, which is exactly why the vendor binary cannot run here.
 *
 * Keys are owned by the kernel via gpio-keys (GPIO18/23/26) and read through
 * evdev. This program must never export those pins - doing so fails with
 * EBUSY and kills the buttons.
 */
#define _DEFAULT_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <limits.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

/* Printed by every mode so a running image can be identified from logread. */
#define YSPEED_BUILD "pin15-24m"
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define PANEL_W 240
#define PANEL_H 320

/*
 * The vendor requests 24 MHz, and its live MASTER register (0xE0078884,
 * clk_sel 7 at sys_freq 193.333 MHz) works out to 21.48 MHz actual. That is
 * the only rate this panel has ever been driven at successfully; 40 MHz asks
 * for nearly double it. 24 MHz lands on the same divider (rate 9, clk_sel 7).
 */
#define SPI_HZ 24000000
#define SPI_CHUNK 4096

#define GPIO_CS_OFFSET 6
#define GPIO_DC_OFFSET 16
#define GPIO_RESET_OFFSET 14
#define GPIO_BACKLIGHT_OFFSET 19
/*
 * The stock firmware exports this line too and holds it high for the whole
 * session; on the vendor box it reads back as gpio495, direction out, value 1,
 * and nothing in its userspace ever touches it again. Its own configuration
 * printout names only the four display pins above, so this is an enable of
 * some kind that the vendor sets and we never did. Drive it high before the
 * panel is reset, in case it powers the glass or releases the GNSS module.
 */
#define GPIO_POWER_OFFSET 15

#define CONFIG_PATH "/etc/lvgl_st7789.conf"
#define STATUS_BIN "/usr/sbin/yspeed-status"
#define STATUS_JSON_MAX 8192

static volatile sig_atomic_t g_running = 1;

/* Diagnostics for the last GPIO chip resolution, reported by --selftest. */
static char g_gpio_chip_label[128];
static const char *g_gpio_chip_rule = "none";
static int g_sat_count;
static int g_sat_offset;

struct display {
	int spi;
	int cs;
	int dc;
	int reset;
	int backlight;
	uint16_t pixels[PANEL_W * PANEL_H];
};

struct config {
	int rotation;      /* 0..3, matches the vendor lcd_rotation option */
	int sleep_seconds; /* 0 = never blank the backlight */
	int english;
};

/* 5x7 column-major font: A-Z, 0-9, space, colon, dot, hyphen. */
static const uint8_t font[40][5] = {
	{0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36},
	{0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c},
	{0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01},
	{0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f},
	{0x00, 0x41, 0x7f, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3f, 0x01},
	{0x7f, 0x08, 0x14, 0x22, 0x41}, {0x7f, 0x40, 0x40, 0x40, 0x40},
	{0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f},
	{0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06},
	{0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46},
	{0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01},
	{0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f},
	{0x3f, 0x40, 0x38, 0x40, 0x3f}, {0x63, 0x14, 0x08, 0x14, 0x63},
	{0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
	{0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00},
	{0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31},
	{0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
	{0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
	{0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1e},
	{0, 0, 0, 0, 0},                {0, 0, 0x14, 0, 0},
	{0, 0x60, 0x60, 0, 0},          {0x08, 0x08, 0x08, 0x08, 0x08}
};

static void on_signal(int sig)
{
	(void)sig;
	g_running = 0;
}

static void msleep(unsigned ms)
{
	while (ms > 0) {
		unsigned step = ms > 1000 ? 1000 : ms;
		struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)step * 1000000L };
		nanosleep(&ts, NULL);
		ms -= step;
	}
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
			   end[-1] == '\r' || end[-1] == '\n'))
		*--end = '\0';
	return s;
}

static int write_text(const char *path, const char *value)
{
	size_t len = strlen(value);
	size_t off = 0;
	int fd = open(path, O_WRONLY);

	if (fd < 0)
		return -1;
	while (off < len) {
		ssize_t n = write(fd, value + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		off += (size_t)n;
	}
	close(fd);
	return 0;
}

static int read_int_file(const char *path, int *out)
{
	FILE *f = fopen(path, "r");
	int value;

	if (!f)
		return -1;
	if (fscanf(f, "%d", &value) != 1) {
		fclose(f);
		return -1;
	}
	fclose(f);
	*out = value;
	return 0;
}

/*
 * Resolve one MT7628 SoC GPIO offset to its global legacy-sysfs number.
 * gpiochipN is a directory name, not a GPIO number. Match the exact
 * controller label and gpio-bankN suffix, then use base + bank-local line.
 * Fail closed on missing, duplicated, or inconsistent bank information.
 */
#define GPIO_BANK_WIDTH 32
#define GPIO_BANK_COUNT 3 /* MT7628 SoC exposes gpio-bank0 through gpio-bank2. */

static char g_gpio_sysfs_root[PATH_MAX] = "/sys/class/gpio";

static int g_gpio_chip_base = -1;
static int g_gpio_chip_bank = -1;
static int g_gpio_line_offset = -1;

static int gpio_chip_base_for(int offset)
{
	DIR *dir;
	struct dirent *entry;
	int matches = 0, chosen = -1;
	int expected_bank, line_offset;

	g_gpio_chip_label[0] = '\0';
	g_gpio_chip_rule = "none";
	g_gpio_chip_base = -1;
	g_gpio_chip_bank = -1;
	g_gpio_line_offset = -1;
	if (offset < 0)
		return -1;

	expected_bank = offset / GPIO_BANK_WIDTH;
	line_offset = offset % GPIO_BANK_WIDTH;
	dir = opendir(g_gpio_sysfs_root);
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		char path[256], label[128] = "";
		char *bank_text, *bank_end;
		long bank;
		int base = -1, ngpio = 0;
		FILE *f;

		if (strncmp(entry->d_name, "gpiochip", 8) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s/base", g_gpio_sysfs_root, entry->d_name);
		if (read_int_file(path, &base) < 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s/ngpio", g_gpio_sysfs_root, entry->d_name);
		if (read_int_file(path, &ngpio) < 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s/label", g_gpio_sysfs_root, entry->d_name);
		f = fopen(path, "r");
		if (f) {
			if (!fgets(label, sizeof(label), f))
				label[0] = '\0';
			fclose(f);
		}
		if (base < 0 || ngpio <= 0)
			continue;
		if (!strstr(label, "10000600") && !strstr(label, "mt7628") &&
		    !strstr(label, "mt7621"))
			continue;

		bank_text = strstr(label, "gpio-bank");
		if (!bank_text)
			continue;
		bank_text += strlen("gpio-bank");
		errno = 0;
		bank = strtol(bank_text, &bank_end, 10);
		if (errno || bank_end == bank_text ||
		    (*bank_end != '\0' && *bank_end != '\n' &&
		     *bank_end != '\r' && *bank_end != ' '))
			continue;
		if (bank != expected_bank || bank < 0 || bank >= GPIO_BANK_COUNT ||
		    line_offset >= ngpio)
			continue;

		matches++;
		chosen = base + line_offset;
		g_gpio_chip_base = base;
		g_gpio_chip_bank = (int)bank;
		g_gpio_line_offset = line_offset;
		snprintf(g_gpio_chip_label, sizeof(g_gpio_chip_label), "%s", label);
	}
	closedir(dir);

	if (matches != 1) {
		g_gpio_chip_rule = matches > 1 ? "duplicate SoC bank, refusing" :
			"expected SoC bank not found, refusing";
		g_gpio_chip_base = -1;
		g_gpio_chip_bank = -1;
		g_gpio_line_offset = -1;
		g_gpio_chip_label[0] = '\0';
		return -1;
	}
	g_gpio_chip_rule = "exact SoC bank match";
	return chosen;
}

static int gpio_lcd_offset_allowed(int offset)
{
	return offset == GPIO_CS_OFFSET || offset == GPIO_DC_OFFSET ||
	       offset == GPIO_RESET_OFFSET || offset == GPIO_BACKLIGHT_OFFSET ||
	       offset == GPIO_POWER_OFFSET;
}

static int gpio_claim(int offset, int *number, int initial)
{
	char path[160];
	char text[32];
	int resolved;

	/* Only the three dedicated LCD outputs may be exported by this process. */
	if (!gpio_lcd_offset_allowed(offset)) {
		fprintf(stderr, "gpio: refusing non-LCD SoC offset %d\n", offset);
		return -1;
	}
	resolved = gpio_chip_base_for(offset);

	if (resolved < 0) {
		fprintf(stderr, "gpio: no gpiochip covers offset %d "
			"(is CONFIG_GPIO_SYSFS enabled?)\n", offset);
		return -1;
	}
	/*
	 * gpio_chip_base_for() already returns the global GPIO number, having
	 * selected the bank that owns the pin. Adding the offset again drives a
	 * different line entirely: offset 19 became 531 + 19 = 550, which is the
	 * front-panel reset button. Never reintroduce "resolved + offset" here.
	 */
	*number = resolved;
	if (*number < 0)
		return -1;

	snprintf(path, sizeof(path), "%s/gpio%d", g_gpio_sysfs_root, *number);
	if (access(path, F_OK) != 0) {
		snprintf(text, sizeof(text), "%d", *number);
		snprintf(path, sizeof(path), "%s/export", g_gpio_sysfs_root);
		if (write_text(path, text) < 0) {
			fprintf(stderr, "gpio: export %d failed: %s\n",
				*number, strerror(errno));
			return -1;
		}
		msleep(25);
	}

	snprintf(path, sizeof(path), "%s/gpio%d/direction", g_gpio_sysfs_root, *number);
	if (write_text(path, "out") < 0) {
		fprintf(stderr, "gpio: direction for %d failed: %s\n",
			*number, strerror(errno));
		return -1;
	}

	snprintf(path, sizeof(path), "%s/gpio%d/value", g_gpio_sysfs_root, *number);
	snprintf(text, sizeof(text), "%d", initial);
	return write_text(path, text);
}

static int gpio_set(int number, int value)
{
	char path[160];
	char text[4];

	snprintf(path, sizeof(path), "%s/gpio%d/value", g_gpio_sysfs_root, number);
	snprintf(text, sizeof(text), "%d", value ? 1 : 0);
	return write_text(path, text);
}

static int spi_write_all(int fd, const uint8_t *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0) {
			errno = EIO;
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

/*
 * GPIO6 is the board's documented LCD SPI_CS line. Keep the controller's
 * automatic CS disabled and frame each ST7789 command/data transaction with
 * this GPIO. This keeps CS asserted while D/C changes from command to data.
 */
/* ---- panel-probe variants; defaults reproduce the daemon exactly ---- */
static int g_dc_invert;      /* 1: command=1, data=0 */
static int g_cs_hold;        /* 1: never deassert CS between commands */
/*
 * 0: SPI_NO_CS - the controller leaves native CS1 asserted, which is the state
 *    the vendor firmware runs in (its POLAR register reads 0 and never moves).
 * 1: native CS1 toggled per SPI message (--hw-cs), kept only for comparison.
 */
static int g_hw_cs;
static int g_skip_reset;     /* 1: no RESET pulse before init */
static int g_bl_fixed = -1;  /* >=0: drive GPIO19 to this level and leave it */
static int g_swap_dc_cs;     /* 1: pad 16 carries CS and pad 6 carries D/C */
static int g_reset_mode;     /* 0 normal, 2 inverted, 3 held low, 4 held high */

static uint32_t g_spi_hz = SPI_HZ;   /* current clock, used by flush() unless set */
static uint8_t g_spi_mode = SPI_MODE_0;
static int g_lsb_first;
static int g_dc_off = GPIO_DC_OFFSET;
static int g_rst_off = GPIO_RESET_OFFSET;
static int g_bl_off = GPIO_BACKLIGHT_OFFSET;
static int g_power_gpio = -1;

static int spi_set_speed(int fd, uint32_t hz)
{
	uint32_t actual = 0;

	if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0)
		return -1;
	if (ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &actual) == 0)
		printf("  speed        : requested %u Hz, driver reports %u Hz\n",
		       hz, actual);
	g_spi_hz = hz;
	return 0;
}

static int dc_write(struct display *d, int is_data)
{
	int line = g_swap_dc_cs ? d->cs : d->dc;

	return gpio_set(line, g_dc_invert ? !is_data : is_data);
}

static int cs_assert(struct display *d)
{
	/* CS1 is asserted by the controller (held) or per message (native). */
	(void)d;
	return 0;
}

static int cs_release(struct display *d)
{
	/* Never deassert here: the vendor's controller leaves CS1 low. */
	(void)d;
	return 0;
}

static int lcd_cmd(struct display *d, uint8_t cmd, const uint8_t *data, size_t len)
{
	int rc = -1;
	int saved_errno = 0;

	if (cs_assert(d) < 0)
		return -1;
	if (dc_write(d, 0) < 0 || spi_write_all(d->spi, &cmd, 1) < 0)
		goto out;
	if (len > 0 &&
	    (dc_write(d, 1) < 0 || spi_write_all(d->spi, data, len) < 0))
		goto out;
	rc = 0;
out:
	if (rc < 0)
		saved_errno = errno;
	if (cs_release(d) < 0 && rc == 0)
		return -1;
	if (rc < 0)
		errno = saved_errno;
	return rc;
}

/* ST7789 MADCTL for each rotation, with the BGR bit set as the vendor panel. */
static uint8_t madctl_for(int rotation)
{
	switch (rotation & 3) {
	case 1:
		return 0x68; /* MX | MV | BGR */
	case 2:
		return 0xC8; /* MX | MY | BGR */
	case 3:
		return 0xA8; /* MY | MV | BGR */
	default:
		return 0x08; /* BGR */
	}
}

static int panel_init(struct display *d, const struct config *cfg)
{
	static const uint8_t porch[] = { 0x0c, 0x0c, 0x00, 0x33, 0x33 };
	static const uint8_t gate[] = { 0x35 };
	static const uint8_t vcoms[] = { 0x19 };
	static const uint8_t lcm[] = { 0x2c };
	static const uint8_t vdvvrhen[] = { 0x01 };
	static const uint8_t vrhs[] = { 0x12 };
	static const uint8_t vdvs[] = { 0x20 };
	static const uint8_t frctrl[] = { 0x0f };
	static const uint8_t pwctrl[] = { 0xa4, 0xa1 };
	static const uint8_t gammap[] = { 0xd0, 0x04, 0x0d, 0x11, 0x13, 0x2b,
					  0x3f, 0x54, 0x4c, 0x18, 0x0d, 0x0b,
					  0x1f, 0x23 };
	static const uint8_t gaman[] = { 0xd0, 0x04, 0x0c, 0x11, 0x13, 0x2c,
					 0x3f, 0x44, 0x51, 0x2f, 0x1f, 0x1f,
					 0x20, 0x23 };
	const uint8_t colmod = 0x55;
	const uint8_t madctl = madctl_for(cfg->rotation);

	gpio_set(d->backlight, g_bl_fixed >= 0 ? g_bl_fixed : 0);
	switch (g_reset_mode) {
	case 2:                       /* inverted pulse: assert high, release low */
		gpio_set(d->reset, 1);
		msleep(30);
		gpio_set(d->reset, 0);
		break;
	case 3:                       /* held low for the whole init */
		gpio_set(d->reset, 0);
		break;
	case 4:                       /* held high for the whole init */
		gpio_set(d->reset, 1);
		break;
	default:
		if (!g_skip_reset) {
			gpio_set(d->reset, 0);
			msleep(30);
		}
		gpio_set(d->reset, 1);
		break;
	}
	msleep(150);

	if (lcd_cmd(d, 0x01, NULL, 0) < 0)  /* SWRESET */
		return -1;
	msleep(150);
	if (lcd_cmd(d, 0x11, NULL, 0) < 0)  /* SLPOUT */
		return -1;
	msleep(120);

	if (lcd_cmd(d, 0x3a, &colmod, 1) < 0 ||        /* COLMOD 16bpp */
	    lcd_cmd(d, 0x36, &madctl, 1) < 0 ||        /* MADCTL rotation */
	    lcd_cmd(d, 0xb2, porch, sizeof(porch)) < 0 ||   /* PORCTRL */
	    lcd_cmd(d, 0xb7, gate, sizeof(gate)) < 0 ||     /* GCTRL */
	    lcd_cmd(d, 0xbb, vcoms, sizeof(vcoms)) < 0 ||   /* VCOMS */
	    lcd_cmd(d, 0xc0, lcm, sizeof(lcm)) < 0 ||       /* LCMCTRL */
	    lcd_cmd(d, 0xc2, vdvvrhen, sizeof(vdvvrhen)) < 0 ||
	    lcd_cmd(d, 0xc3, vrhs, sizeof(vrhs)) < 0 ||     /* VRHS */
	    lcd_cmd(d, 0xc4, vdvs, sizeof(vdvs)) < 0 ||     /* VDVS */
	    lcd_cmd(d, 0xc6, frctrl, sizeof(frctrl)) < 0 || /* FRCTRL2 */
	    lcd_cmd(d, 0xd0, pwctrl, sizeof(pwctrl)) < 0 || /* PWCTRL1 */
	    lcd_cmd(d, 0xe0, gammap, sizeof(gammap)) < 0 || /* PVGAMCTRL */
	    lcd_cmd(d, 0xe1, gaman, sizeof(gaman)) < 0)     /* NVGAMCTRL */
		return -1;

	if (lcd_cmd(d, 0x21, NULL, 0) < 0 ||  /* INVON */
	    lcd_cmd(d, 0x13, NULL, 0) < 0 ||  /* NORON */
	    lcd_cmd(d, 0x29, NULL, 0) < 0)    /* DISPON */
		return -1;

	msleep(100);
	return gpio_set(d->backlight, g_bl_fixed >= 0 ? g_bl_fixed : 1);
}

static void pixel(struct display *d, int x, int y, uint16_t color)
{
	if (x >= 0 && x < PANEL_W && y >= 0 && y < PANEL_H)
		d->pixels[y * PANEL_W + x] = color;
}

static void rect(struct display *d, int x, int y, int w, int h, uint16_t color)
{
	for (int yy = y; yy < y + h; yy++)
		for (int xx = x; xx < x + w; xx++)
			pixel(d, xx, yy, color);
}

static void fill(struct display *d, uint16_t color)
{
	for (size_t i = 0; i < PANEL_W * PANEL_H; i++)
		d->pixels[i] = color;
}

static int glyph(char c)
{
	if (c >= 'a' && c <= 'z')
		c = (char)(c - 'a' + 'A');
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= '0' && c <= '9')
		return 26 + c - '0';
	if (c == ':')
		return 37;
	if (c == '.')
		return 38;
	if (c == '-')
		return 39;
	return 36;
}

static void draw_text(struct display *d, int x, int y, const char *s,
		      int scale, uint16_t color)
{
	for (; *s; s++, x += 6 * scale) {
		int g = glyph(*s);

		for (int col = 0; col < 5; col++)
			for (int row = 0; row < 7; row++)
				if (font[g][col] & (1u << row))
					rect(d, x + col * scale, y + row * scale,
					     scale, scale, color);
	}
}

static int flush(struct display *d)
{
	const uint8_t cols[4] = { 0, 0, (uint8_t)((PANEL_W - 1) >> 8),
				  (uint8_t)(PANEL_W - 1) };
	const uint8_t rows[4] = { 0, 0, (uint8_t)((PANEL_H - 1) >> 8),
				  (uint8_t)(PANEL_H - 1) };
	const uint8_t ramwr = 0x2c;
	const size_t total = (size_t)PANEL_W * PANEL_H;
	struct spi_ioc_transfer tr[40];
	uint8_t *frame;
	size_t off = 0;
	int index = 0;
	int rc;

	frame = malloc(total * 2);
	if (!frame)
		return -1;

	for (size_t i = 0; i < total; i++) {
		frame[2 * i] = (uint8_t)(d->pixels[i] >> 8);
		frame[2 * i + 1] = (uint8_t)(d->pixels[i] & 0xff);
	}

	if (lcd_cmd(d, 0x2a, cols, sizeof(cols)) < 0 ||
	    lcd_cmd(d, 0x2b, rows, sizeof(rows)) < 0) {
		free(frame);
		return -1;
	}

	/* Hold software CS low from RAMWR through the complete pixel stream. */
	if (cs_assert(d) < 0 ||
	    dc_write(d, 0) < 0 ||
	    spi_write_all(d->spi, &ramwr, 1) < 0 ||
	    dc_write(d, 1) < 0) {
		int saved_errno = errno;
		cs_release(d);
		free(frame);
		errno = saved_errno;
		return -1;
	}

	memset(tr, 0, sizeof(tr));
	while (off < total * 2 && index < (int)(sizeof(tr) / sizeof(tr[0]))) {
		size_t n = total * 2 - off;

		if (n > SPI_CHUNK)
			n = SPI_CHUNK;
		tr[index].tx_buf = (uintptr_t)(frame + off);
		tr[index].len = (uint32_t)n;
		tr[index].speed_hz = g_spi_hz;
		tr[index].bits_per_word = 8;
		off += n;
		index++;
	}

	rc = ioctl(d->spi, SPI_IOC_MESSAGE(index), tr) < 0 ? -1 : 0;
	{
		int saved_errno = errno;
		if (cs_release(d) < 0 && rc == 0)
			rc = -1;
		else if (rc < 0)
			errno = saved_errno;
	}
	free(frame);
	return rc;
}

static void json_string(const char *json, const char *key, char *out, size_t out_len)
{
	char needle[64];
	const char *p;
	size_t i = 0;

	snprintf(needle, sizeof(needle), "\"%s\":\"", key);
	p = strstr(json, needle);
	if (!p) {
		snprintf(out, out_len, "UNAVAILABLE");
		return;
	}
	p += strlen(needle);
	while (*p && *p != '"' && i + 1 < out_len) {
		if (*p == '\\' && p[1])
			p++;
		out[i++] = *p++;
	}
	out[i] = '\0';
	if (i == 0)
		snprintf(out, out_len, "UNAVAILABLE");
}

static int json_int_after(const char *start, const char *key, int fallback)
{
	char needle[40];
	const char *p;
	int value;
	snprintf(needle, sizeof(needle), "\"%s\":", key);
	p = strstr(start, needle);
	if (!p)
		return fallback;
	p += strlen(needle);
	return sscanf(p, "%d", &value) == 1 ? value : fallback;
}

struct snapshot {
	char ipv4[40];
	char fix[16];
	char utc[40];
	char ntp_state[24];
	char reference[40];
	int sat_count;
	int sat_used_count;
	int quality;
	int sat_prn[64];
	int sat_gnss[64];
	int sat_snr[64];
	int sat_used[64];
};

static void read_status(struct snapshot *s)
{
	char json[STATUS_JSON_MAX] = "";
	FILE *f = popen(STATUS_BIN " 2>/dev/null", "r");

	if (f) {
		size_t n = fread(json, 1, sizeof(json) - 1, f);
		json[n] = '\0';
		pclose(f);
	}

	json_string(json, "ipv4", s->ipv4, sizeof(s->ipv4));
	json_string(json, "fix", s->fix, sizeof(s->fix));
	json_string(json, "utc", s->utc, sizeof(s->utc));
	json_string(json, "state", s->ntp_state, sizeof(s->ntp_state));
	json_string(json, "reference", s->reference, sizeof(s->reference));
	s->sat_used_count = json_int_after(json, "satellites_used", 0);
	s->quality = json_int_after(json, "quality", 0);
	s->sat_count = 0;
	const char *p = strstr(json, "\"satellites\":[");
	if (p) {
		p += strlen("\"satellites\":[");
		while (*p && *p != ']' && s->sat_count < 64) {
			const char *end;
			if (*p != '{') { p++; continue; }
			end = strchr(p, '}');
			if (!end) break;
			char object[192];
			size_t n = (size_t)(end - p + 1);
			if (n >= sizeof(object)) n = sizeof(object) - 1;
			memcpy(object, p, n); object[n] = '\0';
			int snr = json_int_after(object, "snr", 0);
			if (snr > 0) {
				int i = s->sat_count++;
				s->sat_prn[i] = json_int_after(object, "prn", 0);
				s->sat_gnss[i] = json_int_after(object, "gnss", 0);
				s->sat_snr[i] = snr;
				s->sat_used[i] = strstr(object, "\"used\":true") != NULL;
			}
			p = end + 1;
		}
	}
	g_sat_count = s->sat_count;
	if (g_sat_offset >= g_sat_count)
		g_sat_offset = g_sat_count > 10 ? ((g_sat_count - 1) / 10) * 10 : 0;
}

static void draw_page(struct display *d, int page, const struct config *cfg)
{
	static const uint16_t bg = 0x08a3, panel = 0x19e7, white = 0xef7d;
	static const uint16_t aqua = 0x16d5, muted = 0x8d96;
	struct snapshot snap;
	char line[64];
	time_t now;
	struct tm tmv;

	(void)cfg;
	read_status(&snap);
	now = time(NULL);
	localtime_r(&now, &tmv);

	rect(d, 0, 0, PANEL_W, PANEL_H, bg);
	rect(d, 0, 0, PANEL_W, 42, panel);
	draw_text(d, 12, 12, "YSPEED GNSS NTP", 2, aqua);

	switch (page) {
	case 0:
		draw_text(d, 12, 54, "LOCAL SYSTEM TIME", 1, muted);
		strftime(line, sizeof(line), "%H:%M:%S", &tmv);
		draw_text(d, 12, 72, line, 3, white);
		strftime(line, sizeof(line), "%Y-%m-%d", &tmv);
		draw_text(d, 12, 108, line, 2, white);
		rect(d, 10, 140, PANEL_W - 20, 2, aqua);
		draw_text(d, 12, 152, "ETHERNET", 1, muted);
		draw_text(d, 12, 168, snap.ipv4, 1, white);
		draw_text(d, 12, 194, "GNSS FIX", 1, muted);
		draw_text(d, 12, 210, snap.fix, 1, white);
		draw_text(d, 12, 240, "RECEIVER UTC", 1, muted);
		draw_text(d, 12, 256, snap.utc, 1, white);
		draw_text(d, 12, 286, "CHRONY", 1, aqua);
		draw_text(d, 74, 286, snap.ntp_state, 1, white);
		break;
	case 1:
		draw_text(d, 12, 58, "GNSS STATUS", 2, aqua);
		draw_text(d, 12, 100, "FIX MODE", 1, muted);
		draw_text(d, 12, 118, snap.fix, 1, white);
		draw_text(d, 12, 158, "RECEIVER UTC", 1, muted);
		draw_text(d, 12, 176, snap.utc, 1, white);
		draw_text(d, 12, 230, "ANTENNA REQUIRED", 1, aqua);
		draw_text(d, 12, 250, "FOR SATELLITE FIX", 1, aqua);
		break;
	case 2:
		draw_text(d, 12, 58, "NETWORK AND TIME", 2, aqua);
		draw_text(d, 12, 100, "ETHERNET ADDRESS", 1, muted);
		draw_text(d, 12, 118, snap.ipv4, 1, white);
		draw_text(d, 12, 158, "CHRONY STATE", 1, muted);
		draw_text(d, 12, 176, snap.ntp_state, 1, white);
		draw_text(d, 12, 216, "REFERENCE", 1, muted);
		draw_text(d, 12, 234, snap.reference, 1, white);
		draw_text(d, 12, 284, "LOCAL NTP UDP 123", 1, aqua);
		break;
	default: {
		char label[12];
		draw_text(d, 12, 54, "SATELLITES", 2, aqua);
		snprintf(line, sizeof(line), "VISIBLE %d  USED %d  QUALITY %d%%",
			 snap.sat_count, snap.sat_used_count, snap.quality);
		draw_text(d, 12, 84, line, 1, muted);
		int shown = snap.sat_count - g_sat_offset;
		if (shown > 10) shown = 10;
		for (int i = 0; i < shown; i++) {
			int k = i + g_sat_offset;
			int x = 14 + i * 22;
			int h = snap.sat_snr[k] * 2;
			static const char constellation[] = { 'G', 'S', 'E', 'C', 'I', 'J', 'R', 'I' };
			uint16_t color = snap.sat_used[k] ? aqua : muted;
			if (h > 142) h = 142;
			rect(d, x, 242 - h, 13, h, color);
			int gnss = snap.sat_gnss[k];
			char system = gnss >= 0 && gnss < (int)sizeof(constellation) ? constellation[gnss] : 'U';
			snprintf(label, sizeof(label), "%c%d", system, snap.sat_prn[k]);
			draw_text(d, x - 2, 248, label, 1, white);
		}
		draw_text(d, 12, 280, "BARS SHOW SIGNAL DBHZ", 1, muted);
		if (snap.sat_count > 10)
			draw_text(d, 12, 296, "UP/DOWN SCROLL SATELLITES", 1, aqua);
		else
			draw_text(d, 12, 296, "BRIGHT BARS USED IN FIX", 1, aqua);
		break;
	}
	}
	draw_text(d, 12, 306, "UP/DOWN: PAGES  ENTER: NEXT", 1, muted);

	if (flush(d) < 0)
		fprintf(stderr, "spi: frame transfer failed: %s\n", strerror(errno));
}

static void config_load(struct config *cfg)
{
	char line[160];
	FILE *f;

	cfg->rotation = 3;          /* verified working orientation on this unit */
	cfg->sleep_seconds = 120;   /* vendor default */
	cfg->english = 1;

	f = fopen(CONFIG_PATH, "r");
	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		char *key;
		char *value;

		if (!eq)
			continue;
		*eq = '\0';
		key = trim(line);
		value = trim(eq + 1);

		if (strcmp(key, "rotation") == 0) {
			int r = atoi(value);

			if (r >= 0 && r <= 3)
				cfg->rotation = r;
		} else if (strcmp(key, "backlight_sleep_time") == 0) {
			int s = atoi(value);

			if (s >= 0)
				cfg->sleep_seconds = s;
		} else if (strcmp(key, "language") == 0) {
			cfg->english = strcmp(value, "english") == 0;
		}
	}
	fclose(f);
}

static int config_save(const struct config *cfg)
{
	char text[96];
	int n = snprintf(text, sizeof(text), "rotation=%d\nbacklight_sleep_time=%d\n",
			 cfg->rotation, cfg->sleep_seconds);
	int fd = open(CONFIG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	ssize_t wrote = write(fd, text, (size_t)n);
	close(fd);
	return wrote == n ? 0 : -1;
}

static void draw_settings(struct display *d, int selected, const struct config *cfg)
{
	static const uint16_t bg = 0x08a3, aqua = 0x16d5, white = 0xef7d, muted = 0x8d96;
	char value[40];
	const char *labels[] = { "ROTATION", "BACKLIGHT TIMER", "EXIT SETTINGS" };
	rect(d, 0, 0, PANEL_W, PANEL_H, bg);
	draw_text(d, 12, 18, "DISPLAY SETUP", 2, aqua);
	for (int i = 0; i < 3; i++) {
		int y = 88 + i * 64;
		rect(d, 8, y - 8, PANEL_W - 16, 44, i == selected ? 0x19e7 : bg);
		draw_text(d, 16, y, labels[i], 1, i == selected ? aqua : white);
		if (i == 0) {
			snprintf(value, sizeof(value), "%d", cfg->rotation);
			draw_text(d, 178, y, value, 1, white);
		} else if (i == 1) {
			snprintf(value, sizeof(value), "%d SEC", cfg->sleep_seconds);
			draw_text(d, 154, y, value, 1, white);
		}
	}
	draw_text(d, 12, 286, "UP/DOWN SELECT ENTER APPLY", 1, muted);
	draw_text(d, 12, 304, "HOLD UP TO RETURN", 1, muted);
	if (flush(d) < 0)
		fprintf(stderr, "spi: settings frame transfer failed: %s\n", strerror(errno));
}

static int open_keys(void)
{
	char path[64];

	for (int i = 0; i < 16; i++) {
		unsigned long bits[16] = { 0 };
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;

		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
		    (bits[KEY_UP / (8 * sizeof(unsigned long))] &
		     (1UL << (KEY_UP % (8 * sizeof(unsigned long))))) &&
		    (bits[KEY_DOWN / (8 * sizeof(unsigned long))] &
	     (1UL << (KEY_DOWN % (8 * sizeof(unsigned long))))) &&
		    (bits[KEY_ENTER / (8 * sizeof(unsigned long))] &
	     (1UL << (KEY_ENTER % (8 * sizeof(unsigned long))))))
			return fd;

		close(fd);
	}
	return -1;
}

static int selftest(void)
{
	struct utsname u;
	int ok = 1;

	printf("== yspeed-display selftest ==\n");
	printf("build       : %s\n", YSPEED_BUILD);

	if (uname(&u) == 0)
		printf("kernel      : %s %s\n", u.sysname, u.release);
	else
		printf("kernel      : unknown\n");

	printf("gpiochips   :\n");
	{
		DIR *dir = opendir("/sys/class/gpio");
		struct dirent *entry;

		if (!dir) {
			printf("  /sys/class/gpio missing - CONFIG_GPIO_SYSFS=n?\n");
			ok = 0;
		} else {
			while ((entry = readdir(dir))) {
				char path[256], label[128] = "";
				int base = -1, ngpio = -1;
				FILE *f;

				if (strncmp(entry->d_name, "gpiochip", 8) != 0)
					continue;
				snprintf(path, sizeof(path),
					 "%s/%s/base", g_gpio_sysfs_root, entry->d_name);
				read_int_file(path, &base);
				snprintf(path, sizeof(path),
					 "%s/%s/ngpio", g_gpio_sysfs_root, entry->d_name);
				read_int_file(path, &ngpio);
				snprintf(path, sizeof(path),
					 "%s/%s/label", g_gpio_sysfs_root, entry->d_name);
				f = fopen(path, "r");
				if (f) {
					if (!fgets(label, sizeof(label), f))
						label[0] = '\0';
					fclose(f);
				}
				printf("  %-14s base=%-4d ngpio=%-4d %s",
				       entry->d_name, base, ngpio, label);
				if (strchr(label, '\n') == NULL)
					printf("\n");
			}
			closedir(dir);
		}
	}

	struct { const char *name; int offset; } pins[] = {
		{ "LCD_SPI_CS", GPIO_CS_OFFSET },
		{ "LCD_RESET", GPIO_RESET_OFFSET },
		{ "LCD_DC", GPIO_DC_OFFSET },
		{ "LCD_BACKLIGHT", GPIO_BACKLIGHT_OFFSET },
	};
	int resolved_numbers[sizeof(pins) / sizeof(pins[0])];

	for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		int resolved = gpio_chip_base_for(pins[i].offset);
		resolved_numbers[i] = resolved;
		if (resolved < 0) {
			printf("%-12s: SoC offset %d -> NOT RESOLVABLE (%s)\n",
			       pins[i].name, pins[i].offset, g_gpio_chip_rule);
			ok = 0;
		} else if (g_gpio_chip_bank != pins[i].offset / GPIO_BANK_WIDTH ||
			   g_gpio_line_offset != pins[i].offset % GPIO_BANK_WIDTH ||
			   resolved != g_gpio_chip_base + g_gpio_line_offset) {
			printf("%-12s: SoC offset %d -> INVALID mapping (base=%d bank=%d line=%d resolved=%d)\n",
			       pins[i].name, pins[i].offset, g_gpio_chip_base,
			       g_gpio_chip_bank, g_gpio_line_offset, resolved);
			ok = 0;
		} else {
			for (size_t j = 0; j < i; j++) {
				if (resolved_numbers[j] == resolved) {
					printf("%-12s: SoC offset %d -> DUPLICATE GPIO %d\n",
					       pins[i].name, pins[i].offset, resolved);
					ok = 0;
				}
			}
			printf("%-12s: SoC offset %d -> bank %d, chip base %d, line %d, sysfs gpio%d [%s]\n",
			       pins[i].name, pins[i].offset, g_gpio_chip_bank,
			       g_gpio_chip_base, g_gpio_line_offset, resolved,
			       g_gpio_chip_rule);
		}
	}

	if (access("/dev/spidev0.1", F_OK) == 0)
		printf("spidev      : /dev/spidev0.1 present\n");
	else {
		printf("spidev      : /dev/spidev0.1 MISSING\n");
		printf("              check CONFIG_SPI_SPIDEV and the st7789@1 node\n");
		ok = 0;
	}

	if (access(CONFIG_PATH, F_OK) == 0)
		printf("config      : %s present\n", CONFIG_PATH);
	else
		printf("config      : %s absent (defaults: rotation 3, sleep 120)\n",
		       CONFIG_PATH);

	if (!ok) {
		printf("result      : FAILED before touching the panel\n");
		return 1;
	}
	return 0;
}

static int color_cycle(struct display *d, const struct config *cfg)
{
	/*
	 * WHITE is last and held. The older order ended on BLACK, which looks
	 * exactly like a dead panel once the command returns: a working glass and
	 * a broken one printed the same thing. A full white frame cannot be
	 * confused with either a dark panel or a timed-out backlight.
	 */
	static const struct { const char *name; uint16_t color; } steps[] = {
		{ "RED", 0xf800 }, { "GREEN", 0x07e0 }, { "BLUE", 0x001f },
		{ "BLACK", 0x0000 }, { "WHITE", 0xffff },
	};

	if (panel_init(d, cfg) < 0) {
		fprintf(stderr, "panel init failed\n");
		return 1;
	}

	for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		printf("fill %s\n", steps[i].name);
		fflush(stdout);
		fill(d, steps[i].color);
		if (flush(d) < 0) {
			fprintf(stderr, "flush failed: %s\n", strerror(errno));
			return 1;
		}
		msleep(1200);
	}

	/*
	 * Hold the final white frame long enough to be seen across a bench. A dark
	 * glass at this point means the bytes are not reaching the controller: the
	 * framebuffer and the GPIO mapping are already proven.
	 */
	printf("panel should now be solid WHITE for 15 s - look at it\n");
	fflush(stdout);
	msleep(15000);
	printf("selftest complete\n");
	return 0;
}

/*
 * Hold a full white frame until SIGTERM: the only question the bench can
 * answer is whether the glass lights up at all, and this state cannot be
 * mistaken for a blanked backlight or a redraw timeout.
 */
static int hold_white(struct display *d, const struct config *cfg)
{
	if (panel_init(d, cfg) < 0) {
		fprintf(stderr, "panel init failed: %s\n", strerror(errno));
		return 1;
	}
	fill(d, 0xffff);
	if (flush(d) < 0) {
		fprintf(stderr, "flush failed: %s\n", strerror(errno));
		return 1;
	}
	printf("panel filled white and held; Ctrl-C to stop\n");
	fflush(stdout);
	while (g_running)
		msleep(500);
	return 0;
}


/* SLPOUT, COLMOD, MADCTL, DISPON only: tests whether the longer init hurts. */
static int panel_init_min(struct display *d, const struct config *cfg)
{
	const uint8_t colmod = 0x55;
	const uint8_t madctl = madctl_for(cfg->rotation);

	gpio_set(d->backlight, g_bl_fixed >= 0 ? g_bl_fixed : 0);
	if (!g_skip_reset) {
		gpio_set(d->reset, 0);
		msleep(30);
	}
	gpio_set(d->reset, 1);
	msleep(150);
	if (lcd_cmd(d, 0x11, NULL, 0) < 0 ||
	    lcd_cmd(d, 0x3a, &colmod, 1) < 0 ||
	    lcd_cmd(d, 0x36, &madctl, 1) < 0 ||
	    lcd_cmd(d, 0x29, NULL, 0) < 0)
		return -1;
	msleep(120);
	return gpio_set(d->backlight, g_bl_fixed >= 0 ? g_bl_fixed : 1);
}

/*
 * One boot, every remaining electrical and timing candidate. Each variant ends
 * in a full white frame held for three seconds, so the glass names the winner:
 * the last "PROBE n/9" line printed before the screen lights is the working
 * configuration. Nothing here runs in the daemon path.
 */
/*
 * RDDID (0x04) is the only command that makes the panel talk back, and the
 * controller's read path is the same one the boot flash uses. A real reply
 * proves clock, MOSI, CS, D/C and MISO all work and that the panel is alive;
 * silence is itself informative.
 */
static int panel_id(struct display *d, uint8_t *id, size_t n)
{
	struct spi_ioc_transfer tr = { 0 };
	uint8_t cmd = 0x04;   /* RDDID */
	int rc = -1;

	memset(id, 0, n);
	if (cs_assert(d) < 0)
		return -1;
	if (dc_write(d, 0) < 0 || spi_write_all(d->spi, &cmd, 1) < 0)
		goto out;
	if (dc_write(d, 1) < 0)
		goto out;
	tr.rx_buf = (unsigned long)id;
	tr.len = n;
	tr.speed_hz = g_spi_hz;
	tr.bits_per_word = 8;
	if (ioctl(d->spi, SPI_IOC_MESSAGE(1), &tr) < 0)
		goto out;
	rc = 0;
out:
	cs_release(d);
	return rc;
}

static int panel_probe(struct display *d, const struct config *cfg)
{
	static const struct {
		const char *label;
		uint32_t hz;
		int dc_inv, cs_hold, reset_mode, skip_reset, minimal, bl_fixed, swap;
	} combos[] = {
		{ "baseline: D/C normal, CS toggles, reset, full init", 10000000, 0, 0, 0, 0, 0, -1, 0 },
		{ "40 MHz", 40000000, 0, 0, 0, 0, 0, -1, 0 },
		{ "100 kHz slow clock", 100000, 0, 0, 0, 0, 0, -1, 0 },
		{ "D/C inverted (command=1, data=0)", 10000000, 1, 0, 0, 0, 0, -1, 0 },
		{ "D/C and CS swapped (pad16=CS, pad6=D/C)", 10000000, 0, 0, 0, 0, 0, -1, 1 },
		{ "CS held low across all commands", 10000000, 0, 1, 0, 0, 0, -1, 0 },
		{ "reset pulse inverted (assert high)", 10000000, 0, 0, 2, 0, 0, -1, 0 },
		{ "no reset pulse", 10000000, 0, 0, 0, 1, 0, -1, 0 },
		{ "GPIO14 held low (power-enable shape)", 10000000, 0, 0, 3, 0, 0, -1, 0 },
		{ "GPIO14 held high", 10000000, 0, 0, 4, 0, 0, -1, 0 },
		{ "GPIO19 driven low throughout", 10000000, 0, 0, 0, 0, 0, 0, 0 },
		{ "GPIO19 driven high throughout", 10000000, 0, 0, 0, 0, 0, 1, 0 },
		{ "minimal init (SLPOUT, COLMOD, MADCTL, DISPON)", 10000000, 0, 0, 0, 0, 1, -1, 0 },
	};
	const unsigned n = (unsigned)(sizeof(combos) / sizeof(combos[0]));

	printf("== yspeed-display %s panel probe: %u variants ==\n", YSPEED_BUILD, n);
	{
		uint8_t id[4];

		if (panel_id(d, id, sizeof(id)) == 0)
			printf("RDDID before init : %02x %02x %02x %02x%s\n",
			       id[0], id[1], id[2], id[3],
			       (id[0] == 0xff && id[1] == 0xff) ?
			       "  (all 0xff - nothing driving MISO)" :
			       (id[0] == 0 && id[1] == 0 && id[2] == 0 && id[3] == 0) ?
			       "  (all zero - no answer, or MISO not wired)" : "");
		else
			printf("RDDID before init : read failed: %s\n", strerror(errno));
		fflush(stdout);
	}
	for (unsigned i = 0; i < n; i++) {
		int rc;
		struct timespec t0, t1;
		long ms;

		g_dc_invert = combos[i].dc_inv;
		g_cs_hold = combos[i].cs_hold;
		g_skip_reset = combos[i].skip_reset;
		g_reset_mode = combos[i].reset_mode;
		g_swap_dc_cs = combos[i].swap;
		g_bl_fixed = combos[i].bl_fixed;

		printf("PROBE %u/%u: %s @ %u Hz -> glass must be WHITE for 3 s\n",
		       i + 1, n, combos[i].label, combos[i].hz);
		fflush(stdout);

		if (spi_set_speed(d->spi, combos[i].hz) < 0) {
			printf("  speed rejected: %s\n", strerror(errno));
			continue;
		}
		rc = combos[i].minimal ? panel_init_min(d, cfg) : panel_init(d, cfg);
		if (rc < 0) {
			printf("  init failed: %s\n", strerror(errno));
			goto next;
		}
		fill(d, 0xffff);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		if (flush(d) < 0) {
			printf("  frame failed: %s\n", strerror(errno));
			goto next;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		ms = (t1.tv_sec - t0.tv_sec) * 1000L +
		     (t1.tv_nsec - t0.tv_nsec) / 1000000L;
		/*
		 * 307200 bytes at 40 MHz needs about 61 ms, at 1 MHz about 2.5 s.
		 * A frame that returns in no time at all was never clocked out, which
		 * is the difference between "the panel ignored us" and "nothing was
		 * ever sent".
		 */
		if (ms > 0)
			printf("  frame: 307200 B in %ld ms (~%ld kbit/s)\n", ms,
			       (long)(PANEL_W * PANEL_H * 2L * 8L / ms));
		else
			printf("  frame: returned in under 1 ms - no data was clocked\n");
		msleep(3000);
	next:
		if (d->cs >= 0)
			gpio_set(d->cs, 1);
	}

	g_dc_invert = 0;
	g_cs_hold = 0;
	g_skip_reset = 0;
	g_reset_mode = 0;
	g_swap_dc_cs = 0;
	g_bl_fixed = -1;
	printf("probe complete; the last PROBE line before the glass lit is the winner\n");
	return 0;
}

static int display_open(struct display *d)
{
	uint8_t mode = g_spi_mode | (g_lsb_first ? SPI_LSB_FIRST : 0) |
		       (g_hw_cs ? 0 : SPI_NO_CS);
	uint8_t applied_mode = 0;
	uint8_t bits = 8;
	uint32_t speed = SPI_HZ;

	d->spi = open("/dev/spidev0.1", O_RDWR);   /* RDWR so RDDID can answer */
	if (d->spi < 0) {
		fprintf(stderr, "open /dev/spidev0.1: %s\n", strerror(errno));
		return -1;
	}

	if (ioctl(d->spi, SPI_IOC_WR_MODE, &mode) < 0 ||
	    ioctl(d->spi, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(d->spi, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0 ||
	    ioctl(d->spi, SPI_IOC_RD_MODE, &applied_mode) < 0) {
		fprintf(stderr, "configure SPI: %s\n", strerror(errno));
		close(d->spi);
		d->spi = -1;
		return -1;
	}
	if (!g_hw_cs && !(applied_mode & SPI_NO_CS)) {
		fprintf(stderr, "SPI controller did not retain SPI_NO_CS; refusing unsafe CS configuration\n");
		close(d->spi);
		d->spi = -1;
		errno = EOPNOTSUPP;
		return -1;
	}
	if (g_hw_cs)
		fprintf(stderr, "SPI: mode %u, %u bits, %u Hz, native CS1 toggled per message\n",
			g_spi_mode, bits, speed);
	else
		fprintf(stderr, "SPI: mode %u, %u bits, %u Hz, CS1 held asserted (vendor NO_CS)\n",
			g_spi_mode, bits, speed);
	return 0;
}

static int gpio_open(struct display *d)
{
	/*
	 * Claim the vendor's enable line first and leave it high. It is not part
	 * of the display configuration the vendor prints, so it was the one line
	 * we never reproduced; a failure here is not fatal, because the panel
	 * pins below are what the display itself drives.
	 */
	if (gpio_claim(GPIO_POWER_OFFSET, &g_power_gpio, 1) == 0)
		fprintf(stderr, "GPIO: vendor enable pin %d held high\n",
			g_power_gpio);
	else
		fprintf(stderr, "gpio: warning: vendor enable pin (offset %d) not driven\n",
			GPIO_POWER_OFFSET);

	/* CS is the controller's native CS1; userspace never claims that pad. */
	if (gpio_claim(g_dc_off, &d->dc, 0) < 0 ||
	    gpio_claim(g_rst_off, &d->reset, 1) < 0 ||
	    gpio_claim(g_bl_off, &d->backlight, 0) < 0) {
		fprintf(stderr, "unable to claim ST7789 GPIO lines\n");
		return -1;
	}
	return 0;
}

/*
 * One boot, many wiring hypotheses. Each variant re-opens the bus with a
 * different clock phase and re-claims D/C and reset on different SoC offsets,
 * then sends a full white frame, so the operator can see which combination,
 * if any, the glass answers. Reboot-free because every axis we doubt is now a
 * runtime knob instead of a rebuild.
 */
static int panel_sweep(struct display *d, const struct config *cfg)
{
	static const struct variant {
		uint8_t mode;
		int dc;
		int rst;
		uint32_t hz;
	} variants[] = {
		{ SPI_MODE_0, 16, 14, 24000000 },	/* wiring we assumed so far */
		{ SPI_MODE_3, 16, 14, 24000000 },	/* data on the other edge */
		{ SPI_MODE_0, 15, 14, 24000000 },	/* D/C on the spare vendor pin */
		{ SPI_MODE_3, 15, 14, 24000000 },
		{ SPI_MODE_0, 16, 15, 24000000 },	/* reset on the spare vendor pin */
		{ SPI_MODE_0, 15, 15, 24000000 },
		{ SPI_MODE_0, 16, 14, 1000000 },	/* slow clock, long wires */
		{ SPI_MODE_3, 15, 14, 1000000 },
	};
	size_t n = sizeof(variants) / sizeof(variants[0]);
	size_t i;

	for (i = 0; i < n; i++) {
		const struct variant *v = &variants[i];

		g_spi_mode = v->mode;
		g_dc_off = v->dc;
		g_rst_off = v->rst;
		g_spi_hz = v->hz;
		printf("\n=== sweep %zu/%zu: SPI mode %u, D/C offset %d, reset offset %d, %u Hz ===\n",
		       i + 1, n, v->mode, v->dc, v->rst, v->hz);
		fflush(stdout);
		if (d->spi >= 0) {
			close(d->spi);
			d->spi = -1;
		}
		if (display_open(d) < 0 || gpio_open(d) < 0)
			continue;
		if (panel_init(d, cfg) < 0) {
			printf("   init did not complete\n");
			continue;
		}
		fill(d, 0xffff);
		if (flush(d) < 0)
			printf("   white frame failed: %s\n", strerror(errno));
		else
			printf("   white frame sent - look at the glass now\n");
		msleep(2500);
		fill(d, 0x0000);
		flush(d);
		msleep(400);
	}
	printf("\nsweep finished; if nothing lit, D/C, reset and clock phase are not the fault\n");
	return 0;
}

int main(int argc, char **argv)
{
	/* static: the struct carries a 150 KiB framebuffer, too large for the stack */
	static struct display d = { .spi = -1, .cs = -1, .dc = -1, .reset = -1, .backlight = -1 };
	struct config cfg;
	const char *mode = NULL;
	int argi;

	for (argi = 1; argi < argc; argi++) {
		const char *a = argv[argi];
		int has_value = argi + 1 < argc;

		if (strcmp(a, "--hw-cs") == 0) {
			g_hw_cs = 1;
			continue;
		}
		if (strcmp(a, "--sw-cs") == 0) {
			g_hw_cs = 0;
			continue;
		}
		if (strcmp(a, "--lsb") == 0) {
			g_lsb_first = 1;
			continue;
		}
		if (strcmp(a, "--dc") == 0 && has_value) {
			g_dc_off = atoi(argv[++argi]);
			continue;
		}
		if (strcmp(a, "--rst") == 0 && has_value) {
			g_rst_off = atoi(argv[++argi]);
			continue;
		}
		if (strcmp(a, "--bl") == 0 && has_value) {
			g_bl_off = atoi(argv[++argi]);
			continue;
		}
		if (strcmp(a, "--mode") == 0 && has_value) {
			g_spi_mode = (uint8_t)(atoi(argv[++argi]) & 3);
			continue;
		}
		if (strcmp(a, "--speed") == 0 && has_value) {
			g_spi_hz = (uint32_t)strtoul(argv[++argi], NULL, 0);
			continue;
		}
		if (mode != NULL) {
			fprintf(stderr, "yspeed-display: unexpected argument '%s'\n", a);
			return 2;
		}
		mode = a;
	}
	struct sigaction sa;
	int keys;
	int page = 0;
	int in_settings = 0;
	int setting = 0;
	int pressed_code = -1;
	int64_t pressed_us = 0;
	time_t last_activity;
	int backlight_on = 1;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	config_load(&cfg);

	/*
	 * An unknown option used to fall through and start the daemon, which made
	 * an old image look like a new one whenever a diagnostic flag was missing.
	 */
	if (mode && mode[0] == '-' &&
	    strcmp(mode, "--selftest") != 0 && strcmp(mode, "--test") != 0 &&
	    strcmp(mode, "--white") != 0 && strcmp(mode, "--panel-probe") != 0 &&
	    strcmp(mode, "--panel-id") != 0 && strcmp(mode, "--sweep") != 0) {
		fprintf(stderr, "yspeed-display %s: unknown option '%s'\n"
			"options: --selftest --test --white --panel-probe --panel-id --sweep\n"
			"         [--hw-cs|--sw-cs] [--dc N] [--rst N] [--bl N] [--mode 0|3]\n"
			"         [--speed HZ] [--lsb]\n",
			YSPEED_BUILD, mode);
		return 2;
	}
	fprintf(stderr, "yspeed-display %s starting\n", YSPEED_BUILD);

	if (mode && (strcmp(mode, "--selftest") == 0 ||
		     strcmp(mode, "--test") == 0)) {
		int rc = selftest();

		if (rc != 0)
			return rc;
		if (!mode || strcmp(mode, "--test") != 0)
			return 0;
	}

	if (gpio_open(&d) < 0)
		return 1;
	if (display_open(&d) < 0)
		return 1;

	if (mode && strcmp(mode, "--test") == 0)
		return color_cycle(&d, &cfg);

	if (mode && strcmp(mode, "--white") == 0)
		return hold_white(&d, &cfg);

	if (mode && strcmp(mode, "--panel-id") == 0) {
		uint8_t id[4];

		if (panel_id(&d, id, sizeof(id)) < 0) {
			fprintf(stderr, "RDDID read failed: %s\n", strerror(errno));
			return 1;
		}
		printf("RDDID: %02x %02x %02x %02x\n", id[0], id[1], id[2], id[3]);
		return 0;
	}

	if (mode && strcmp(mode, "--sweep") == 0)
		return panel_sweep(&d, &cfg);

	if (mode && strcmp(mode, "--panel-probe") == 0)
		return panel_probe(&d, &cfg);

	if (panel_init(&d, &cfg) < 0) {
		fprintf(stderr, "ST7789 initialization failed\n");
		return 1;
	}

	/*
	 * Unambiguous power-on signal. A full white frame is the one state nobody
	 * can mistake for a dead panel or a blanked backlight, so the first thing
	 * the daemon draws is white, before any dashboard content.
	 */
	fill(&d, 0xffff);
	if (flush(&d) < 0)
		fprintf(stderr, "spi: white splash failed: %s\n", strerror(errno));
	else
		fprintf(stderr, "white splash drawn; a dark glass here means the panel is not receiving\n");
	msleep(3000);

	keys = open_keys();
	last_activity = time(NULL);
	draw_page(&d, page, &cfg);

	while (g_running) {
		struct pollfd pfd;
		int timeout_ms = 1000;
		int ready;

		if (keys < 0) {
			keys = open_keys();
			if (keys < 0) {
				/* No key device: keep the dashboard updating anyway. */
				if (backlight_on)
					draw_page(&d, page, &cfg);
				msleep(2000);
				continue;
			}
		}

		pfd.fd = keys;
		pfd.events = POLLIN;
		pfd.revents = 0;
		ready = poll(&pfd, 1, timeout_ms);

		if (ready > 0 && (pfd.revents & POLLIN)) {
			struct input_event ev;

			while (read(keys, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
				if (ev.type != EV_KEY)
					continue;
				if (ev.value == 1) {
					pressed_code = ev.code;
					pressed_us = (int64_t)ev.input_event_sec * 1000000 + ev.input_event_usec;
					continue;
				}
				if (ev.value != 0 || ev.code != pressed_code)
					continue;
				last_activity = time(NULL);
				if (!backlight_on) {
					gpio_set(d.backlight, 1);
					backlight_on = 1;
				}
				int64_t released_us = (int64_t)ev.input_event_sec * 1000000 + ev.input_event_usec;
				int held = released_us - pressed_us >= 1000000;
				pressed_code = -1;
				if (held && ev.code == KEY_DOWN) {
					in_settings = 1;
					setting = 0;
				} else if (held && ev.code == KEY_UP && in_settings) {
					in_settings = 0;
				} else if (in_settings) {
					if (ev.code == KEY_UP)
						setting = (setting + 2) % 3;
					else if (ev.code == KEY_DOWN)
						setting = (setting + 1) % 3;
					else if (ev.code == KEY_ENTER) {
						if (setting == 0) {
							cfg.rotation = (cfg.rotation + 1) % 4;
							panel_init(&d, &cfg);
						} else if (setting == 1) {
							static const int timers[] = { 0, 30, 60, 120, 300 };
							int next = 0;
							for (int i = 0; i < 5; i++) {
								if (timers[i] == cfg.sleep_seconds)
									next = (i + 1) % 5;
							}
							cfg.sleep_seconds = timers[next];
						}
						if (setting == 2)
							in_settings = 0;
						if (config_save(&cfg) < 0)
							fprintf(stderr, "settings: cannot save %s: %s\n",
								CONFIG_PATH, strerror(errno));
					}
				} else if (page == 3 && g_sat_count > 10 && ev.code == KEY_UP) {
					g_sat_offset -= 10;
					if (g_sat_offset < 0) g_sat_offset = ((g_sat_count - 1) / 10) * 10;
				} else if (page == 3 && g_sat_count > 10 && ev.code == KEY_DOWN) {
					g_sat_offset += 10;
					if (g_sat_offset >= g_sat_count) g_sat_offset = 0;
				} else if (ev.code == KEY_UP)
					page = (page + 3) % 4;
				else if (ev.code == KEY_DOWN || ev.code == KEY_ENTER)
					page = (page + 1) % 4;
			}
		}

		if (cfg.sleep_seconds > 0 && backlight_on &&
		    (time(NULL) - last_activity) >= cfg.sleep_seconds) {
			gpio_set(d.backlight, 0);
			backlight_on = 0;
		}

		if (backlight_on) {
			if (in_settings)
				draw_settings(&d, setting, &cfg);
			else
				draw_page(&d, page, &cfg);
		}
	}

	gpio_set(d.backlight, 0);
	if (d.cs >= 0)
		gpio_set(d.cs, 1);
	gpio_set(d.reset, 0);
	if (d.spi >= 0)
		close(d.spi);
	return 0;
}

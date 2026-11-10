/*
 * Host-side behavioral tests for the production GPIO resolver.
 * Includes the production translation unit so the static resolver is tested
 * directly. All sysfs access is redirected to disposable temporary fixtures.
 */
#define main yspeed_display_program_main
#include "../package/yspeed-display/src/yspeed-display.c"
#undef main

#include <sys/stat.h>

static int failures;

static void check(const char *name, int condition)
{
	printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
	if (!condition)
		failures++;
}

static void put_file(const char *path, const char *value)
{
	FILE *f = fopen(path, "w");
	if (!f) {
		perror(path);
		exit(2);
	}
	fputs(value, f);
	fclose(f);
}

static void fixture(const char *root)
{
	if (mkdir(root, 0700) < 0 && errno != EEXIST) {
		perror(root);
		exit(2);
	}
	snprintf(g_gpio_sysfs_root, sizeof(g_gpio_sysfs_root), "%s", root);
}

static void chip(const char *name, int base, int ngpio, const char *label,
		 int omit_ngpio)
{
	char dir[PATH_MAX], path[PATH_MAX], value[32];
	snprintf(dir, sizeof(dir), "%s/%s", g_gpio_sysfs_root, name);
	if (mkdir(dir, 0700) < 0) { perror(dir); exit(2); }
	snprintf(path, sizeof(path), "%s/base", dir);
	snprintf(value, sizeof(value), "%d\n", base);
	put_file(path, value);
	if (!omit_ngpio) {
		snprintf(path, sizeof(path), "%s/ngpio", dir);
		snprintf(value, sizeof(value), "%d\n", ngpio);
		put_file(path, value);
	}
	snprintf(path, sizeof(path), "%s/label", dir);
	put_file(path, label);
}

int main(void)
{
	char root[PATH_MAX], path[PATH_MAX];
	int n = -1;
	char template[] = "/tmp/yspeed-gpio-test-XXXXXX";
	char *tmp = mkdtemp(template);
	if (!tmp) { perror("mkdtemp"); return 2; }

	/* Vendor-kernel numbering: bank 0 base 512 => 526/528/531. */
	snprintf(root, sizeof(root), "%s/vendor", tmp);
	fixture(root);
	chip("gpiochip576", 512, 32, "10000600.gpio-bank0\n", 0);
	check("vendor reset maps to 526", gpio_chip_base_for(14) == 526);
	check("vendor D/C maps to 528", gpio_chip_base_for(16) == 528);
	check("vendor backlight maps to 531", gpio_chip_base_for(19) == 531);

	/* Mainline numbering is runtime-dependent; no hardcoded 512 assumption. */
	snprintf(root, sizeof(root), "%s/mainline", tmp);
	fixture(root);
	chip("gpiochip0", 0, 32, "10000600.gpio-bank0\n", 0);
	check("mainline reset maps to 14", gpio_chip_base_for(14) == 14);
	check("mainline D/C maps to 16", gpio_chip_base_for(16) == 16);
	check("mainline backlight maps to 19", gpio_chip_base_for(19) == 19);

	snprintf(root, sizeof(root), "%s/wrong-bank", tmp);
	fixture(root);
	chip("gpiochip512", 512, 32, "10000600.gpio-bank1\n", 0);
	check("wrong bank fails closed", gpio_chip_base_for(14) < 0);

	snprintf(root, sizeof(root), "%s/duplicate", tmp);
	fixture(root);
	chip("gpiochip512", 512, 32, "10000600.gpio-bank0\n", 0);
	chip("gpiochip544", 544, 32, "mt7628.gpio-bank0\n", 0);
	check("duplicate matching banks fail closed", gpio_chip_base_for(14) < 0);

	snprintf(root, sizeof(root), "%s/short-bank", tmp);
	fixture(root);
	chip("gpiochip512", 512, 10, "10000600.gpio-bank0\n", 0);
	check("bank too short for requested line fails closed", gpio_chip_base_for(14) < 0);

	snprintf(root, sizeof(root), "%s/incomplete", tmp);
	fixture(root);
	chip("gpiochip512", 512, 32, "10000600.gpio-bank0\n", 1);
	check("incomplete chip metadata fails closed", gpio_chip_base_for(14) < 0);

	snprintf(root, sizeof(root), "%s/unsupported-bank", tmp);
	fixture(root);
	chip("gpiochip608", 608, 32, "10000600.gpio-bank3\n", 0);
	check("unsupported fourth bank fails closed", gpio_chip_base_for(96) < 0);

	check("only LCD offsets are permitted",
	      gpio_lcd_offset_allowed(14) && gpio_lcd_offset_allowed(16) &&
	      gpio_lcd_offset_allowed(19) && !gpio_lcd_offset_allowed(18) &&
	      !gpio_lcd_offset_allowed(23) && !gpio_lcd_offset_allowed(26) &&
	      !gpio_lcd_offset_allowed(11) && !gpio_lcd_offset_allowed(38) &&
	      !gpio_lcd_offset_allowed(44));
	check("gpio_claim rejects key/PPS/reset/status offsets before sysfs writes",
	      gpio_claim(18, &n, 0) < 0 && gpio_claim(23, &n, 0) < 0 &&
	      gpio_claim(26, &n, 0) < 0 && gpio_claim(11, &n, 0) < 0 &&
	      gpio_claim(38, &n, 0) < 0 && gpio_claim(44, &n, 0) < 0);

	/* Prove tests did not export or alter any real GPIO. */
	check("test operated only inside temporary sysfs fixtures",
	      strstr(g_gpio_sysfs_root, tmp) == g_gpio_sysfs_root);
	snprintf(path, sizeof(path), "%s/vendor/gpiochip576/base", tmp);
	check("fixture remained readable", access(path, R_OK) == 0);

	printf("GPIO resolver tests: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}

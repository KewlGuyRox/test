#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <dirent.h>
#include <poll.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static const uint8_t font[40][5] = {
 {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
 {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c},
 {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
 {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f},
 {0x00,0x41,0x7f,0x41,0x00}, {0x20,0x40,0x41,0x3f,0x01},
 {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
 {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
 {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06},
 {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
 {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
 {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
 {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
 {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
 {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
 {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31},
 {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
 {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
 {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e},
 {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x14,0x00,0x00},
 {0x00,0x60,0x60,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}
};

struct screen {
    int fd;
    uint8_t *mem;
    size_t len;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
};

struct telemetry {
    char fix[32], utc[64], ntp[32], ref[48], leap[32];
    char last_offset[48], rms_offset[48], ip[32], gateway[32], dns[64];
    char pps_assert[48], pps_clear[48], rtc[64];
    double latitude, longitude, altitude, speed, course, hdop, vdop, pdop, uptime;
    int satellites, stratum;
    int have_lat, have_lon, have_alt, have_speed, have_course;
    int have_hdop, have_vdop, have_pdop, have_satellites, have_stratum, have_uptime;
    int pps_present;
};

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b, struct fb_var_screeninfo *v) {
    uint32_t p = 0;
    if (v->bits_per_pixel == 16)
        return ((uint32_t)(r >> 3) << 11) | ((uint32_t)(g >> 2) << 5) | (b >> 3);
    if (v->red.length) p |= ((uint32_t)(r >> (8-v->red.length)) << v->red.offset);
    if (v->green.length) p |= ((uint32_t)(g >> (8-v->green.length)) << v->green.offset);
    if (v->blue.length) p |= ((uint32_t)(b >> (8-v->blue.length)) << v->blue.offset);
    return p;
}
static void pixel(struct screen *s, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= (int)s->var.xres || y >= (int)s->var.yres) return;
    size_t o = (size_t)(y + s->var.yoffset) * s->fix.line_length +
               (size_t)(x + s->var.xoffset) * (s->var.bits_per_pixel / 8);
    if (o + s->var.bits_per_pixel/8 > s->len) return;
    if (s->var.bits_per_pixel == 16) *(uint16_t *)(s->mem + o) = (uint16_t)c;
    else if (s->var.bits_per_pixel == 32) *(uint32_t *)(s->mem + o) = c;
    else if (s->var.bits_per_pixel == 24) {
        s->mem[o] = c & 255; s->mem[o+1] = (c >> 8) & 255; s->mem[o+2] = (c >> 16) & 255;
    }
}
static void rect(struct screen *s, int x, int y, int w, int h, uint32_t c) {
    for (int yy=y; yy<y+h; yy++) for (int xx=x; xx<x+w; xx++) pixel(s,xx,yy,c);
}
static int glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return c-'A';
    if (c >= '0' && c <= '9') return 26+c-'0';
    if (c == ' ') return 36;
    if (c == ':') return 37;
    if (c == '.') return 38;
    if (c == '-') return 39;
    return 36;
}
static void text(struct screen *s, int x, int y, const char *str, int scale, uint32_t fg) {
    for (; *str; str++, x += 6*scale) {
        int g = glyph(*str);
        for (int col=0; col<5; col++) for (int row=0; row<7; row++)
            if (font[g][col] & (1u << row))
                rect(s, x+col*scale, y+row*scale, scale, scale, fg);
    }
}

static int json_string_value(const char *json, const char *key, char *out, size_t n) {
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}
static int json_number(const char *line, const char *key, double *v);

static int json_bool_value(const char *json, const char *key) {
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    return strncmp(p, "true", 4) == 0;
}

static void get_shared_status(struct telemetry *t) {
    snprintf(t->fix, sizeof(t->fix), "NO TELEMETRY");
    snprintf(t->utc, sizeof(t->utc), "UNAVAILABLE");
    snprintf(t->ntp, sizeof(t->ntp), "UNAVAILABLE");
    snprintf(t->ref, sizeof(t->ref), "NONE");
    snprintf(t->leap, sizeof(t->leap), "UNKNOWN");
    snprintf(t->ip, sizeof(t->ip), "UNAVAILABLE");
    snprintf(t->gateway, sizeof(t->gateway), "UNAVAILABLE");
    snprintf(t->dns, sizeof(t->dns), "UNAVAILABLE");
    snprintf(t->rtc, sizeof(t->rtc), "UNAVAILABLE");
    snprintf(t->pps_assert, sizeof(t->pps_assert), "UNAVAILABLE");
    snprintf(t->pps_clear, sizeof(t->pps_clear), "UNAVAILABLE");

    FILE *f = popen("/usr/sbin/yspeed-status 2>/dev/null", "r");
    if (!f) return;
    char json[8192];
    size_t used = fread(json, 1, sizeof(json) - 1, f);
    json[used] = '\0';
    pclose(f);

    /* Keys are intentionally searched in the shared nested JSON snapshot. */
    json_string_value(json, "fix", t->fix, sizeof(t->fix));
    json_string_value(json, "utc", t->utc, sizeof(t->utc));
    json_string_value(json, "state", t->ntp, sizeof(t->ntp));
    json_string_value(json, "reference", t->ref, sizeof(t->ref));
    json_string_value(json, "leap_status", t->leap, sizeof(t->leap));
    json_string_value(json, "last_offset", t->last_offset, sizeof(t->last_offset));
    json_string_value(json, "rms_offset", t->rms_offset, sizeof(t->rms_offset));
    json_string_value(json, "ipv4", t->ip, sizeof(t->ip));
    json_string_value(json, "gateway", t->gateway, sizeof(t->gateway));
    json_string_value(json, "dns", t->dns, sizeof(t->dns));
    json_string_value(json, "assert", t->pps_assert, sizeof(t->pps_assert));
    json_string_value(json, "clear", t->pps_clear, sizeof(t->pps_clear));
    json_string_value(json, "rtc", t->rtc, sizeof(t->rtc));
    t->pps_present = json_bool_value(json, "device");

    t->have_lat = json_number(json, "latitude", &t->latitude);
    t->have_lon = json_number(json, "longitude", &t->longitude);
    t->have_alt = json_number(json, "altitude_m", &t->altitude);
    t->have_speed = json_number(json, "speed_mps", &t->speed);
    t->have_course = json_number(json, "course_deg", &t->course);
    t->have_hdop = json_number(json, "hdop", &t->hdop);
    t->have_vdop = json_number(json, "vdop", &t->vdop);
    t->have_pdop = json_number(json, "pdop", &t->pdop);
    t->have_uptime = json_number(json, "uptime_seconds", &t->uptime);
    double value;
    if (json_number(json, "satellites_reported", &value)) {
        t->satellites = (int)value;
        t->have_satellites = 1;
    }
    if (json_number(json, "stratum", &value)) {
        t->stratum = (int)value;
        t->have_stratum = 1;
    }
}

static int json_number(const char *line, const char *key, double *v) {
    char k[32]; snprintf(k,sizeof(k),"\"%s\":",key);
    const char *p=strstr(line,k); if(!p) return 0;
    p+=strlen(k); while(*p==' ')p++;
    if(*p=='n') return 0;
    char *end=NULL; *v=strtod(p,&end); return end!=p;
}
static int has_key(const unsigned long *bits, int code) {
    const int width = (int)(sizeof(unsigned long) * 8);
    return (bits[code / width] >> (code % width)) & 1UL;
}

static int open_keys(void) {
    char path[64];
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long bits[(KEY_MAX + sizeof(unsigned long) * 8) /
                           (sizeof(unsigned long) * 8)];
        memset(bits, 0, sizeof(bits));
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
            has_key(bits, KEY_UP) && has_key(bits, KEY_DOWN) &&
            has_key(bits, KEY_ENTER))
            return fd;
        close(fd);
    }
    return -1;
}

static void set_backlight(int on) {
    DIR *d = opendir("/sys/class/backlight");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[256];
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/bl_power", e->d_name);
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "%d\n", on ? 0 : 4); fclose(f); }
    }
    closedir(d);
}

static void draw_status(struct screen *s, int page, const struct telemetry *t) {
    uint32_t bg=rgb(7,18,32,&s->var), panel=rgb(15,39,59,&s->var);
    uint32_t white=rgb(238,245,250,&s->var), accent=rgb(20,190,170,&s->var);
    uint32_t muted=rgb(140,170,190,&s->var);
    char line[80], value[64];
    time_t now=time(NULL); struct tm tmv; localtime_r(&now,&tmv);
    rect(s,0,0,s->var.xres,s->var.yres,bg);
    rect(s,0,0,s->var.xres,42,panel);

    if (page == 0) {
        text(s,12,12,"GNSS POSITION",2,accent);
        strftime(line,sizeof(line),"%H:%M:%S",&tmv);
        text(s,12,54,"LOCAL TIME",1,muted); text(s,12,70,line,2,white);
        snprintf(line,sizeof(line),"FIX %s",t->fix); text(s,12,106,line,1,accent);
        if (t->have_satellites) snprintf(line,sizeof(line),"SATELLITES %d",t->satellites);
        else snprintf(line,sizeof(line),"SATELLITES UNAVAILABLE");
        text(s,12,130,line,1,white);
        snprintf(line,sizeof(line),"UTC %.19s",t->utc); text(s,12,158,line,1,white);
        if (t->have_lat) snprintf(value,sizeof(value),"LAT %.6f",t->latitude);
        else snprintf(value,sizeof(value),"LAT UNAVAILABLE");
        text(s,12,190,value,1,white);
        if (t->have_lon) snprintf(value,sizeof(value),"LON %.6f",t->longitude);
        else snprintf(value,sizeof(value),"LON UNAVAILABLE");
        text(s,12,214,value,1,white);
        if (t->have_alt) snprintf(value,sizeof(value),"ALT %.1fM",t->altitude);
        else snprintf(value,sizeof(value),"ALT UNAVAILABLE");
        text(s,12,238,value,1,white);
        text(s,12,292,"UP DOWN CHANGE PAGE",1,muted);
    } else if (page == 1) {
        text(s,12,12,"GNSS QUALITY",2,accent);
        if (t->have_hdop) snprintf(line,sizeof(line),"HDOP %.2f",t->hdop);
        else snprintf(line,sizeof(line),"HDOP UNAVAILABLE");
        text(s,12,58,line,1,white);
        if (t->have_vdop) snprintf(line,sizeof(line),"VDOP %.2f",t->vdop);
        else snprintf(line,sizeof(line),"VDOP UNAVAILABLE");
        text(s,12,82,line,1,white);
        if (t->have_pdop) snprintf(line,sizeof(line),"PDOP %.2f",t->pdop);
        else snprintf(line,sizeof(line),"PDOP UNAVAILABLE");
        text(s,12,106,line,1,white);
        if (t->have_speed) snprintf(line,sizeof(line),"SPEED %.2f MPS",t->speed);
        else snprintf(line,sizeof(line),"SPEED UNAVAILABLE");
        text(s,12,142,line,1,white);
        if (t->have_course) snprintf(line,sizeof(line),"COURSE %.1f DEG",t->course);
        else snprintf(line,sizeof(line),"COURSE UNAVAILABLE");
        text(s,12,166,line,1,white);
        snprintf(line,sizeof(line),"PPS %s",t->pps_present ? "PRESENT" : "NOT FOUND");
        text(s,12,202,line,1,t->pps_present ? accent : muted);
        snprintf(line,sizeof(line),"ASSERT %.20s",t->pps_assert); text(s,12,226,line,1,white);
        snprintf(line,sizeof(line),"CLEAR %.20s",t->pps_clear); text(s,12,250,line,1,white);
        snprintf(line,sizeof(line),"RTC %.19s",t->rtc); text(s,12,278,line,1,muted);
    } else {
        text(s,12,12,"NTP AND NETWORK",2,accent);
        snprintf(line,sizeof(line),"NTP %s",t->ntp); text(s,12,54,line,1,accent);
        snprintf(line,sizeof(line),"REF %.28s",t->ref); text(s,12,78,line,1,white);
        if (t->have_stratum) snprintf(line,sizeof(line),"STRATUM %d",t->stratum);
        else snprintf(line,sizeof(line),"STRATUM UNAVAILABLE");
        text(s,12,102,line,1,white);
        snprintf(line,sizeof(line),"LEAP %.24s",t->leap); text(s,12,126,line,1,white);
        snprintf(line,sizeof(line),"OFFSET %.24s",t->last_offset); text(s,12,150,line,1,white);
        snprintf(line,sizeof(line),"RMS %.24s",t->rms_offset); text(s,12,174,line,1,white);
        snprintf(line,sizeof(line),"IP %.28s",t->ip); text(s,12,210,line,1,white);
        snprintf(line,sizeof(line),"GW %.28s",t->gateway); text(s,12,234,line,1,muted);
        snprintf(line,sizeof(line),"DNS %.26s",t->dns); text(s,12,258,line,1,muted);
        if (t->have_uptime) snprintf(line,sizeof(line),"UPTIME %.0fS",t->uptime);
        else snprintf(line,sizeof(line),"UPTIME UNAVAILABLE");
        text(s,12,286,line,1,accent);
    }
}

static void draw_setup(struct screen *s, int selected, int backlight, int rotation) {
    uint32_t bg=rgb(7,18,32,&s->var), panel=rgb(15,39,59,&s->var);
    uint32_t white=rgb(238,245,250,&s->var), accent=rgb(20,190,170,&s->var);
    uint32_t muted=rgb(140,170,190,&s->var);
    char line[64];
    rect(s,0,0,s->var.xres,s->var.yres,bg);
    rect(s,0,0,s->var.xres,42,panel);
    text(s,12,12,"DEVICE SETUP",2,accent);
    text(s,12,62,"LANGUAGE: ENGLISH",1,white);
    text(s,12,86,"LANGUAGE IS FIXED",1,muted);
    snprintf(line,sizeof(line),"BACKLIGHT: %s",backlight ? "ON" : "OFF");
    text(s,12,132,selected==0 ? "-" : " ",1,accent);
    text(s,28,132,line,1,selected==0 ? white : muted);
    snprintf(line,sizeof(line),"ROTATION: %d",rotation);
    text(s,12,166,selected==1 ? "-" : " ",1,accent);
    text(s,28,166,line,1,selected==1 ? white : muted);
    text(s,12,200,selected==2 ? "-" : " ",1,accent);
    text(s,28,200,"EXIT SETUP",1,selected==2 ? white : muted);
    text(s,12,274,"TOP/BOTTOM: MOVE",1,accent);
    text(s,12,292,"MIDDLE: SELECT",1,accent);
    text(s,12,310,"HOLD BOTTOM: EXIT",1,muted);
}

int main(void) {
    struct screen s={.fd=-1};
    s.fd=open("/dev/fb0",O_RDWR);
    if(s.fd<0){perror("open /dev/fb0");return 1;}
    if(ioctl(s.fd,FBIOGET_FSCREENINFO,&s.fix)||ioctl(s.fd,FBIOGET_VSCREENINFO,&s.var)){perror("framebuffer ioctl");return 1;}
    if(s.var.bits_per_pixel!=16 && s.var.bits_per_pixel!=24 && s.var.bits_per_pixel!=32){fprintf(stderr,"unsupported framebuffer depth %u\n",s.var.bits_per_pixel);return 1;}
    s.len=s.fix.smem_len;
    s.mem=mmap(NULL,s.len,PROT_READ|PROT_WRITE,MAP_SHARED,s.fd,0);
    if(s.mem==MAP_FAILED){perror("mmap");return 1;}

    int keyfd=open_keys(), page=0, setup=0, selected=0, backlight=1, rotation=0;
    int bottom_down=0;
    struct timespec bottom_start={0};
    time_t refresh_at=0;
    struct telemetry telemetry={0};
    if(keyfd<0) fprintf(stderr,"gpio-keys evdev device not found; display remains available without button navigation\n");

    for(;;) {
        if(keyfd<0) keyfd=open_keys();
        if(keyfd>=0) {
            struct pollfd pfd={.fd=keyfd,.events=POLLIN};
            if(poll(&pfd,1,100)>0 && (pfd.revents&POLLIN)) {
                struct input_event ev;
                while(read(keyfd,&ev,sizeof(ev))==(ssize_t)sizeof(ev)) {
                    if(ev.type!=EV_KEY) continue;
                    if(ev.code==KEY_DOWN && ev.value==1) {
                        bottom_down=1; clock_gettime(CLOCK_MONOTONIC,&bottom_start);
                    } else if(ev.code==KEY_DOWN && ev.value==0 && bottom_down) {
                        struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
                        long ms=(now.tv_sec-bottom_start.tv_sec)*1000L+
                                (now.tv_nsec-bottom_start.tv_nsec)/1000000L;
                        bottom_down=0;
                        if(ms>=1000) setup=!setup;
                        else if(setup) selected=(selected+1)%3;
                        else page=(page+1)%3;
                    } else if(ev.code==KEY_UP && ev.value==0) {
                        if(setup) selected=(selected+2)%3;
                        else page=(page+2)%3;
                    } else if(ev.code==KEY_ENTER && ev.value==0 && setup) {
                        if(selected==0) { backlight=!backlight; set_backlight(backlight); }
                        else if(selected==1) {
                            rotation=rotation ? 0 : 180;
                            s.var.rotate=rotation;
                            if(ioctl(s.fd,FBIOPUT_VSCREENINFO,&s.var)==0) {
                                ioctl(s.fd,FBIOGET_VSCREENINFO,&s.var);
                                ioctl(s.fd,FBIOGET_FSCREENINFO,&s.fix);
                            }
                        } else setup=0;
                    } else if(ev.code==KEY_ENTER && ev.value==0) {
                        page=(page+1)%3;
                    }
                }
            }
        } else usleep(100000);

        time_t now=time(NULL);
        if(now>=refresh_at) {
            get_shared_status(&telemetry);
            refresh_at=now+2;
        }
        if(setup) draw_setup(&s,selected,backlight,rotation);
        else draw_status(&s,page,&telemetry);
        usleep(100000);
    }
    return 0;
}

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
static void get_shared_status(char *fix, size_t fixn, char *utc, size_t utcn,
                              char *ntp, size_t ntpn, char *ref, size_t refn,
                              char *ip, size_t ipn) {
    snprintf(fix, fixn, "NO TELEMETRY");
    snprintf(utc, utcn, "UNAVAILABLE");
    snprintf(ntp, ntpn, "UNAVAILABLE");
    snprintf(ref, refn, "NONE");
    snprintf(ip, ipn, "NO ADDRESS");
    FILE *f = popen("/usr/sbin/yspeed-status 2>/dev/null", "r");
    if (!f) return;
    char json[8192];
    size_t used = fread(json, 1, sizeof(json) - 1, f);
    json[used] = '\0';
    pclose(f);
    json_string_value(json, "fix", fix, fixn);
    json_string_value(json, "utc", utc, utcn);
    json_string_value(json, "state", ntp, ntpn);
    json_string_value(json, "reference", ref, refn);
    json_string_value(json, "ipv4", ip, ipn);
}

static void get_ip(char *out, size_t n) {
    struct ifaddrs *all = NULL;
    snprintf(out,n,"NO DHCP ADDRESS");
    if (getifaddrs(&all)) return;
    for (struct ifaddrs *p=all; p; p=p->ifa_next) {
        if (!p->ifa_addr || strcmp(p->ifa_name,"eth0.2") || p->ifa_addr->sa_family != AF_INET) continue;
        if (inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, out, n)) break;
    }
    freeifaddrs(all);
}
static int json_number(const char *line, const char *key, double *v) {
    char k[32]; snprintf(k,sizeof(k),"\"%s\":",key);
    const char *p=strstr(line,k); if(!p) return 0;
    p+=strlen(k); while(*p==' ')p++;
    if(*p=='n') return 0;
    char *end=NULL; *v=strtod(p,&end); return end!=p;
}
static void get_gps(char *out, size_t n) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(out,n,"GPSD UNAVAILABLE"); return; }
    struct timeval tv = {.tv_sec=2,.tv_usec=0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    struct sockaddr_in addr = {.sin_family=AF_INET,.sin_port=htons(2947)};
    addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (connect(fd,(struct sockaddr *)&addr,sizeof(addr)) < 0) {
        close(fd); snprintf(out,n,"GPSD UNAVAILABLE"); return;
    }
    static const char watch[] = "?WATCH={\"enable\":true,\"json\":true};\n";
    if (write(fd,watch,sizeof(watch)-1) < 0) {
        close(fd); snprintf(out,n,"GPSD UNAVAILABLE"); return;
    }
    FILE *f=fdopen(fd,"r");
    if(!f){close(fd);snprintf(out,n,"GPSD UNAVAILABLE");return;}
    char line[512]; int found=0;
    while(fgets(line,sizeof(line),f)) {
        if(!strstr(line,"\"class\":\"TPV\"")) continue;
        double mode=0,lat=0,lon=0;
        json_number(line,"mode",&mode);
        if(mode>=2 && json_number(line,"lat",&lat) && json_number(line,"lon",&lon)) {
            snprintf(out,n,"FIX %.4f %.4f",lat,lon); found=1; break;
        }
    }
    fclose(f);
    if(!found) snprintf(out,n,"WAITING FOR GNSS FIX");
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

static void draw_status(struct screen *s, int page, const char *ip,
                        const char *fix, const char *utc,
                        const char *ntp, const char *ref) {
    uint32_t bg=rgb(7,18,32,&s->var), panel=rgb(15,39,59,&s->var);
    uint32_t white=rgb(238,245,250,&s->var), accent=rgb(20,190,170,&s->var);
    uint32_t muted=rgb(140,170,190,&s->var);
    char line[80], utc_short[24];
    time_t now=time(NULL); struct tm tmv; localtime_r(&now,&tmv);
    snprintf(utc_short,sizeof(utc_short),"%.19s",utc);
    rect(s,0,0,s->var.xres,s->var.yres,bg);
    rect(s,0,0,s->var.xres,42,panel);
    text(s,12,12,"YSPEED GNSS NTP",2,accent);
    if (page == 0) {
        text(s,12,52,"LOCAL SYSTEM TIME",1,muted);
        strftime(line,sizeof(line),"%H:%M:%S",&tmv); text(s,12,70,line,3,white);
        strftime(line,sizeof(line),"%Y-%m-%d",&tmv); text(s,12,104,line,2,white);
        rect(s,10,137,s->var.xres-20,2,accent);
        text(s,12,147,"ETHERNET",1,muted); text(s,12,163,ip,1,white);
        text(s,12,185,"GNSS FIX",1,muted); text(s,12,201,fix,2,white);
        text(s,12,228,"RECEIVER UTC",1,muted); text(s,12,244,utc_short,1,white);
        text(s,12,267,"CHRONY",1,accent); text(s,12,283,ntp,1,white);
        text(s,12,300,ref,1,muted);
    } else if (page == 1) {
        text(s,12,56,"GNSS STATUS",2,accent);
        text(s,12,94,"FIX MODE",1,muted); text(s,12,112,fix,2,white);
        text(s,12,154,"RECEIVER UTC",1,muted); text(s,12,172,utc_short,1,white);
        text(s,12,214,"SATELLITE FIX REQUIRES",1,muted);
        text(s,12,232,"A CONNECTED ANTENNA",1,muted);
        text(s,12,286,"UP/DOWN: PAGES",1,accent);
        text(s,12,302,"ENTER: SELECT",1,accent);
    } else {
        text(s,12,56,"NETWORK AND TIME",2,accent);
        text(s,12,94,"ETHERNET ADDRESS",1,muted); text(s,12,112,ip,1,white);
        text(s,12,154,"CHRONY STATE",1,muted); text(s,12,172,ntp,2,white);
        text(s,12,214,"REFERENCE",1,muted); text(s,12,232,ref,1,white);
        text(s,12,276,"LOCAL NTP SERVICE",1,accent);
        text(s,12,294,"UDP PORT 123",1,white);
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
    char ip[64]="NO ADDRESS",fix[32]="NO TELEMETRY",utc[64]="UNAVAILABLE";
    char ntp[32]="UNAVAILABLE",ref[32]="NONE";
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
            get_shared_status(fix,sizeof(fix),utc,sizeof(utc),ntp,sizeof(ntp),ref,sizeof(ref),ip,sizeof(ip));
            refresh_at=now+2;
        }
        if(setup) draw_setup(&s,selected,backlight,rotation);
        else draw_status(&s,page,ip,fix,utc,ntp,ref);
        usleep(100000);
    }
    return 0;
}

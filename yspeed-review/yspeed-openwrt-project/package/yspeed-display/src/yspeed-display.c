#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/fb.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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
    FILE *f=popen("timeout 2 gpspipe -w -n 20 2>/dev/null","r");
    if(!f){snprintf(out,n,"GPSD UNAVAILABLE");return;}
    char line[512]; int found=0;
    while(fgets(line,sizeof(line),f)) {
        if(!strstr(line,"\"class\":\"TPV\"")) continue;
        double mode=0,lat=0,lon=0;
        json_number(line,"mode",&mode);
        if(mode>=2 && json_number(line,"lat",&lat) && json_number(line,"lon",&lon)) {
            snprintf(out,n,"FIX %.4f %.4f",lat,lon); found=1; break;
        }
    }
    pclose(f);
    if(!found) snprintf(out,n,"WAITING FOR GNSS FIX");
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
    uint32_t bg=rgb(7,18,32,&s.var), panel=rgb(15,39,59,&s.var), white=rgb(238,245,250,&s.var), accent=rgb(20,190,170,&s.var), muted=rgb(140,170,190,&s.var);
    for(;;) {
        char ip[64],gps[96],line[64];
        time_t now=time(NULL); struct tm tmv; localtime_r(&now,&tmv);
        get_ip(ip,sizeof(ip)); get_gps(gps,sizeof(gps));
        rect(&s,0,0,s.var.xres,s.var.yres,bg);
        rect(&s,0,0,s.var.xres,42,panel);
        text(&s,12,12,"YSPEED GNSS",2,accent);
        text(&s,12,58,"LOCAL TIME",1,muted);
        strftime(line,sizeof(line),"%H:%M:%S",&tmv); text(&s,12,78,line,3,white);
        strftime(line,sizeof(line),"%Y-%m-%d",&tmv); text(&s,12,112,line,2,white);
        rect(&s,10,155,s.var.xres-20,2,accent);
        text(&s,12,170,"NETWORK ETH0.2",1,muted);
        text(&s,12,190,ip,1,white);
        text(&s,12,225,"GNSS STATUS",1,muted);
        text(&s,12,245,gps,1,white);
        text(&s,12,285,"NTP CHRONY",1,accent);
        usleep(2000000);
    }
    return 0;
}

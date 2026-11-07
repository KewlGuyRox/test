#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define W 240
#define H 320
#define SPI_HZ 40000000
#define GPIO_DC_OFFSET 16
#define GPIO_RESET_OFFSET 14
#define GPIO_BACKLIGHT_OFFSET 19

static const uint8_t font[40][5] = {
 {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},
 {0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},
 {0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
 {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},
 {0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},
 {0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
 {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},
 {0x3e,0x41,0x41,0x41,0x3e},{0x7f,0x09,0x09,0x09,0x06},
 {0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
 {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},
 {0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},
 {0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
 {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
 {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},
 {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
 {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
 {0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
 {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},
 {0,0,0,0,0},{0,0,0x14,0,0},{0,0x60,0x60,0,0},{0x08,0x08,0x08,0x08,0x08}
};

struct display { int spi, dc, reset, backlight; uint16_t *pixels; };

static int write_text(const char *path, const char *value)
{
    int fd=open(path,O_WRONLY);
    if(fd<0) return -1;
    ssize_t n=write(fd,value,strlen(value));
    close(fd);
    return n==(ssize_t)strlen(value)?0:-1;
}
static int gpio_number(int offset)
{
    DIR *d=opendir("/sys/class/gpio");
    if(!d) return -1;
    struct dirent *e; int result=-1;
    while((e=readdir(d))) {
        if(strncmp(e->d_name,"gpiochip",8)) continue;
        char p[256], label[128]=""; int base=-1, ngpio=0;
        snprintf(p,sizeof(p),"/sys/class/gpio/%s/base",e->d_name);
        FILE *f=fopen(p,"r"); if(f){fscanf(f,"%d",&base);fclose(f);}
        snprintf(p,sizeof(p),"/sys/class/gpio/%s/ngpio",e->d_name);
        f=fopen(p,"r"); if(f){fscanf(f,"%d",&ngpio);fclose(f);}
        snprintf(p,sizeof(p),"/sys/class/gpio/%s/label",e->d_name);
        f=fopen(p,"r"); if(f){fgets(label,sizeof(label),f);fclose(f);}
        if(base>=0 && ngpio>offset) {
            if(result<0) result=base+offset;
            if(strstr(label,"10000600") || strstr(label,"mt7628")) {
                result=base+offset;
                break;
            }
        }
    }
    closedir(d);
    return result;
}
static int gpio_setup(int offset, int *number, int initial)
{
    *number=gpio_number(offset);
    if(*number<0) { fprintf(stderr,"No gpiochip covers GPIO offset %d\n",offset); return -1; }
    char p[128],v[32];
    snprintf(p,sizeof(p),"/sys/class/gpio/gpio%d",*number);
    if(access(p,F_OK)!=0) {
        snprintf(v,sizeof(v),"%d",*number);
        if(write_text("/sys/class/gpio/export",v)<0 && errno!=EBUSY) return -1;
        usleep(20000);
    }
    snprintf(p,sizeof(p),"/sys/class/gpio/gpio%d/direction",*number);
    if(write_text(p,"out")<0) return -1;
    snprintf(p,sizeof(p),"/sys/class/gpio/gpio%d/value",*number);
    snprintf(v,sizeof(v),"%d",initial);
    return write_text(p,v);
}
static int gpio_set(int number, int value)
{
    char p[128],v[4];
    snprintf(p,sizeof(p),"/sys/class/gpio/gpio%d/value",number);
    snprintf(v,sizeof(v),"%d",value);
    return write_text(p,v);
}
static int spi_write(struct display *d, const uint8_t *data, size_t len)
{
    while(len) {
        size_t n=len>4096?4096:len;
        ssize_t w=write(d->spi,data,n);
        if(w<0) { if(errno==EINTR) continue; return -1; }
        data+=w; len-=(size_t)w;
    }
    return 0;
}
static int command(struct display *d, uint8_t cmd, const uint8_t *data, size_t len)
{
    if(gpio_set(d->dc,0)<0 || spi_write(d,&cmd,1)<0) return -1;
    if(len && (gpio_set(d->dc,1)<0 || spi_write(d,data,len)<0)) return -1;
    return 0;
}
static int panel_init(struct display *d)
{
    if(gpio_set(d->backlight,0)<0) return -1;
    gpio_set(d->reset,0); usleep(30000);
    gpio_set(d->reset,1); usleep(150000);
    if(command(d,0x01,NULL,0)<0) return -1; usleep(150000); /* SWRESET */
    if(command(d,0x11,NULL,0)<0) return -1; usleep(120000); /* SLPOUT */
    const uint8_t colmod=0x55, madctl=0x08;
    const uint8_t cols[]={0x00,0x00,0x00,0xef};
    const uint8_t rows[]={0x00,0x00,0x01,0x3f};
    const uint8_t porch[]={0x0c,0x0c,0x00,0x33,0x33};
    if(command(d,0x3a,&colmod,1)<0 || command(d,0x36,&madctl,1)<0 ||
       command(d,0x2a,cols,sizeof(cols))<0 || command(d,0x2b,rows,sizeof(rows))<0 ||
       command(d,0xb2,porch,sizeof(porch))<0 || command(d,0x21,NULL,0)<0 ||
       command(d,0x13,NULL,0)<0 || command(d,0x29,NULL,0)<0) return -1;
    usleep(100000);
    return gpio_set(d->backlight,1);
}
static void pixel(struct display *d,int x,int y,uint16_t c)
{
    if(x>=0&&x<W&&y>=0&&y<H) d->pixels[y*W+x]=c;
}
static void rect(struct display *d,int x,int y,int w,int h,uint16_t c)
{
    for(int yy=y;yy<y+h;yy++) for(int xx=x;xx<x+w;xx++) pixel(d,xx,yy,c);
}
static int glyph(char c)
{
    if(c>='a'&&c<='z') c=(char)(c-'a'+'A');
    if(c>='A'&&c<='Z') return c-'A';
    if(c>='0'&&c<='9') return 26+c-'0';
    if(c==' ') return 36; if(c==':') return 37; if(c=='.') return 38; if(c=='-') return 39;
    return 36;
}
static void draw_text(struct display *d,int x,int y,const char *s,int scale,uint16_t color)
{
    for(;*s;s++,x+=6*scale) {
        int g=glyph(*s);
        for(int col=0;col<5;col++) for(int row=0;row<7;row++)
            if(font[g][col]&(1u<<row))
                rect(d,x+col*scale,y+row*scale,scale,scale,color);
    }
}
static int flush(struct display *d)
{
    const uint8_t cols[]={0,0,0,(uint8_t)(W-1)};
    const uint8_t rows[]={0,0,(uint8_t)((H-1)>>8),(uint8_t)(H-1)};
    if(command(d,0x2a,cols,4)<0 || command(d,0x2b,rows,4)<0 ||
       command(d,0x2c,NULL,0)<0 || gpio_set(d->dc,1)<0) return -1;
    uint8_t chunk[4096]; size_t total=W*H, pos=0;
    while(pos<total) {
        size_t count=total-pos; if(count>sizeof(chunk)/2) count=sizeof(chunk)/2;
        for(size_t i=0;i<count;i++) {
            uint16_t p=d->pixels[pos+i];
            chunk[2*i]=(uint8_t)(p>>8); chunk[2*i+1]=(uint8_t)p;
        }
        if(spi_write(d,chunk,count*2)<0) return -1;
        pos+=count;
    }
    return 0;
}
static void json_value(const char *json,const char *key,char *out,size_t n)
{
    char needle[64]; snprintf(needle,sizeof(needle),"\"%s\":\"",key);
    const char *p=strstr(json,needle);
    if(!p){snprintf(out,n,"UNAVAILABLE");return;}
    p+=strlen(needle); size_t i=0;
    while(*p&&*p!='"'&&i+1<n) { if(*p=='\\'&&p[1])p++; out[i++]=*p++; }
    out[i]=0;
    if(!i) snprintf(out,n,"UNAVAILABLE");
}
static void status(char *ip,size_t ni,char *fix,size_t nf,char *utc,size_t nu,
                   char *ntp,size_t nn,char *ref,size_t nr)
{
    char json[8192]="";
    FILE *f=popen("/usr/sbin/yspeed-status 2>/dev/null","r");
    if(f){size_t n=fread(json,1,sizeof(json)-1,f);json[n]=0;pclose(f);}
    json_value(json,"ipv4",ip,ni); json_value(json,"fix",fix,nf);
    json_value(json,"utc",utc,nu); json_value(json,"state",ntp,nn);
    json_value(json,"reference",ref,nr);
}
static int open_keys(void)
{
    char p[64];
    for(int i=0;i<16;i++) {
        snprintf(p,sizeof(p),"/dev/input/event%d",i);
        int fd=open(p,O_RDONLY|O_NONBLOCK); if(fd<0)continue;
        unsigned long bits[(KEY_MAX+sizeof(unsigned long)*8)/(sizeof(unsigned long)*8)]={0};
        if(ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(bits)),bits)>=0 &&
           (bits[KEY_UP/(sizeof(unsigned long)*8)]&(1UL<<(KEY_UP%(sizeof(unsigned long)*8)))) &&
           (bits[KEY_DOWN/(sizeof(unsigned long)*8)]&(1UL<<(KEY_DOWN%(sizeof(unsigned long)*8)))) &&
           (bits[KEY_ENTER/(sizeof(unsigned long)*8)]&(1UL<<(KEY_ENTER%(sizeof(unsigned long)*8))))) return fd;
        close(fd);
    }
    return -1;
}
static void draw_page(struct display *d,int page)
{
    char ip[64],fix[64],utc[64],ntp[64],ref[64],line[64];
    status(ip,sizeof(ip),fix,sizeof(fix),utc,sizeof(utc),ntp,sizeof(ntp),ref,sizeof(ref));
    const uint16_t bg=0x08a3, panel=0x19e7, white=0xef7d, aqua=0x16d5, muted=0x8d96;
    rect(d,0,0,W,H,bg); rect(d,0,0,W,42,panel);
    draw_text(d,12,12,"YSPEED GNSS NTP",2,aqua);
    if(page==0) {
        time_t t=time(NULL); struct tm tmv; localtime_r(&t,&tmv);
        draw_text(d,12,54,"LOCAL SYSTEM TIME",1,muted);
        strftime(line,sizeof(line),"%H:%M:%S",&tmv); draw_text(d,12,72,line,3,white);
        strftime(line,sizeof(line),"%Y-%m-%d",&tmv); draw_text(d,12,108,line,2,white);
        rect(d,10,140,W-20,2,aqua);
        draw_text(d,12,152,"ETHERNET",1,muted); draw_text(d,12,168,ip,1,white);
        draw_text(d,12,194,"GNSS FIX",1,muted); draw_text(d,12,210,fix,1,white);
        draw_text(d,12,240,"RECEIVER UTC",1,muted); draw_text(d,12,256,utc,1,white);
        draw_text(d,12,286,"CHRONY",1,aqua); draw_text(d,70,286,ntp,1,white);
    } else if(page==1) {
        draw_text(d,12,58,"GNSS STATUS",2,aqua);
        draw_text(d,12,100,"FIX MODE",1,muted); draw_text(d,12,118,fix,1,white);
        draw_text(d,12,158,"RECEIVER UTC",1,muted); draw_text(d,12,176,utc,1,white);
        draw_text(d,12,230,"ANTENNA REQUIRED",1,aqua);
        draw_text(d,12,250,"FOR SATELLITE FIX",1,aqua);
    } else {
        draw_text(d,12,58,"NETWORK AND TIME",2,aqua);
        draw_text(d,12,100,"ETHERNET ADDRESS",1,muted); draw_text(d,12,118,ip,1,white);
        draw_text(d,12,158,"CHRONY STATE",1,muted); draw_text(d,12,176,ntp,1,white);
        draw_text(d,12,216,"REFERENCE",1,muted); draw_text(d,12,234,ref,1,white);
        draw_text(d,12,284,"LOCAL NTP UDP 123",1,aqua);
    }
    draw_text(d,12,306,"UP/DOWN: PAGES",1,muted);
    if(flush(d)<0) fprintf(stderr,"SPI display frame transfer failed: %s\n",strerror(errno));
}
int main(void)
{
    struct display d={.spi=-1,.dc=-1,.reset=-1,.backlight=-1};
    int dcno,rsno,blno;
    if(gpio_setup(GPIO_DC_OFFSET,&dcno,0)<0 ||
       gpio_setup(GPIO_RESET_OFFSET,&rsno,1)<0 ||
       gpio_setup(GPIO_BACKLIGHT_OFFSET,&blno,0)<0) {
        fprintf(stderr,"Unable to export/configure ST7789 GPIO lines\n"); return 1;
    }
    d.dc=dcno; d.reset=rsno; d.backlight=blno;
    d.spi=open("/dev/spidev0.1",O_WRONLY);
    if(d.spi<0){perror("open /dev/spidev0.1");return 1;}
    uint8_t mode=SPI_MODE_0,bits=8; uint32_t speed=SPI_HZ;
    if(ioctl(d.spi,SPI_IOC_WR_MODE,&mode)<0 ||
       ioctl(d.spi,SPI_IOC_WR_BITS_PER_WORD,&bits)<0 ||
       ioctl(d.spi,SPI_IOC_WR_MAX_SPEED_HZ,&speed)<0) {
        perror("configure SPI"); return 1;
    }
    d.pixels=calloc(W*H,sizeof(uint16_t));
    if(!d.pixels){perror("calloc");return 1;}
    if(panel_init(&d)<0){fprintf(stderr,"ST7789 initialization failed\n");return 1;}
    int keys=open_keys(),page=0;
    draw_page(&d,page);
    for(;;) {
        if(keys<0) keys=open_keys();
        if(keys>=0) {
            struct pollfd pfd={.fd=keys,.events=POLLIN};
            if(poll(&pfd,1,1800)>0 && (pfd.revents&POLLIN)) {
                struct input_event ev;
                while(read(keys,&ev,sizeof(ev))==(ssize_t)sizeof(ev))
                    if(ev.type==EV_KEY&&ev.value==0) {
                        if(ev.code==KEY_UP) page=(page+2)%3;
                        if(ev.code==KEY_DOWN||ev.code==KEY_ENTER) page=(page+1)%3;
                    }
            }
        } else sleep(2);
        draw_page(&d,page);
    }
    return 0;
}

#include <vlibc/vgfx.h>
#include <vlibc/keys.h>
#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/stdlib.h>
#include <vlibc/socket.h>
#include <vlibc/netdb.h>
#include <vlibc/stdio.h>
static char* bchr(char* s,char c){while(*s){if(*s==c)return s;++s;}return 0;}
static char* bbstr(char* s,const char* n){int nl=strlen(n);if(!nl)return s;while(*s){int ok=1;for(int i=0;i<nl;++i){if(!s[i]||s[i]!=n[i]){ok=0;break;}}if(ok)return s;++s;}return 0;}

#define BAR_H 20
#define BUF_SZ 65536
#define MAX_LINKS 128

typedef struct {
    int x,y,w,h;
    char url[256];
} Link;

static Link links[MAX_LINKS];
static int nlinks;

static void bsprintf(char* out,int cap,const char* fmt,...){
    (void)cap;(void)fmt;
    out[0]=0;
}
static int parse_url(const char* s, char* host, int hcap, uint16_t* port, char* path, int pcap) {
    const char* p = s;
    if (strncmp(p,"http://",7)==0) p+=7;
    else if (strncmp(p,"https://",8)==0) { /* no tls yet */ p+=8; }
    int i=0;
    while (*p && *p!=':' && *p!='/' && i<hcap-1) host[i++]=*p++;
    host[i]=0;
    *port = 80;
    if (*p==':') {
        p++; int pr=0;
        while (*p>='0'&&*p<='9') { pr=pr*10+(*p-'0'); p++; }
        if (pr) *port=(uint16_t)pr;
    }
    i=0; if (*p=='/') { while (*p && i<pcap-1) path[i++]=*p++; } else { path[0]='/'; path[1]=0; }
    path[i]=0; return 0;
}

static int http_get(const char* host,uint16_t port,const char* path,char* buf,int cap) {
    uint32_t ip=0;
    if (gethostbyname(host,&ip)<0) return -1;
    int s = vnu_socket(2,1); /* AF_INET, SOCK_STREAM */
    if (s<0) return -1;
    if (vnu_connect(s,ip,port,5000)<0) { vnu_netclose(s); return -1; }
    char req[512];
    bsprintf(req,sizeof(req),"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",path,host);
    vnu_send(s,req,strlen(req),5000);
    int total=0;
    while (total < cap) {
        int r = vnu_recv(s,buf+total,cap-total-1,5000);
        if (r<=0) break;
        total+=r;
    }
    buf[total]=0;
    vnu_netclose(s);
    return total;
}

static char* skip_headers(char* s) {
    char* p=s;
    while (*p) {
        if (p[0]=='\r'&&p[1]=='\n'&&p[2]=='\r'&&p[3]=='\n') return p+4;
        if (p[0]=='\n'&&p[1]=='\n') return p+2;
        p++;
    }
    return s;
}

static void render_text(char* html, int y) {
    char* p=html;
    nlinks=0;
    int x=4, row=y;
    while (*p && row < VGFX_H-BAR_H) {
        if (*p=='<' && p[1]=='a' && p[2]==' ') {
            /* crude link <a href="...">text</a> */
            char* href=bbstr(p,"href=\"");
            if (href) {
                href+=6; char* q=bchr(href,'"'); if (q) {
                    int hl=q-href; if (hl<250 && nlinks<MAX_LINKS) {
                        memcpy(links[nlinks].url,href,hl); links[nlinks].url[hl]=0;
                        char* end=bbstr(q,"</a>"); char* txt=q+1;
                        int txl = end? (end-txt):0;
                        if (txl<200) {
                            char tmp[256]; memcpy(tmp,txt,txl); tmp[txl]=0;
                            int tw = vgfx_text_width(tmp);
                            links[nlinks].x=x; links[nlinks].y=row; links[nlinks].w=tw; links[nlinks].h=16;
                            vgfx_str(x,row,tmp,VGFX_LBLUE);
                            x+=tw+4; nlinks++;
                        }
                        if (end) p=end+4; else break;
                        continue;
                    }
                }
            }
        }
        if (*p=='\n') { row+=16; x=4; p++; continue; }
        if (*p=='\r') { p++; continue; }
        if (*p=='<' || *p=='&') { p++; continue; }
        char c=*p++; vgfx_char(x,row,c,VGFX_WHITE); x+=8;
        if (x>VGFX_W-8) { row+=16; x=4; }
    }
}

int main() {
    char url[256]="http://example.com/";
    char host[128],path[256];
    uint16_t port=80;
    char buf[BUF_SZ];
    int focus=1; /* 1=url */
    int cur=7;
    for (int i=0;i<(int)strlen(url);i++) if (url[i]) cur=i+1;
    buf[0]=0;
    for (;;) {
        vgfx_clear(VGFX_BLACK);
        vgfx_fill_rect(0,0,VGFX_W,BAR_H,VGFX_DGRAY);
        char disp[280]; bsprintf(disp,sizeof(disp),"> %s",url);
        vgfx_str(4,2,disp,VGFX_WHITE);
        if (focus==1) vgfx_rect(4+8*(cur>0?cur:1)-1,1,9,17,VGFX_YELLOW);
        if (buf[0]) {
            char* body = skip_headers(buf);
            render_text(body, BAR_H+4);
        } else {
            vgfx_str(4,BAR_H+4,"Enter URL and press Enter",VGFX_LGRAY);
        }
        int k=0;
        vgfx_flush();
        k = read(0,0,0);
        if (k==27) break;
        if (focus==1) {
            if (k=='\n') {
                parse_url(url,host,sizeof(host),&port,path,sizeof(path));
                http_get(host,port,path,buf,BUF_SZ);
                focus=0;
            } else if (k==8 && cur>1) { cur--; url[cur]=0; }
            else if (k>=32 && k<127 && cur<(int)sizeof(url)-2) { url[cur++]=k; url[cur]=0; }
        } else {
            if (k=='\t') { focus=1; }
            if (k=='\n' || k==' ') { focus=1; }
        }
    }
    return 0;
}

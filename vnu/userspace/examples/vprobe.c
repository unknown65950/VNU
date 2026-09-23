#include <vlibc/socket.h>
#include <vlibc/stdio.h>
#include <vlibc/stdlib.h>
#include <vlibc/string.h>
#include <vlibc/unistd.h>
#include <stdint.h>

static long parse_ip(const char* s, unsigned long* out)
{
    unsigned long oct[4];
    int n = 0;
    unsigned long v = 0;
    int have = 0;
    for (const char* p = s;; ++p) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned long)(*p - '0');
            have = 1;
        } else if (*p == '.' && have && n < 3) {
            if (v > 255)
                return -1;
            oct[n++] = v;
            v = 0;
            have = 0;
        } else if (*p == '\0' && have && n == 3) {
            if (v > 255)
                return -1;
            oct[3] = v;
            *out = ((oct[0] << 24) | (oct[1] << 16) | (oct[2] << 8) | oct[3]);
            return 0;
        } else {
            return -1;
        }
    }
}

int main(int argc, char** argv)
{
    unsigned long ip;
    long port;
    long tmo;
    if (argc < 3 || parse_ip(argv[1], &ip) != 0) {
        puts("usage: vprobe IP PORT [TMO_MS]");
        return 1;
    }
    port = atol(argv[2]);
    tmo = argc >= 4 ? atol(argv[3]) : 8000;

    long s = vnu_socket(VNU_AF_INET, VNU_SOCK_STREAM);
    if (s < 0) {
        printf("socket: %ld\n", s);
        return 1;
    }
    long rc = vnu_connect(s, (uint32_t)ip, (uint16_t)port, (uint32_t)tmo);
    if (rc < 0) {
        printf("connect: %ld\n", rc);
        vnu_netclose(s);
        return 1;
    }
    puts("connected");

    static const char req[] = "hello vnu\r\n";
    rc = vnu_send(s, req, (uint32_t)(sizeof(req) - 1), (uint32_t)tmo);
    printf("send: %ld\n", rc);

    uint8_t buf[512];
    for (;;) {
        long n = vnu_recv(s, buf, (uint32_t)sizeof(buf), (uint32_t)tmo);
        if (n < 0) {
            printf("recv: %ld\n", n);
            break;
        }
        if (n == 0) {
            puts("[eof]");
            break;
        }
        for (long i = 0; i < n; ++i)
            putchar((int)buf[i]);
    }
    vnu_netclose(s);
    return 0;
}
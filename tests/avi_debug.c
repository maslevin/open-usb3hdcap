#include "hdcap.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
/* usage: avi_debug [cpld|csc|none] : after lock, poke one thing, then poll the AVI registers */
int main(int argc, char **argv)
{
    hdcap *d; hdcap_signal s; hdcap_avi a; uint8_t v;
    const char *what = argc > 1 ? argv[1] : "none";
    if (hdcap_open(&d) < 0) return 1;
    hdcap_init(d); hdcap_select_input(d, HDCAP_INPUT_DVI);
    printf("%s: lock %d\n", what, hdcap_wait_signal(d, HDCAP_INPUT_DVI, 5000, &s));
    if (!strcmp(what, "cpld")) { hdcap_i2c_read(d, 0x98, 0x20, &v); hdcap_i2c_write(d, 0x98, 0x20, v); }
    if (!strcmp(what, "csc"))  { hdcap_i2c_write(d, 0x9c, 0x00, 0x00); hdcap_i2c_write(d, 0x9c, 0x93, 0x10); }
    for (int i = 0; i < 5; i++) {
        if (!strcmp(what, "ack")) { hdcap_i2c_write(d, 0x9c, 0x00, 0x02); hdcap_i2c_read(d, 0x9c, 0x0b, &v); usleep(100000); }
        hdcap_read_avi(d, &a);
        printf("  %4d ms avi %02x %02x %02x %02x\n", i * 500, a.pb[0], a.pb[1], a.pb[2], a.pb[3]);
        usleep(500000);
    }
    hdcap_close(d); return 0;
}

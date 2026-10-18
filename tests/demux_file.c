/* Feed a raw reassembled isochronous payload file through the demuxer and report what it finds. */
#include "hdcap.h"
#include <stdio.h>
#include <stdlib.h>

static int saved;
static void on_frame(const hdcap_frame *f, void *u)
{
    (void)u;
    if (saved < 1 && f->index > 3) {
        uint8_t *rgb = malloc((size_t)f->width * f->height * 3);
        hdcap_yuyv_to_rgb24(f->yuyv, rgb, f->width, f->height);
        FILE *o = fopen("tests/out_frame.ppm", "wb");
        fprintf(o, "P6\n%d %d\n255\n", f->width, f->height);
        fwrite(rgb, 1, (size_t)f->width * f->height * 3, o);
        fclose(o);
        free(rgb);
        printf("frame %llu: %dx%d interlaced=%d -> tests/out_frame.ppm\n", (unsigned long long)f->index, f->width, f->height, f->interlaced);
        saved++;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *in = fopen(argv[1], "rb");
    long limit = argc > 2 ? atol(argv[2]) : 0;
    hdcap_demux *dx = hdcap_demux_new(on_frame, NULL, NULL);
    static uint8_t buf[16384];
    size_t n; long total = 0;
    while ((n = fread(buf, 1, 16384, in)) > 0 && (!limit || total < limit)) { hdcap_demux_feed(dx, buf, n); total += n; }
    hdcap_demux_stats s; hdcap_demux_get_stats(dx, &s);
    printf("bytes %llu lines %llu frames %llu dropped %llu resyncs %llu audio_records %llu audio_frames %llu (%.4f/line) last_line %d B\n",
           (unsigned long long)s.bytes, (unsigned long long)s.video_lines, (unsigned long long)s.frames, (unsigned long long)s.dropped_frames,
           (unsigned long long)s.resyncs, (unsigned long long)s.audio_records, (unsigned long long)s.audio_frames,
           (double)s.audio_frames / s.video_lines, s.last_line_bytes);
    hdcap_demux_free(dx);
    return 0;
}

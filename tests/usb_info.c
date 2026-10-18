/* Print link speed and the interface/alt-setting/endpoint layout of the USB3HDCAP. */
#include <libusb.h>
#include <stdio.h>
int main(void)
{
    libusb_context *ctx; libusb_init(&ctx);
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, 0x1164, 0xF533);
    if (!h) { printf("not found\n"); return 1; }
    libusb_device *d = libusb_get_device(h);
    static const char *sp[] = { "unknown", "low (1.5M)", "full (12M)", "high (480M)", "super (5G)", "super+ (10G)" };
    int s = libusb_get_device_speed(d);
    printf("speed: %s\n", s >= 0 && s <= 5 ? sp[s] : "?");
    struct libusb_config_descriptor *c;
    if (libusb_get_active_config_descriptor(d, &c) && libusb_get_config_descriptor(d, 0, &c)) { printf("no config\n"); return 1; }
    for (int i = 0; i < c->bNumInterfaces; i++)
        for (int a = 0; a < c->interface[i].num_altsetting; a++) {
            const struct libusb_interface_descriptor *id = &c->interface[i].altsetting[a];
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                printf("IF%d alt%d ep %02x type %d wMaxPacketSize 0x%04x interval %d -> max alt packet %d\n", i, a, ep->bEndpointAddress,
                       ep->bmAttributes & 3, ep->wMaxPacketSize, ep->bInterval, libusb_get_max_alt_packet_size(d, i, a, ep->bEndpointAddress));
            }
        }
    libusb_free_config_descriptor(c); libusb_close(h); libusb_exit(ctx); return 0;
}

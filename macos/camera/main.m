/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/* Camera extension entry point: register the provider with CoreMediaIO and run. */
#import "CameraExtension.h"

int main(void)
{
    @autoreleasepool {
        g_cam_log = os_log_create("org.open-usb3hdcap.camera", "extension");
        CamProviderSource *ps = [[CamProviderSource alloc] initWithClientQueue:nil];
        [CMIOExtensionProvider startServiceWithProvider:ps.provider];
        os_log(g_cam_log, "camera extension started");
        CFRunLoopRun();
    }
    return 0;
}

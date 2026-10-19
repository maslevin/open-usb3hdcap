/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/* render-svg IN.svg OUT.png SIZE: rasterize an SVG with AppKit onto a transparent canvas (Quick Look
 * thumbnails are composited on white, which leaves white corners around the icon tile). */
#import <AppKit/AppKit.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 4) { fprintf(stderr, "usage: render-svg IN.svg OUT.png SIZE\n"); return 2; }
        NSImage *img = [[NSImage alloc] initWithContentsOfFile:@(argv[1])];
        int size = atoi(argv[3]);
        if (!img || size <= 0) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:size pixelsHigh:size
            bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
            bytesPerRow:0 bitsPerPixel:0];
        rep.size = NSMakeSize(size, size);
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [NSColor.clearColor set];
        NSRectFill(NSMakeRect(0, 0, size, size));
        [img drawInRect:NSMakeRect(0, 0, size, size) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];
        [NSGraphicsContext restoreGraphicsState];
        NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        if (![png writeToFile:@(argv[2]) atomically:YES]) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    }
    return 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * HDCapFrameSource: video client of hdcapd. Delivers every captured frame scaled (letterboxed)
 * into a CVPixelBuffer of the requested output size, and synthesises black frames at the output
 * rate while there is no signal so consumers never stall. Used by the camera extension and by
 * frame-source-test.
 */
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/* hostTimeNs: capture time on the host clock (mach_absolute_time in ns). synthetic: no-signal filler. */
typedef void (^HDCapFrameHandler)(CVPixelBufferRef buffer, uint64_t hostTimeNs, BOOL synthetic);
typedef void (^HDCapStatusHandler)(NSDictionary<NSString *, id> *status);

@interface HDCapFrameSource : NSObject
- (instancetype)initWithWidth:(int)width height:(int)height fps:(int)fps;
@property (nonatomic, readonly) int width, height, fps;
@property (nonatomic, copy, nullable) HDCapFrameHandler frameHandler;
@property (nonatomic, copy, nullable) HDCapStatusHandler statusHandler;
- (void)start;
- (void)stop;
/* Change output geometry (e.g. the client picked another format); safe while running. */
- (void)setWidth:(int)width height:(int)height fps:(int)fps;
@end

NS_ASSUME_NONNULL_END

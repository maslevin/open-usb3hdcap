/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/* Camera extension classes (CameraExtension.m); main.m starts the service, camera-harness drives
 * them in-process. */
#import <CoreMediaIO/CMIOExtension.h>
#import <os/log.h>

NS_ASSUME_NONNULL_BEGIN

typedef struct { int w, h, fps; } CamMode;
extern const CamMode kCamModes[];
extern const size_t kCamModeCount;
extern os_log_t g_cam_log;

@class CamStreamSource;

@interface CamDeviceSource : NSObject <CMIOExtensionDeviceSource>
@property (nonatomic, strong) CMIOExtensionDevice *device;
@property (nonatomic, strong) CamStreamSource *streamSource;
- (instancetype)initWithLocalizedName:(NSString *)name;
@end

@interface CamStreamSource : NSObject <CMIOExtensionStreamSource>
@property (nonatomic, strong) CMIOExtensionStream *stream;
@property (nonatomic, weak) CamDeviceSource *deviceSource;
@property (nonatomic) NSUInteger activeFormatIndex;
- (instancetype)initWithLocalizedName:(NSString *)name streamID:(NSUUID *)sid;
@end

@interface CamProviderSource : NSObject <CMIOExtensionProviderSource>
@property (nonatomic, strong) CMIOExtensionProvider *provider;
@property (nonatomic, strong) CamDeviceSource *deviceSource;
- (instancetype)initWithClientQueue:(nullable dispatch_queue_t)queue;
@end

NS_ASSUME_NONNULL_END

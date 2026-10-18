/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * USB3HDCAP Core Audio HAL plug-in (AudioServerPlugIn, loaded by coreaudiod).
 *
 * One input device ("USB3HDCAP", 48 kHz stereo). While IO runs, the plug-in is an audio client of
 * hdcapd (macos/common/hdcapd_protocol.h): it maps the daemon's shared ring buffer and reads from
 * it about 100 ms behind the write position, converting int16 to float32. Without the daemon it
 * produces silence. Timing follows the host clock (zero timestamps every RING_PERIOD frames);
 * device/host clock drift is absorbed by resynchronising the read position when it leaves a window.
 *
 * Object IDs: 1 = plug-in (kAudioObjectPlugInObject), 2 = device, 3 = input stream.
 */
#include "../common/hdcapd_protocol.h"

#include <CoreAudio/AudioServerPlugIn.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <xpc/xpc.h>

#include <os/lock.h>
#include <os/log.h>
#include <stdatomic.h>
#include <string.h>

#define OBJ_PLUGIN  kAudioObjectPlugInObject
#define OBJ_DEVICE  2
#define OBJ_STREAM  3

#define SAMPLE_RATE   48000.0
#define CHANNELS      2
#define RING_PERIOD   16384          /* zero-timestamp period (frames) */
#define READ_LATENCY  4800           /* read this far behind the daemon's write position */
#define DEVICE_UID    "org.open-usb3hdcap.hal.device"
#define MODEL_UID     "org.open-usb3hdcap.hal.model"

static os_log_t g_log;
static AudioServerPlugInHostRef g_host;
static UInt32 g_refcount = 1;
static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;

/* IO state */
static UInt32 g_io_clients;
static UInt64 g_anchor_host, g_timestamps;
static Float64 g_ticks_per_frame;
static xpc_connection_t g_conn;
static const hdcapd_audio_ring *_Atomic g_ring;
static int64_t g_offset;             /* ring frame = sample time + offset */
static int g_offset_valid;
static uint32_t g_generation;

static HRESULT HAL_QueryInterface(void *self, REFIID iid, LPVOID *out);
static ULONG HAL_AddRef(void *self);
static ULONG HAL_Release(void *self);
static OSStatus HAL_Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host);
static OSStatus HAL_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *out);
static OSStatus HAL_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id);
static OSStatus HAL_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *ci);
static OSStatus HAL_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *ci);
static OSStatus HAL_PerformConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info);
static OSStatus HAL_AbortConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info);
static Boolean HAL_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a);
static OSStatus HAL_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out);
static OSStatus HAL_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qsz, const void *q, UInt32 *out);
static OSStatus HAL_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qsz, const void *q, UInt32 insz, UInt32 *outsz, void *out);
static OSStatus HAL_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qsz, const void *q, UInt32 sz, const void *data);
static OSStatus HAL_StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client);
static OSStatus HAL_StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client);
static OSStatus HAL_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, Float64 *st, UInt64 *ht, UInt64 *seed);
static OSStatus HAL_WillDoIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, Boolean *will, Boolean *inplace);
static OSStatus HAL_BeginIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *ci);
static OSStatus HAL_DoIO(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *ci, void *main, void *secondary);
static OSStatus HAL_EndIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *ci);

static AudioServerPlugInDriverInterface g_vtable = {
    NULL, HAL_QueryInterface, HAL_AddRef, HAL_Release, HAL_Initialize, HAL_CreateDevice, HAL_DestroyDevice,
    HAL_AddDeviceClient, HAL_RemoveDeviceClient, HAL_PerformConfigChange, HAL_AbortConfigChange, HAL_HasProperty,
    HAL_IsPropertySettable, HAL_GetPropertyDataSize, HAL_GetPropertyData, HAL_SetPropertyData, HAL_StartIO, HAL_StopIO,
    HAL_GetZeroTimeStamp, HAL_WillDoIO, HAL_BeginIO, HAL_DoIO, HAL_EndIO
};
static AudioServerPlugInDriverInterface *g_vtable_ptr = &g_vtable;
static AudioServerPlugInDriverRef g_driver = &g_vtable_ptr;

/* ---------- factory / COM ---------- */

void *HDCAP_HAL_Create(CFAllocatorRef alloc, CFUUIDRef type);
void *HDCAP_HAL_Create(CFAllocatorRef alloc, CFUUIDRef type)
{
    (void)alloc;
    if (!CFEqual(type, kAudioServerPlugInTypeUUID))
        return NULL;
    g_log = os_log_create("org.open-usb3hdcap.hal", "plugin");
    return g_driver;
}

static HRESULT HAL_QueryInterface(void *self, REFIID iid, LPVOID *out)
{
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        os_unfair_lock_lock(&g_lock);
        g_refcount++;
        os_unfair_lock_unlock(&g_lock);
        *out = g_driver;
        r = S_OK;
    }
    CFRelease(req);
    (void)self;
    return r;
}
static ULONG HAL_AddRef(void *self) { (void)self; os_unfair_lock_lock(&g_lock); ULONG r = ++g_refcount; os_unfair_lock_unlock(&g_lock); return r; }
static ULONG HAL_Release(void *self) { (void)self; os_unfair_lock_lock(&g_lock); ULONG r = g_refcount ? --g_refcount : 0; os_unfair_lock_unlock(&g_lock); return r; }

static OSStatus HAL_Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host)
{
    (void)d;
    g_host = host;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_ticks_per_frame = (1e9 * tb.denom / tb.numer) / SAMPLE_RATE;
    os_log(g_log, "USB3HDCAP HAL plug-in initialized");
    return kAudioHardwareNoError;
}
static OSStatus HAL_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *out) { (void)d; (void)desc; (void)ci; (void)out; return kAudioHardwareUnsupportedOperationError; }
static OSStatus HAL_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id) { (void)d; (void)id; return kAudioHardwareUnsupportedOperationError; }
static OSStatus HAL_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *ci) { (void)d; (void)id; (void)ci; return kAudioHardwareNoError; }
static OSStatus HAL_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *ci) { (void)d; (void)id; (void)ci; return kAudioHardwareNoError; }
static OSStatus HAL_PerformConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 a, void *i) { (void)d; (void)id; (void)a; (void)i; return kAudioHardwareNoError; }
static OSStatus HAL_AbortConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 a, void *i) { (void)d; (void)id; (void)a; (void)i; return kAudioHardwareNoError; }

/* ---------- connection to hdcapd ---------- */

static void connect_daemon(void)
{
    if (g_conn)
        return;
    xpc_connection_t c = xpc_connection_create_mach_service(HDCAPD_SERVICE_NAME, NULL, 0);
    xpc_connection_set_event_handler(c, ^(xpc_object_t ev) {
        if (ev == XPC_ERROR_CONNECTION_INVALID || ev == XPC_ERROR_CONNECTION_INTERRUPTED) {
            atomic_store(&g_ring, NULL);
            os_log(g_log, "hdcapd connection lost");
        }
    });
    xpc_connection_resume(c);
    g_conn = c;
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(m, "cmd", "hello");
    xpc_dictionary_set_string(m, "client", "HAL plug-in");
    xpc_dictionary_set_bool(m, "video", false);
    xpc_dictionary_set_bool(m, "audio", true);
    xpc_connection_send_message_with_reply(c, m, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(xpc_object_t r) {
        void *addr = NULL;
        xpc_object_t shm = xpc_get_type(r) == XPC_TYPE_DICTIONARY ? xpc_dictionary_get_value(r, "audio_ring") : NULL;
        if (shm && xpc_shmem_map(shm, &addr) >= HDCAPD_RING_BYTES && ((hdcapd_audio_ring *)addr)->magic == HDCAPD_RING_MAGIC) {
            atomic_store(&g_ring, (const hdcapd_audio_ring *)addr);
            os_log(g_log, "mapped hdcapd audio ring");
        } else {
            os_log(g_log, "hdcapd unavailable; producing silence");
        }
    });
    xpc_release(m);
}

static void disconnect_daemon(void)
{
    atomic_store(&g_ring, NULL);
    if (g_conn) {
        xpc_connection_cancel(g_conn);
        xpc_release(g_conn);
        g_conn = NULL;
    }
}

/* ---------- IO ---------- */

static OSStatus HAL_StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{
    (void)d; (void)client;
    if (id != OBJ_DEVICE)
        return kAudioHardwareBadObjectError;
    os_unfair_lock_lock(&g_lock);
    if (g_io_clients++ == 0) {
        g_anchor_host = mach_absolute_time();
        g_timestamps = 0;
        g_offset_valid = 0;
        os_unfair_lock_unlock(&g_lock);
        connect_daemon();
        return kAudioHardwareNoError;
    }
    os_unfair_lock_unlock(&g_lock);
    return kAudioHardwareNoError;
}

static OSStatus HAL_StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{
    (void)d; (void)client;
    if (id != OBJ_DEVICE)
        return kAudioHardwareBadObjectError;
    os_unfair_lock_lock(&g_lock);
    int last = g_io_clients && --g_io_clients == 0;
    os_unfair_lock_unlock(&g_lock);
    if (last)
        disconnect_daemon();
    return kAudioHardwareNoError;
}

static OSStatus HAL_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, Float64 *st, UInt64 *ht, UInt64 *seed)
{
    (void)d; (void)client;
    if (id != OBJ_DEVICE)
        return kAudioHardwareBadObjectError;
    os_unfair_lock_lock(&g_lock);
    UInt64 now = mach_absolute_time();
    Float64 ticks_per_period = g_ticks_per_frame * RING_PERIOD;
    if (g_anchor_host + (UInt64)((g_timestamps + 1) * ticks_per_period) <= now)
        g_timestamps++;
    *st = (Float64)g_timestamps * RING_PERIOD;
    *ht = g_anchor_host + (UInt64)(g_timestamps * ticks_per_period);
    *seed = 1;
    os_unfair_lock_unlock(&g_lock);
    return kAudioHardwareNoError;
}

static OSStatus HAL_WillDoIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, Boolean *will, Boolean *inplace)
{
    (void)d; (void)id; (void)client;
    *will = op == kAudioServerPlugInIOOperationReadInput;
    *inplace = true;
    return kAudioHardwareNoError;
}
static OSStatus HAL_BeginIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 c, UInt32 op, UInt32 f, const AudioServerPlugInIOCycleInfo *ci) { (void)d; (void)id; (void)c; (void)op; (void)f; (void)ci; return kAudioHardwareNoError; }
static OSStatus HAL_EndIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 c, UInt32 op, UInt32 f, const AudioServerPlugInIOCycleInfo *ci) { (void)d; (void)id; (void)c; (void)op; (void)f; (void)ci; return kAudioHardwareNoError; }

static OSStatus HAL_DoIO(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream, UInt32 client, UInt32 op,
                         UInt32 frames, const AudioServerPlugInIOCycleInfo *ci, void *main, void *secondary)
{
    (void)d; (void)id; (void)stream; (void)client; (void)secondary;
    if (op != kAudioServerPlugInIOOperationReadInput || !main)
        return kAudioHardwareNoError;
    float *out = main;
    const hdcapd_audio_ring *ring = atomic_load(&g_ring);
    if (!ring) {
        memset(out, 0, (size_t)frames * CHANNELS * sizeof(float));
        return kAudioHardwareNoError;
    }
    int64_t w = (int64_t)atomic_load_explicit(&ring->write_frames, memory_order_acquire);
    int64_t s = (int64_t)ci->mInputTime.mSampleTime;
    uint32_t gen = atomic_load_explicit(&ring->generation, memory_order_relaxed);
    int64_t pos = s + g_offset;
    /* (Re)synchronise when not yet aligned, after a stream restart, or when drift has moved the
     * read position outside [w - capacity/2, w - frames]. */
    if (!g_offset_valid || gen != g_generation || pos + frames > w || pos < w - (int64_t)HDCAPD_RING_FRAMES / 2) {
        g_offset = (w - READ_LATENCY) - s;
        g_offset_valid = 1;
        g_generation = gen;
        pos = s + g_offset;
    }
    for (UInt32 i = 0; i < frames; i++) {
        int64_t f = pos + i;
        if (f < 0) {
            out[i * 2] = out[i * 2 + 1] = 0;
            continue;
        }
        size_t slot = (size_t)(f % HDCAPD_RING_FRAMES);
        out[i * 2] = ring->data[slot * 2] / 32768.0f;
        out[i * 2 + 1] = ring->data[slot * 2 + 1] / 32768.0f;
    }
    return kAudioHardwareNoError;
}

/* ---------- properties ---------- */

static AudioStreamBasicDescription stream_format(void)
{
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = SAMPLE_RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = f.mBytesPerFrame = CHANNELS * sizeof(float);
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = CHANNELS;
    f.mBitsPerChannel = 32;
    return f;
}

static Boolean HAL_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a)
{
    UInt32 sz;
    (void)d;
    return HAL_GetPropertyDataSize(g_driver, obj, pid, a, 0, NULL, &sz) == kAudioHardwareNoError;
}

static OSStatus HAL_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out)
{
    (void)d; (void)obj; (void)pid; (void)a;
    *out = false;   /* nothing is settable: fixed format and rate */
    return kAudioHardwareNoError;
}

#define RET_SIZE(T) do { *out = sizeof(T); return kAudioHardwareNoError; } while (0)

static OSStatus HAL_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                        UInt32 qsz, const void *q, UInt32 *out)
{
    (void)d; (void)pid; (void)qsz; (void)q;
    UInt32 s = a->mSelector;
    if (obj == OBJ_PLUGIN) {
        switch (s) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: RET_SIZE(AudioClassID);
        case kAudioObjectPropertyOwner: case kAudioPlugInPropertyTranslateUIDToDevice: RET_SIZE(AudioObjectID);
        case kAudioObjectPropertyManufacturer: case kAudioPlugInPropertyResourceBundle: RET_SIZE(CFStringRef);
        case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyDeviceList: RET_SIZE(AudioObjectID);
        }
    } else if (obj == OBJ_DEVICE) {
        switch (s) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: RET_SIZE(AudioClassID);
        case kAudioObjectPropertyOwner: RET_SIZE(AudioObjectID);
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: RET_SIZE(CFStringRef);
        case kAudioObjectPropertyOwnedObjects:
            *out = (a->mScope == kAudioObjectPropertyScopeOutput) ? 0 : sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        case kAudioDevicePropertyStreams:
            *out = (a->mScope == kAudioObjectPropertyScopeOutput) ? 0 : sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        case kAudioDevicePropertyRelatedDevices: RET_SIZE(AudioObjectID);
        case kAudioObjectPropertyControlList: *out = 0; return kAudioHardwareNoError;
        case kAudioDevicePropertyTransportType: case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioDevicePropertyClockIsStable: RET_SIZE(UInt32);
        case kAudioDevicePropertyNominalSampleRate: RET_SIZE(Float64);
        case kAudioDevicePropertyAvailableNominalSampleRates: RET_SIZE(AudioValueRange);
        case kAudioDevicePropertyPreferredChannelsForStereo: *out = 2 * sizeof(UInt32); return kAudioHardwareNoError;
        }
    } else if (obj == OBJ_STREAM) {
        switch (s) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: RET_SIZE(AudioClassID);
        case kAudioObjectPropertyOwner: RET_SIZE(AudioObjectID);
        case kAudioObjectPropertyOwnedObjects: *out = 0; return kAudioHardwareNoError;
        case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency: RET_SIZE(UInt32);
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: RET_SIZE(AudioStreamBasicDescription);
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            RET_SIZE(AudioStreamRangedDescription);
        }
    } else {
        return kAudioHardwareBadObjectError;
    }
    return kAudioHardwareUnknownPropertyError;
}

#define PUT(T, v) do { if (insz < sizeof(T)) return kAudioHardwareBadPropertySizeError; *(T *)out = (v); *outsz = sizeof(T); return kAudioHardwareNoError; } while (0)
#define PUTSTR(s) do { if (insz < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError; *(CFStringRef *)out = CFSTR(s); *outsz = sizeof(CFStringRef); return kAudioHardwareNoError; } while (0)

static OSStatus HAL_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qsz, const void *q, UInt32 insz, UInt32 *outsz, void *out)
{
    (void)d; (void)pid;
    UInt32 s = a->mSelector;
    if (obj == OBJ_PLUGIN) {
        switch (s) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: PUTSTR("open-usb3hdcap");
        case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyDeviceList:
            if (insz < sizeof(AudioObjectID)) { *outsz = 0; return kAudioHardwareNoError; }
            PUT(AudioObjectID, OBJ_DEVICE);
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            AudioObjectID r = kAudioObjectUnknown;
            if (qsz == sizeof(CFStringRef) && q && CFStringCompare(*(CFStringRef *)q, CFSTR(DEVICE_UID), 0) == kCFCompareEqualTo)
                r = OBJ_DEVICE;
            PUT(AudioObjectID, r);
        }
        case kAudioPlugInPropertyResourceBundle: PUTSTR("");
        }
    } else if (obj == OBJ_DEVICE) {
        switch (s) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, OBJ_PLUGIN);
        case kAudioObjectPropertyName: PUTSTR("USB3HDCAP");
        case kAudioObjectPropertyManufacturer: PUTSTR("StarTech (open-usb3hdcap)");
        case kAudioDevicePropertyDeviceUID: PUTSTR(DEVICE_UID);
        case kAudioDevicePropertyModelUID: PUTSTR(MODEL_UID);
        case kAudioObjectPropertyOwnedObjects: case kAudioDevicePropertyStreams:
            if (a->mScope == kAudioObjectPropertyScopeOutput || insz < sizeof(AudioObjectID)) { *outsz = 0; return kAudioHardwareNoError; }
            PUT(AudioObjectID, OBJ_STREAM);
        case kAudioDevicePropertyRelatedDevices: PUT(AudioObjectID, OBJ_DEVICE);
        case kAudioObjectPropertyControlList: *outsz = 0; return kAudioHardwareNoError;
        case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeUSB);
        case kAudioDevicePropertyClockDomain: PUT(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: PUT(UInt32, g_io_clients > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: PUT(UInt32, a->mScope != kAudioObjectPropertyScopeOutput);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, 0);
        case kAudioDevicePropertyLatency: PUT(UInt32, 0);
        case kAudioDevicePropertySafetyOffset: PUT(UInt32, 0);
        case kAudioDevicePropertyIsHidden: PUT(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, RING_PERIOD);
        case kAudioDevicePropertyClockIsStable: PUT(UInt32, 0);
        case kAudioDevicePropertyNominalSampleRate: PUT(Float64, SAMPLE_RATE);
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            AudioValueRange r = { SAMPLE_RATE, SAMPLE_RATE };
            PUT(AudioValueRange, r);
        }
        case kAudioDevicePropertyPreferredChannelsForStereo:
            if (insz < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)out)[0] = 1; ((UInt32 *)out)[1] = 2;
            *outsz = 2 * sizeof(UInt32);
            return kAudioHardwareNoError;
        }
    } else if (obj == OBJ_STREAM) {
        switch (s) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, OBJ_DEVICE);
        case kAudioObjectPropertyOwnedObjects: *outsz = 0; return kAudioHardwareNoError;
        case kAudioStreamPropertyIsActive: PUT(UInt32, 1);
        case kAudioStreamPropertyDirection: PUT(UInt32, 1);   /* 1 = input */
        case kAudioStreamPropertyTerminalType: PUT(UInt32, kAudioStreamTerminalTypeLine);
        case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1);
        case kAudioStreamPropertyLatency: PUT(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
            PUT(AudioStreamBasicDescription, stream_format());
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r = { stream_format(), { SAMPLE_RATE, SAMPLE_RATE } };
            PUT(AudioStreamRangedDescription, r);
        }
        }
    } else {
        return kAudioHardwareBadObjectError;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus HAL_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qsz, const void *q, UInt32 sz, const void *data)
{
    (void)d; (void)obj; (void)pid; (void)a; (void)qsz; (void)q; (void)sz; (void)data;
    return kAudioHardwareUnsupportedOperationError;
}

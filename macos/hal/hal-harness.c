/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * hal-harness: load USB3HDCAP.driver in-process (as coreaudiod would), list its objects and
 * properties, then run input IO for a few seconds and report level/continuity. Needs hdcapd
 * reachable from this process (the per-user dev agent is enough).
 *   hal-harness build/USB3HDCAP.driver [SECONDS]
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static AudioServerPlugInDriverRef drv;

static UInt32 get_u32(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope sc)
{
    AudioObjectPropertyAddress a = { s, sc, kAudioObjectPropertyElementMain };
    UInt32 v = 0, sz = sizeof v;
    if ((*drv)->GetPropertyData(drv, o, 0, &a, 0, NULL, sz, &sz, &v))
        return 0xFFFFFFFF;
    return v;
}

static void print_str(AudioObjectID o, AudioObjectPropertySelector s, const char *label)
{
    AudioObjectPropertyAddress a = { s, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef v = NULL;
    UInt32 sz = sizeof v;
    char buf[128] = "?";
    if (!(*drv)->GetPropertyData(drv, o, 0, &a, 0, NULL, sz, &sz, &v) && v)
        CFStringGetCString(v, buf, sizeof buf, kCFStringEncodingUTF8);
    printf("  %-14s %s\n", label, buf);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: hal-harness PATH/USB3HDCAP.driver [SECONDS]\n");
        return 2;
    }
    int seconds = argc > 2 ? atoi(argv[2]) : 5;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1], (CFIndex)strlen(argv[1]), true);
    CFPlugInRef plug = CFPlugInCreate(NULL, url);
    CFArrayRef factories = plug ? CFPlugInFindFactoriesForPlugInTypeInPlugIn(kAudioServerPlugInTypeUUID, plug) : NULL;
    if (!factories || CFArrayGetCount(factories) < 1) {
        fprintf(stderr, "no AudioServerPlugIn factory in %s\n", argv[1]);
        return 1;
    }
    IUnknownVTbl **iu = CFPlugInInstanceCreate(NULL, CFArrayGetValueAtIndex(factories, 0), kAudioServerPlugInTypeUUID);
    if (!iu || (*iu)->QueryInterface(iu, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), (LPVOID *)&drv) != S_OK) {
        fprintf(stderr, "factory/QueryInterface failed\n");
        return 1;
    }
    (*drv)->Initialize(drv, (AudioServerPlugInHostRef)(uintptr_t)1); /* host callbacks unused */

    AudioObjectPropertyAddress a = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectID dev = 0, stream = 0;
    UInt32 sz = sizeof dev;
    (*drv)->GetPropertyData(drv, kAudioObjectPlugInObject, 0, &a, 0, NULL, sz, &sz, &dev);
    a = (AudioObjectPropertyAddress){ kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain };
    sz = sizeof stream;
    (*drv)->GetPropertyData(drv, dev, 0, &a, 0, NULL, sz, &sz, &stream);
    printf("device %u, input stream %u\n", dev, stream);
    print_str(dev, kAudioObjectPropertyName, "name");
    print_str(dev, kAudioObjectPropertyManufacturer, "manufacturer");
    print_str(dev, kAudioDevicePropertyDeviceUID, "uid");
    printf("  %-14s %u\n  %-14s %u\n", "zts period", get_u32(dev, kAudioDevicePropertyZeroTimeStampPeriod, 0),
           "direction", get_u32(stream, kAudioStreamPropertyDirection, 0));
    AudioStreamBasicDescription f;
    a = (AudioObjectPropertyAddress){ kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    sz = sizeof f;
    (*drv)->GetPropertyData(drv, stream, 0, &a, 0, NULL, sz, &sz, &f);
    printf("  %-14s %.0f Hz, %u ch, %u bits, flags 0x%x\n", "format", f.mSampleRate, f.mChannelsPerFrame, f.mBitsPerChannel, f.mFormatFlags);

    /* IO loop: 512-frame cycles paced by the plug-in's own zero timestamps. */
    (*drv)->StartIO(drv, dev, 1);
    usleep(300000); /* allow the daemon hello to complete */
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const UInt32 N = 512;
    float buf[512 * 2];
    double sample = 0, sum = 0, peak = 0;
    unsigned long frames = 0, cycles = 0, jumps = 0;
    float prev = 0;
    Float64 zst; UInt64 zht, seed;
    (*drv)->GetZeroTimeStamp(drv, dev, 1, &zst, &zht, &seed);
    sample = zst;
    uint64_t start = mach_absolute_time();
    uint64_t end_ticks = (uint64_t)seconds * 1000000000ull * tb.denom / tb.numer;
    while (mach_absolute_time() - start < end_ticks) {
        AudioServerPlugInIOCycleInfo ci = {0};
        ci.mIOCycleCounter = cycles;
        ci.mNominalIOBufferFrameSize = N;
        ci.mInputTime.mSampleTime = sample;
        ci.mInputTime.mFlags = kAudioTimeStampSampleTimeValid;
        (*drv)->DoIOOperation(drv, dev, stream, 1, kAudioServerPlugInIOOperationReadInput, N, &ci, buf, NULL);
        for (UInt32 i = 0; i < N; i++) {
            float l = buf[i * 2];
            sum += (double)l * l;
            if (fabsf(l) > peak) peak = fabsf(l);
            if (fabsf(l - prev) > 0.5f) jumps++;
            prev = l;
        }
        frames += N;
        cycles++;
        sample += N;
        if (cycles % ((unsigned long)(48000 / N)) == 0)
            printf("  %5.1fs  rms L %.4f  peak %.3f  large steps %lu\n", frames / 48000.0, sqrt(sum / (double)frames), peak, jumps);
        usleep((useconds_t)(N * 1e6 / 48000));
    }
    (*drv)->StopIO(drv, dev, 1);
    printf("done: %lu cycles\n", cycles);
    return 0;
}

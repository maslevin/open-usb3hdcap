/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * USB3HDCAP.app: installs/removes the camera system extension it carries and acts as a small
 * control panel for hdcapd (input selection, live status). Built without nibs.
 */
#import <Cocoa/Cocoa.h>
#import <SystemExtensions/SystemExtensions.h>
#include "../common/hdcapd_protocol.h"
#include <xpc/xpc.h>

/* The extension's bundle identifier is the app's plus ".camera" (set at build time). */
#define EXT_ID [NSBundle.mainBundle.bundleIdentifier stringByAppendingString:@".camera"]

static NSString *const kInputNames[] = { @"HDMI", @"VGA", @"Component", @"Composite", @"S-Video", @"DVI" };

@interface AppDelegate : NSObject <NSApplicationDelegate, OSSystemExtensionRequestDelegate>
@end

@implementation AppDelegate {
    NSWindow *_win;
    NSTextField *_status, *_extStatus;
    NSPopUpButton *_input;
    xpc_connection_t _conn;
    NSTimer *_timer;
}

static NSTextField *label(NSString *s, NSRect r)
{
    NSTextField *t = [NSTextField labelWithString:s];
    t.frame = r;
    return t;
}

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    _win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 460, 220)
                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                         backing:NSBackingStoreBuffered defer:NO];
    _win.title = @"USB3HDCAP";
    NSView *v = _win.contentView;

    [v addSubview:label(@"Input:", NSMakeRect(20, 172, 60, 22))];
    _input = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(80, 168, 160, 28) pullsDown:NO];
    for (int i = 0; i < 6; i++) [_input addItemWithTitle:kInputNames[i]];
    _input.target = self;
    _input.action = @selector(inputChanged:);
    [v addSubview:_input];

    _status = label(@"hdcapd: connecting…", NSMakeRect(20, 120, 420, 40));
    _status.maximumNumberOfLines = 2;
    [v addSubview:_status];

    NSButton *install = [NSButton buttonWithTitle:@"Install Camera" target:self action:@selector(installExtension:)];
    install.frame = NSMakeRect(16, 60, 150, 32);
    NSButton *remove = [NSButton buttonWithTitle:@"Remove Camera" target:self action:@selector(removeExtension:)];
    remove.frame = NSMakeRect(170, 60, 150, 32);
    [v addSubview:install];
    [v addSubview:remove];
    _extStatus = label(@"", NSMakeRect(20, 20, 420, 34));
    _extStatus.maximumNumberOfLines = 2;
    [v addSubview:_extStatus];

    [_win center];
    [_win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    [self connect];
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) { (void)t; [self refresh]; }];
    [self refresh];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }

/* ---------- hdcapd ---------- */

- (void)connect
{
    _conn = xpc_connection_create_mach_service(HDCAPD_SERVICE_NAME, dispatch_get_main_queue(), 0);
    __weak AppDelegate *weakSelf = self;
    xpc_connection_set_event_handler(_conn, ^(xpc_object_t ev) {
        AppDelegate *s = weakSelf;
        if (s && xpc_get_type(ev) == XPC_TYPE_ERROR && ev == XPC_ERROR_CONNECTION_INVALID) {
            s->_conn = nil;
            s->_status.stringValue = @"hdcapd is not running (install it with macos/scripts/install-system.sh)";
        }
    });
    xpc_connection_resume(_conn);
}

- (void)refresh
{
    if (!_conn) [self connect];
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(m, "cmd", "get-status");
    xpc_connection_send_message_with_reply(_conn, m, dispatch_get_main_queue(), ^(xpc_object_t r) {
        if (xpc_get_type(r) != XPC_TYPE_DICTIONARY) return;
        int64_t in = xpc_dictionary_get_int64(r, "input");
        if (in >= 0 && in < 6 && self->_input.indexOfSelectedItem != in) [self->_input selectItemAtIndex:in];
        const char *state = xpc_dictionary_get_string(r, "state_name"), *detail = xpc_dictionary_get_string(r, "detail");
        int64_t w = xpc_dictionary_get_int64(r, "width"), h = xpc_dictionary_get_int64(r, "height");
        NSString *s = [NSString stringWithFormat:@"hdcapd: %s", state ?: "?"];
        if (w > 0)
            s = [s stringByAppendingFormat:@" — %lldx%lld%s @ %.2f fps", w, h, xpc_dictionary_get_bool(r, "interlaced") ? "i" : "p",
                 xpc_dictionary_get_double(r, "fps")];
        if (detail && *detail) s = [s stringByAppendingFormat:@"\n%s", detail];
        self->_status.stringValue = s;
    });
}

- (void)inputChanged:(id)sender
{
    if (!_conn) return;
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(m, "cmd", "set-config");
    xpc_dictionary_set_int64(m, "input", _input.indexOfSelectedItem);
    xpc_dictionary_set_int64(m, "analog_mode", -1);
    xpc_dictionary_set_int64(m, "h_offset", 0);
    xpc_dictionary_set_int64(m, "v_offset", 0);
    xpc_dictionary_set_int64(m, "tv_standard", 7 /* HDCAP_STD_AUTO */);
    xpc_connection_send_message_with_reply(_conn, m, dispatch_get_main_queue(), ^(xpc_object_t r) { (void)r; [self refresh]; });
}

/* ---------- system extension ---------- */

- (void)installExtension:(id)sender
{
    _extStatus.stringValue = @"Requesting activation…";
    OSSystemExtensionRequest *r = [OSSystemExtensionRequest activationRequestForExtension:EXT_ID queue:dispatch_get_main_queue()];
    r.delegate = self;
    [[OSSystemExtensionManager sharedManager] submitRequest:r];
}

- (void)removeExtension:(id)sender
{
    _extStatus.stringValue = @"Requesting removal…";
    OSSystemExtensionRequest *r = [OSSystemExtensionRequest deactivationRequestForExtension:EXT_ID queue:dispatch_get_main_queue()];
    r.delegate = self;
    [[OSSystemExtensionManager sharedManager] submitRequest:r];
}

- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest *)request
                  actionForReplacingExtension:(OSSystemExtensionProperties *)existing
                                withExtension:(OSSystemExtensionProperties *)ext
{
    return OSSystemExtensionReplacementActionReplace;
}

- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request
{
    _extStatus.stringValue = @"Approve the extension in System Settings → General → Login Items & Extensions.";
}

- (void)request:(OSSystemExtensionRequest *)request didFinishWithResult:(OSSystemExtensionRequestResult)result
{
    _extStatus.stringValue = result == OSSystemExtensionRequestCompleted ? @"Done." : @"Done; takes effect after a restart.";
}

- (void)request:(OSSystemExtensionRequest *)request didFailWithError:(NSError *)error
{
    _extStatus.stringValue = [NSString stringWithFormat:@"Failed: %@ (code %ld)", error.localizedDescription, (long)error.code];
}
@end

int main(void)
{
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        AppDelegate *d = [AppDelegate new];
        app.delegate = d;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        [app run];
    }
    return 0;
}

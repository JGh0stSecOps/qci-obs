@import CoreMediaIO;
@import SystemExtensions;

#include <obs-module.h>
#include "OBSDALMachServer.h"
#include "Defines.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("mac-virtualcam", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "macOS virtual webcam output";
}

NSString *const OBSDalDestination = @"/Library/CoreMediaIO/Plug-Ins/DAL";

// The DAL plug-in bundle this fork installs into the SHARED system DAL directory, and that
// bundle's identifier. Both are supplied as compile definitions read straight off the
// obs-dal-plugin target (plugins/mac-virtualcam/CMakeLists.txt, after add_subdirectory), so
// neither can be a second literal that drifts from what is actually built, staged into
// Contents/Resources, and installed. That mattered: upstream typed "obs-mac-virtualcam.plugin"
// into four call sites below, and once obs-dal-plugin was renamed those four kept pointing at the
// STOCK OBS plug-in living beside ours in the same directory — including the one that deletes.
#ifndef OBS_DAL_PLUGIN_BUNDLE_NAME
#error "OBS_DAL_PLUGIN_BUNDLE_NAME must be defined; see plugins/mac-virtualcam/CMakeLists.txt"
#endif
#ifndef OBS_DAL_PLUGIN_BUNDLE_ID
#error "OBS_DAL_PLUGIN_BUNDLE_ID must be defined; see plugins/mac-virtualcam/CMakeLists.txt"
#endif

// The CMIO camera extension this fork activates, supplied the same way and for a sharper reason.
// This identifier is what macOS keys system-extension REPLACEMENT on: activationRequestForExtension:
// takes it, and -request:actionForReplacingExtension:withExtension: below fires only against an
// incumbent bearing the same one. The three VIRTUALCAM_*_UUIDs do not enter that decision. As a
// hand-typed literal here it was a second, unchecked copy of a value CMake owns
// (src/camera-extension/CMakeLists.txt): had it kept upstream's
// "com.obsproject.obs-studio.mac-camera-extension" through the rename, this fork would have
// submitted activation requests naming STOCK OBS'S extension, and the delegate's unconditional
// Replace would have taken it over — with nothing in the build complaining. Read off the
// mac-camera-extension target instead, and asserted there against upstream's value.
#ifndef OBS_CAMERA_EXTENSION_BUNDLE_ID
#error "OBS_CAMERA_EXTENSION_BUNDLE_ID must be defined; see plugins/mac-virtualcam/CMakeLists.txt"
#endif

static NSString *const OBSDalPluginName = @OBS_DAL_PLUGIN_BUNDLE_NAME;
static NSString *const OBSDalPluginBundleID = @OBS_DAL_PLUGIN_BUNDLE_ID;
static NSString *const OBSCameraExtensionBundleID = @OBS_CAMERA_EXTENSION_BUNDLE_ID;

// This plug-in's own bundle identifier, as assigned by cmake/macos/helpers.cmake
// (PRODUCT_BUNDLE_IDENTIFIER solutions.zoetic.qci-studio.${target}, target "mac-virtualcam").
// Keep these two in lockstep: a mismatch makes +[NSBundle bundleWithIdentifier:] return nil.
static NSString *const OBSVirtualCamPluginBundleID = @"solutions.zoetic.qci-studio.mac-virtualcam";

static bool cmio_extension_supported()
{
    if (@available(macOS 13.0, *)) {
        return true;
    } else {
        return false;
    }
}

struct virtualcam_data {
    obs_output_t *output;
    obs_video_info videoInfo;
    CVPixelBufferPoolRef pool;

    // CMIO Extension (available with macOS 13)
    CMSimpleQueueRef queue;
    CMIODeviceID deviceID;
    CMIOStreamID streamID;
    CMFormatDescriptionRef formatDescription;
    id extensionDelegate;

    // Legacy DAL (deprecated since macOS 12.3)
    OBSDALMachServer *machServer;
};

@interface SystemExtensionActivationDelegate : NSObject <OSSystemExtensionRequestDelegate> {
      @private
    struct virtualcam_data *_vcam;
}

@property (getter=isInstalled) BOOL installed;
@property NSString *lastErrorMessage;
- (instancetype)init __unavailable;
@end

@implementation SystemExtensionActivationDelegate

- (id)initWithVcam:(virtualcam_data *)vcam
{
    self = [super init];

    if (self) {
        _vcam = vcam;
        _installed = NO;
    }

    return self;
}

- (OSSystemExtensionReplacementAction)request:(nonnull OSSystemExtensionRequest *)request
                  actionForReplacingExtension:(nonnull OSSystemExtensionProperties *)existing
                                withExtension:(nonnull OSSystemExtensionProperties *)ext
{
    NSString *infoString = [NSString
        stringWithFormat:
            @"mac-camera-extension: Replacement requested. Existing version: %@ (%@), new version: %@ (%@). Replacing...",
            existing.bundleShortVersion, existing.bundleVersion, ext.bundleShortVersion, ext.bundleVersion];
    blog(LOG_INFO, "%s", infoString.UTF8String);
    return OSSystemExtensionReplacementActionReplace;
}

- (void)request:(nonnull OSSystemExtensionRequest *)request didFailWithError:(nonnull NSError *)error
{
    NSString *errorMessage;
    int severity;

    switch (error.code) {
        case OSSystemExtensionErrorUnsupportedParentBundleLocation:
            self.lastErrorMessage =
                [NSString stringWithUTF8String:obs_module_text("Error.SystemExtension.WrongLocation")];
            errorMessage = self.lastErrorMessage;
            severity = LOG_WARNING;
            break;
        default:
            self.lastErrorMessage = error.localizedDescription;
            errorMessage = [NSString stringWithFormat:@"OSSystemExtensionErrorCode %ld (\"%s\")", error.code,
                                                      error.localizedDescription.UTF8String];
            severity = LOG_ERROR;
            break;
    }

    blog(severity, "mac-camera-extension: %s", errorMessage.UTF8String);
}

- (void)request:(nonnull OSSystemExtensionRequest *)request didFinishWithResult:(OSSystemExtensionRequestResult)result
{
    switch (result) {
        case OSSystemExtensionRequestCompleted:
            self.installed = YES;
            blog(LOG_INFO, "macOS Camera Extension activated successfully.");
            break;
        case OSSystemExtensionRequestWillCompleteAfterReboot:
            self.lastErrorMessage =
                [NSString stringWithUTF8String:obs_module_text("Error.SystemExtension.CompleteAfterReboot")];
            blog(LOG_INFO, "macOS Camera Extension will activate after reboot.");
            break;
    }
}

- (void)requestNeedsUserApproval:(nonnull OSSystemExtensionRequest *)request
{
    self.installed = NO;
    blog(LOG_INFO, "macOS Camera Extension user approval required.");
}

@end

// Whether this application actually CONTAINS the camera extension it is about to ask macOS to
// activate. macOS only activates a system extension embedded in the calling app's bundle, at
// Contents/Library/SystemExtensions.
//
// This is checked because the embed is conditional at build time and degrades quietly:
// cmake/macos/helpers.cmake only adds the copy step when signing is Automatic or a provisioning
// profile is set, and otherwise builds the .systemextension and leaves it beside the app. An app
// built that way submits a perfectly well-formed activation request for an extension that is not
// there, and the user is then told "the virtual camera is not installed — please approve it in
// System Settings", where there is nothing to approve. Distinguishing the two costs one directory
// scan and turns an unfalsifiable support problem into a one-line answer.
//
// Matched on CFBundleIdentifier, not on filename: the identifier is what the activation request
// names and what the OS resolves, and the bundle's filename is only conventionally the same string.
static BOOL camera_extension_is_embedded()
{
    NSURL *extensionsURL = [[[NSBundle mainBundle] bundleURL]
        URLByAppendingPathComponent:@"Contents/Library/SystemExtensions"
                        isDirectory:YES];

    NSArray<NSURL *> *entries = [[NSFileManager defaultManager] contentsOfDirectoryAtURL:extensionsURL
                                                              includingPropertiesForKeys:nil
                                                                                 options:0
                                                                                   error:nil];

    for (NSURL *entry in entries) {
        NSBundle *candidate = [NSBundle bundleWithURL:entry];

        if ([candidate.bundleIdentifier isEqualToString:OBSCameraExtensionBundleID]) {
            return YES;
        }
    }

    return NO;
}

static void install_cmio_system_extension(struct virtualcam_data *vcam)
{
    if (!camera_extension_is_embedded()) {
        SystemExtensionActivationDelegate *delegate = vcam->extensionDelegate;
        NSString *message = [NSString
            stringWithFormat:@"This build of the application does not contain the camera extension "
                             @"\"%@\" (nothing with that identifier is in "
                             @"Contents/Library/SystemExtensions), so macOS has nothing to activate. "
                             @"This is a build configuration problem, not something to approve in "
                             @"System Settings: the extension is only embedded when the app is signed "
                             @"with a provisioning profile carrying "
                             @"com.apple.developer.system-extension.install. Rebuild with "
                             @"PROVISIONING_PROFILE set.",
                             OBSCameraExtensionBundleID];

        blog(LOG_ERROR, "mac-camera-extension: %s", message.UTF8String);
        delegate.lastErrorMessage = message;
        return;
    }

    OSSystemExtensionRequest *request = [OSSystemExtensionRequest
        activationRequestForExtension:OBSCameraExtensionBundleID
                                queue:dispatch_get_main_queue()];
    request.delegate = vcam->extensionDelegate;

    [[OSSystemExtensionManager sharedManager] submitRequest:request];
}

// ---------------------------------------------------------------------------------------------
// Everything from here to uninstall_dal_plugin() runs, or composes text that runs, as ROOT via
// NSAppleScript's `with administrator privileges`, inside a directory this application does not
// own. Three rules hold throughout, and each one is load-bearing:
//
//   1. Address a BUNDLE, never the directory. /Library/CoreMediaIO/Plug-Ins/DAL is Apple's shared
//      drop point for third-party CoreMediaIO plug-ins ("Third party CoreMediaIO DAL Plug-Ins",
//      per the plugins-info.txt Apple ships in it). Upstream's update path ran
//      `rm -rf '/Library/CoreMediaIO/Plug-Ins/DAL'` — the whole directory, every vendor's plug-in
//      and Apple's own marker file, as root.
//   2. Prove ownership before deleting. A filename is an assumption; CFBundleIdentifier is
//      evidence. Nothing gets removed unless the bundle standing there is the one this build
//      produced.
//   3. Quote everything that reaches the shell. The source path comes from the app's own bundle
//      URL, which the user can rename or relocate at will, and it is interpolated into a
//      root shell command nested inside an AppleScript string literal.
// ---------------------------------------------------------------------------------------------

// Quote `string` as a single literal POSIX shell word. Everything inside single quotes is literal
// to the shell; an embedded single quote cannot be escaped there, so the standard idiom is to
// close the quoting, emit an escaped quote, and reopen it.
static NSString *shell_quoted(NSString *string)
{
    NSString *escaped = [string stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"];

    return [NSString stringWithFormat:@"'%@'", escaped];
}

// Escape `string` for embedding in an AppleScript double-quoted string literal. Backslashes must
// be doubled first, otherwise the backslashes introduced when escaping the quotes get doubled too.
static NSString *applescript_quoted(NSString *string)
{
    NSString *escaped = [string stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];

    return [escaped stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
}

// Absolute path of the one bundle inside the shared DAL directory that belongs to this build.
static NSString *dal_plugin_path()
{
    return [OBSDalDestination stringByAppendingPathComponent:OBSDalPluginName];
}

// YES only when the bundle installed at dal_plugin_path() is this build's own, judged by the
// identifier baked into it. Gates every destructive step. Sharing the directory with stock OBS
// means a bundle being present is never on its own a reason to delete it.
static BOOL installed_dal_plugin_is_ours()
{
    NSString *pluginPath = dal_plugin_path();

    // Nothing installed under our name is the ordinary case, and says nothing about ownership.
    // Answer it before the identifier check so it stays silent instead of warning on every start.
    if (![[NSFileManager defaultManager] fileExistsAtPath:pluginPath]) {
        return NO;
    }

    NSString *infoPlistPath = [pluginPath stringByAppendingPathComponent:@"Contents/Info.plist"];
    NSDictionary *infoPlist = [NSDictionary dictionaryWithContentsOfURL:[NSURL fileURLWithPath:infoPlistPath]];
    NSString *identifier = [infoPlist valueForKey:@"CFBundleIdentifier"];

    if (![identifier isEqualToString:OBSDalPluginBundleID]) {
        blog(LOG_WARNING,
             "[macOS] A DAL plug-in named '%s' is installed but its bundle identifier is '%s', not this "
             "build's '%s'. Leaving it alone: it belongs to another application.",
             OBSDalPluginName.UTF8String, identifier ? identifier.UTF8String : "(unreadable)",
             OBSDalPluginBundleID.UTF8String);
        return NO;
    }

    return YES;
}

typedef enum {
    OBSDalPluginNotInstalled,
    OBSDalPluginInstalled,
    OBSDalPluginNeedsUpdate
} dal_plugin_status;

static dal_plugin_status check_dal_plugin()
{
    NSString *dalPluginFileName = dal_plugin_path();

    // Ours, or nothing. A bundle at this path that belongs to someone else must read as
    // NotInstalled: reporting it as Installed/NeedsUpdate would send virtualcam_output_start()
    // into uninstall_dal_plugin() or install_dal_plugin(true) against another vendor's files.
    BOOL dalPluginInstalled = installed_dal_plugin_is_ours();

    if (dalPluginInstalled) {
        NSString *dalPluginInfoPlistPath = [dalPluginFileName stringByAppendingPathComponent:@"Contents/Info.plist"];
        NSDictionary *dalPluginInfoPlist =
            [NSDictionary dictionaryWithContentsOfURL:[NSURL fileURLWithPath:dalPluginInfoPlistPath]];

        NSString *dalPluginVersion = [dalPluginInfoPlist valueForKey:@"CFBundleShortVersionString"];
        NSString *dalPluginBuild = [dalPluginInfoPlist valueForKey:@"CFBundleVersion"];

        NSString *obsVersion = [[[NSBundle mainBundle] infoDictionary] objectForKey:@"CFBundleShortVersionString"];
        NSString *obsBuild = [[[NSBundle mainBundle] infoDictionary] objectForKey:(NSString *) kCFBundleVersionKey];
        BOOL dalPluginUpdateNeeded =
            !([dalPluginVersion isEqualToString:obsVersion] && [dalPluginBuild isEqualToString:obsBuild]);

        return dalPluginUpdateNeeded ? OBSDalPluginNeedsUpdate : OBSDalPluginInstalled;
    }

    return OBSDalPluginNotInstalled;
}

static bool install_dal_plugin(bool update)
{
    NSFileManager *fileManager = [NSFileManager defaultManager];

    // Rule 2 of the three above -- prove ownership before touching -- applied to the WRITE, not just
    // the delete. check_dal_plugin() deliberately reports a foreign bundle standing at our path as
    // NotInstalled, which is correct for the delete (it must not be removed) but routes straight
    // here, where the copy below runs as root with no ownership test of its own.
    //
    // Measured, because the comment on the copy claims otherwise: `cp -R <ours> <DAL dir>` with a
    // directory of our name already present does NOT create a nested bundle and does NOT replace the
    // directory. It MERGES -- our Contents/Info.plist and executable overwrite theirs, their extra
    // files survive -- producing one root-owned bundle that is half ours and half someone else's,
    // and belongs to neither. That is a worse outcome than refusing.
    //
    // Unreachable today (this whole DAL path is macOS < 13 only and the deployment target is 13.0),
    // and no other vendor ships a bundle by our name. It is closed because the cost is four lines
    // and the failure mode is an elevated write into another application's files.
    if (!update && [fileManager fileExistsAtPath:dal_plugin_path()] && !installed_dal_plugin_is_ours()) {
        blog(LOG_ERROR,
             "[macOS] Refusing to install the virtual camera DAL plug-in: '%s' already exists and is "
             "not this build's. Copying over it as root would merge two applications' bundles. "
             "Remove it by hand if it is stale.",
             dal_plugin_path().UTF8String);
        return false;
    }

    BOOL dalPluginDirExists = [fileManager fileExistsAtPath:OBSDalDestination];

    NSURL *bundleURL = [[NSBundle mainBundle] bundleURL];
    NSString *pluginPath = [@"Contents/Resources" stringByAppendingPathComponent:OBSDalPluginName];

    NSURL *pluginUrl = [bundleURL URLByAppendingPathComponent:pluginPath];
    NSString *dalPluginSourcePath = [pluginUrl path];

    NSString *createPluginDirCmd =
        (!dalPluginDirExists) ? [NSString stringWithFormat:@"mkdir -p %@ && ", shell_quoted(OBSDalDestination)] : @"";

    // Remove this ONE bundle, and only after confirming it is ours. Upstream removed
    // OBSDalDestination itself — the entire shared directory, every other vendor's plug-in
    // included — as root. Nothing here may name anything but our own bundle.
    NSString *deleteOldPluginCmd = (update && installed_dal_plugin_is_ours())
                                       ? [NSString stringWithFormat:@"rm -rf %@ && ", shell_quoted(dal_plugin_path())]
                                       : @"";
    // The destination is the directory: `cp -R <bundle> <dir>` places our bundle inside it,
    // leaving everything else in there untouched.
    NSString *copyPluginCmd =
        [NSString stringWithFormat:@"cp -R %@ %@", shell_quoted(dalPluginSourcePath), shell_quoted(OBSDalDestination)];

    if ([fileManager fileExistsAtPath:dalPluginSourcePath]) {
        // dalPluginSourcePath is derived from the app's own bundle URL, so it carries whatever the
        // user named or moved the application to. Unescaped, a single quote in that path closes the
        // shell quoting and a double quote or backslash closes the AppleScript literal — turning the
        // rest of the path into commands that run as root. Escape for the shell first, then for
        // AppleScript, which is the order the two layers unwrap in.
        NSString *shellCmd =
            [NSString stringWithFormat:@"%@%@%@", createPluginDirCmd, deleteOldPluginCmd, copyPluginCmd];
        NSString *copyCmd = [NSString
            stringWithFormat:@"do shell script \"%@\" with administrator privileges", applescript_quoted(shellCmd)];

        NSDictionary *errorDict;
        NSAppleScript *scriptObject = [[NSAppleScript alloc] initWithSource:copyCmd];
        [scriptObject executeAndReturnError:&errorDict];
        if (errorDict != nil) {
            const char *errorMessage = [[errorDict objectForKey:@"NSAppleScriptErrorMessage"] UTF8String];

            blog(LOG_INFO, "[macOS] VirtualCam DAL Plugin Installation status: %s", errorMessage);
            return false;
        } else {
            return true;
        }
    } else {
        blog(LOG_INFO, "[macOS] VirtualCam DAL Plugin not shipped with OBS");
        return false;
    }
}

static bool uninstall_dal_plugin()
{
    // The live destructive path on macOS 13+: virtualcam_output_start() calls this on every start
    // whenever a DAL plug-in is present, to retire the legacy plug-in in favour of the CMIO system
    // extension. Retiring OUR legacy plug-in is the entire intent — a stock OBS installation's
    // plug-in in the same directory is not ours to retire, and deleting it would break that
    // application's virtual camera on a machine where it drives a live stream.
    if (!installed_dal_plugin_is_ours()) {
        // Not an error for the caller: there is no plug-in of ours left to remove, which is the
        // post-condition it is asking for. Returning false here would instead fail the camera start
        // with Error.DAL.NotUninstalled for as long as the other application stayed installed.
        return true;
    }

    NSString *removeCmd = [NSString stringWithFormat:@"rm -rf %@", shell_quoted(dal_plugin_path())];
    NSAppleScript *scriptObject = [[NSAppleScript alloc]
        initWithSource:[NSString stringWithFormat:@"do shell script \"%@\" with administrator privileges",
                                                  applescript_quoted(removeCmd)]];

    NSDictionary *errorDict;

    [scriptObject executeAndReturnError:&errorDict];
    if (errorDict) {
        blog(LOG_INFO, "[macOS] VirtualCam DAL Plugin could not be uninstalled: %s",
             [[errorDict objectForKey:NSAppleScriptErrorMessage] UTF8String]);
        return false;
    } else {
        return true;
    }
}

FourCharCode convert_video_format_to_mac(enum video_format format, enum video_range_type range)
{
    switch (format) {
        case VIDEO_FORMAT_I420:
            return (range == VIDEO_RANGE_FULL) ? kCVPixelFormatType_420YpCbCr8PlanarFullRange
                                               : kCVPixelFormatType_420YpCbCr8Planar;
        case VIDEO_FORMAT_NV12:
            return (range == VIDEO_RANGE_FULL) ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                               : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        case VIDEO_FORMAT_UYVY:
            return (range == VIDEO_RANGE_FULL) ? kCVPixelFormatType_422YpCbCr8FullRange : kCVPixelFormatType_422YpCbCr8;
        case VIDEO_FORMAT_P010:
            return (range == VIDEO_RANGE_FULL) ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
                                               : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
        default:
            // Zero indicates that the format is not supported on macOS
            // Note that some formats do have an associated constant, but
            // constructing such formats fails with kCVReturnInvalidPixelFormat.
            return 0;
    }
}

static const char *virtualcam_output_get_name(void *type_data)
{
    (void) type_data;
    return obs_module_text("Plugin_Name");
}

static void *virtualcam_output_create(obs_data_t *settings, obs_output_t *output)
{
    UNUSED_PARAMETER(settings);

    struct virtualcam_data *vcam = (struct virtualcam_data *) bzalloc(sizeof(*vcam));

    vcam->output = output;

    if (cmio_extension_supported()) {
        vcam->extensionDelegate = [[SystemExtensionActivationDelegate alloc] initWithVcam:vcam];
        install_cmio_system_extension(vcam);
    } else {
        vcam->machServer = [[OBSDALMachServer alloc] init];
    }

    return vcam;
}

static void virtualcam_output_destroy(void *data)
{
    struct virtualcam_data *vcam = (struct virtualcam_data *) data;

    if (cmio_extension_supported()) {
        vcam->extensionDelegate = nil;
    } else {
        vcam->machServer = nil;
    }

    bfree(vcam);
}

static bool virtualcam_output_start(void *data)
{
    struct virtualcam_data *vcam = (struct virtualcam_data *) data;

    dal_plugin_status dal_status = check_dal_plugin();

    if (cmio_extension_supported()) {
        if (dal_status != OBSDalPluginNotInstalled) {
            if (!uninstall_dal_plugin()) {
                obs_output_set_last_error(vcam->output, obs_module_text("Error.DAL.NotUninstalled"));
                return false;
            }
        }

        SystemExtensionActivationDelegate *delegate = vcam->extensionDelegate;

        if (!delegate.installed) {
            if (delegate.lastErrorMessage) {
                obs_output_set_last_error(
                    vcam->output,
                    [NSString stringWithFormat:@"%s\n\n%@", obs_module_text("Error.SystemExtension.InstallationError"),
                                               delegate.lastErrorMessage]
                        .UTF8String);
            } else {
                if (@available(macOS 15.0, *)) {
                    obs_output_set_last_error(vcam->output,
                                              obs_module_text("Error.SystemExtension.NotInstalled.MacOS15"));
                } else {
                    obs_output_set_last_error(vcam->output, obs_module_text("Error.SystemExtension.NotInstalled"));
                }
            }

            return false;
        }
    } else {
        bool success = false;
        if (dal_status == OBSDalPluginNotInstalled) {
            success = install_dal_plugin(false);
        } else if (dal_status == OBSDalPluginNeedsUpdate) {
            success = install_dal_plugin(true);
        } else {
            success = true;
        }

        if (!success) {
            obs_output_set_last_error(vcam->output, "Error.DAL.NotInstalled");
            return false;
        }
    }

    obs_get_video_info(&vcam->videoInfo);

    FourCharCode video_format = convert_video_format_to_mac(vcam->videoInfo.output_format, vcam->videoInfo.range);

    struct video_scale_info conversion = {};
    conversion.width = vcam->videoInfo.output_width;
    conversion.height = vcam->videoInfo.output_height;
    conversion.colorspace = vcam->videoInfo.colorspace;
    conversion.range = vcam->videoInfo.range;

    if (!video_format) {
        // Selected output format is not supported natively by CoreVideo, CPU conversion necessary
        blog(LOG_WARNING, "Selected output format (%s) not supported by CoreVideo, enabling CPU transcoding...",
             get_video_format_name(vcam->videoInfo.output_format));

        conversion.format = VIDEO_FORMAT_NV12;
        video_format = convert_video_format_to_mac(conversion.format, conversion.range);
    } else {
        conversion.format = vcam->videoInfo.output_format;
    }
    obs_output_set_video_conversion(vcam->output, &conversion);

    NSDictionary *pAttr = @ {};
    NSDictionary *pbAttr = @{
        (id) kCVPixelBufferPixelFormatTypeKey: @(video_format),
        (id) kCVPixelBufferWidthKey: @(vcam->videoInfo.output_width),
        (id) kCVPixelBufferHeightKey: @(vcam->videoInfo.output_height),
        (id) kCVPixelBufferIOSurfacePropertiesKey: @ {}
    };
    CVReturn status = CVPixelBufferPoolCreate(kCFAllocatorDefault, (__bridge CFDictionaryRef) pAttr,
                                              (__bridge CFDictionaryRef) pbAttr, &vcam->pool);

    if (status != kCVReturnSuccess) {
        blog(LOG_ERROR, "unable to allocate pixel buffer pool (error %d)", status);
        return false;
    }

    if (cmio_extension_supported()) {
        UInt32 size;
        UInt32 used;

        CMIOObjectPropertyAddress address {.mSelector = kCMIOHardwarePropertyDevices,
                                           .mScope = kCMIOObjectPropertyScopeGlobal,
                                           .mElement = kCMIOObjectPropertyElementMain};
        CMIOObjectGetPropertyDataSize(kCMIOObjectSystemObject, &address, 0, NULL, &size);
        NSMutableData *cmioDevices = [NSMutableData dataWithLength:size];
        void *device_data = [cmioDevices mutableBytes];
        CMIOObjectGetPropertyData(kCMIOObjectSystemObject, &address, 0, NULL, size, &used, device_data);

        vcam->deviceID = 0;

        // Fail loudly here rather than a few lines down. A nil bundle yields a nil UUID string,
        // CFUUIDCreateFromString(_, NULL) returns NULL, and CFEqual(NULL, _) then traps inside
        // CoreFoundation with a backtrace that names neither this plug-in nor the identifier it
        // was looking for.
        NSBundle *virtualCamPluginBundle = [NSBundle bundleWithIdentifier:OBSVirtualCamPluginBundleID];

        if (!virtualCamPluginBundle) {
            NSString *message =
                [NSString stringWithFormat:@"Virtual camera plugin bundle \"%@\" could not be found. "
                                           @"Its CFBundleIdentifier no longer matches the identifier this "
                                           @"plugin looks up; the camera device UUID cannot be read.",
                                           OBSVirtualCamPluginBundleID];
            blog(LOG_ERROR, "%s", message.UTF8String);
            obs_output_set_last_error(vcam->output, message.UTF8String);
            return false;
        }

        NSString *OBSVirtualCamUUIDString = [virtualCamPluginBundle objectForInfoDictionaryKey:@"OBSCameraDeviceUUID"];

        if (!OBSVirtualCamUUIDString) {
            NSString *message = [NSString stringWithFormat:@"Virtual camera plugin bundle \"%@\" has no "
                                                           @"OBSCameraDeviceUUID key in its Info.plist.",
                                                           OBSVirtualCamPluginBundleID];
            blog(LOG_ERROR, "%s", message.UTF8String);
            obs_output_set_last_error(vcam->output, message.UTF8String);
            return false;
        }

        CFUUIDRef OBSVirtualCamUUID =
            CFUUIDCreateFromString(kCFAllocatorDefault, (CFStringRef) OBSVirtualCamUUIDString);

        if (!OBSVirtualCamUUID) {
            NSString *message =
                [NSString stringWithFormat:@"OBSCameraDeviceUUID \"%@\" in virtual camera plugin bundle \"%@\" "
                                           @"is not a valid UUID string.",
                                           OBSVirtualCamUUIDString, OBSVirtualCamPluginBundleID];
            blog(LOG_ERROR, "%s", message.UTF8String);
            obs_output_set_last_error(vcam->output, message.UTF8String);
            return false;
        }

        size_t num_elements = size / sizeof(CMIOObjectID);
        for (size_t i = 0; i < num_elements; i++) {
            CMIOObjectID cmioDevice;
            [cmioDevices getBytes:&cmioDevice range:NSMakeRange(i * sizeof(CMIOObjectID), sizeof(CMIOObjectID))];

            address.mSelector = kCMIODevicePropertyDeviceUID;
            UInt32 device_name_size;
            CMIOObjectGetPropertyDataSize(cmioDevice, &address, 0, NULL, &device_name_size);
            CFStringRef uid;
            CMIOObjectGetPropertyData(cmioDevice, &address, 0, NULL, device_name_size, &used, &uid);
            CFUUIDRef deviceUUID = CFUUIDCreateFromString(kCFAllocatorDefault, uid);

            // `uid` is some OTHER vendor's kCMIODevicePropertyDeviceUID — every camera on the
            // machine passes through here, including ones this build knows nothing about. When it is
            // not parseable as a UUID (an empty UID string is the case observed to do this)
            // CFUUIDCreateFromString returns NULL, and both CFEqual(_, NULL) and CFRelease(NULL) then
            // trap inside CoreFoundation. The three checks above were added to keep exactly that
            // NULL out of CFEqual's LEFT operand; this is the same trap on the right one, reached
            // once per camera per virtual-camera start.
            if (!deviceUUID) {
                CFRelease(uid);
                continue;
            }

            if (CFEqual(OBSVirtualCamUUID, deviceUUID)) {
                vcam->deviceID = cmioDevice;
                CFRelease(uid);
                CFRelease(deviceUUID);
                break;
            } else {
                CFRelease(uid);
                CFRelease(deviceUUID);
            }
        }
        CFRelease(OBSVirtualCamUUID);

        if (!vcam->deviceID) {
            obs_output_set_last_error(vcam->output, obs_module_text("Error.SystemExtension.CameraUnavailable"));
            return false;
        }

        address.mSelector = kCMIODevicePropertyStreams;
        CMIOObjectGetPropertyDataSize(vcam->deviceID, &address, 0, NULL, &size);
        NSMutableData *streamIds = [NSMutableData dataWithLength:size];
        void *stream_data = [streamIds mutableBytes];
        CMIOObjectGetPropertyData(vcam->deviceID, &address, 0, NULL, size, &used, stream_data);

        if (size < (2 * sizeof(CMIOStreamID))) {
            obs_output_set_last_error(vcam->output, obs_module_text("Error.SystemExtension.CameraNotStarted"));
            return false;
        }
        [streamIds getBytes:&vcam->streamID range:NSMakeRange(sizeof(CMIOStreamID), sizeof(CMIOStreamID))];

        CMIOStreamCopyBufferQueue(
            vcam->streamID, [](CMIOStreamID, void *, void *) {
            }, NULL, &vcam->queue);
        CMVideoFormatDescriptionCreate(kCFAllocatorDefault, video_format, vcam->videoInfo.output_width,
                                       vcam->videoInfo.output_height, NULL, &vcam->formatDescription);

        OSStatus result = CMIODeviceStartStream(vcam->deviceID, vcam->streamID);

        if (result != noErr) {
            obs_output_set_last_error(vcam->output, obs_module_text("Error.SystemExtension.CameraNotStarted"));
            return false;
        }
    } else {
        [vcam->machServer run];
    }

    if (!obs_output_begin_data_capture(vcam->output, 0)) {
        return false;
    }

    return true;
}

static void virtualcam_output_stop(void *data, uint64_t ts)
{
    UNUSED_PARAMETER(ts);

    struct virtualcam_data *vcam = (struct virtualcam_data *) data;

    obs_output_end_data_capture(vcam->output);
    if (cmio_extension_supported()) {
        CMIODeviceStopStream(vcam->deviceID, vcam->streamID);
        CFRelease(vcam->formatDescription);
    } else {
        [vcam->machServer stop];
    }
    CVPixelBufferPoolRelease(vcam->pool);
}

static void virtualcam_output_raw_video(void *data, struct video_data *frame)
{
    struct virtualcam_data *vcam = (struct virtualcam_data *) data;

    CVPixelBufferRef frameRef = nil;
    CVReturn status = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, vcam->pool, &frameRef);

    if (status != kCVReturnSuccess) {
        blog(LOG_ERROR, "unable to allocate pixel buffer (error %d)", status);
        return;
    }

    // Copy all planes into pixel buffer
    size_t planeCount = CVPixelBufferGetPlaneCount(frameRef);
    CVPixelBufferLockBaseAddress(frameRef, 0);

    if (planeCount == 0) {
        uint8_t *src = frame->data[0];
        uint8_t *dst = (uint8_t *) CVPixelBufferGetBaseAddress(frameRef);

        size_t destBytesPerRow = CVPixelBufferGetBytesPerRow(frameRef);
        size_t srcBytesPerRow = frame->linesize[0];
        size_t height = CVPixelBufferGetHeight(frameRef);

        // Sometimes CVPixelBufferCreate will create a pixel buffer that's a different
        // size than necessary to hold the frame (probably for some optimization reason).
        // If that is the case this will do a row-by-row copy into the buffer.
        if (destBytesPerRow == srcBytesPerRow) {
            memcpy(dst, src, destBytesPerRow * height);
        } else {
            for (int line = 0; (size_t) line < height; line++) {
                memcpy(dst, src, srcBytesPerRow);
                src += srcBytesPerRow;
                dst += destBytesPerRow;
            }
        }
    } else {
        for (size_t plane = 0; plane < planeCount; plane++) {
            uint8_t *src = frame->data[plane];

            if (!src) {
                blog(LOG_WARNING, "Video data from OBS contains less planes than CVPixelBuffer");
                break;
            }

            uint8_t *dst = (uint8_t *) CVPixelBufferGetBaseAddressOfPlane(frameRef, plane);

            size_t destBytesPerRow = CVPixelBufferGetBytesPerRowOfPlane(frameRef, plane);
            size_t srcBytesPerRow = frame->linesize[plane];
            size_t height = CVPixelBufferGetHeightOfPlane(frameRef, plane);

            if (destBytesPerRow == srcBytesPerRow) {
                memcpy(dst, src, destBytesPerRow * height);
            } else {
                for (int line = 0; (size_t) line < height; line++) {
                    memcpy(dst, src, srcBytesPerRow);
                    src += srcBytesPerRow;
                    dst += destBytesPerRow;
                }
            }
        }
    }

    CVPixelBufferUnlockBaseAddress(frameRef, 0);

    if (cmio_extension_supported()) {
        CMSampleBufferRef sampleBuffer;
        CMSampleTimingInfo timingInfo {.presentationTimeStamp = CMTimeMake(frame->timestamp, NSEC_PER_SEC)};

        CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, frameRef, true, NULL, NULL, vcam->formatDescription,
                                           &timingInfo, &sampleBuffer);
        CMSimpleQueueEnqueue(vcam->queue, sampleBuffer);
    } else {
        // Share pixel buffer with clients
        [vcam->machServer sendPixelBuffer:frameRef timestamp:frame->timestamp fpsNumerator:vcam->videoInfo.fps_num
                           fpsDenominator:vcam->videoInfo.fps_den];
    }

    CVPixelBufferRelease(frameRef);
}

struct obs_output_info virtualcam_output_info = {
    .id = "virtualcam_output",
    .flags = OBS_OUTPUT_VIDEO,
    .get_name = virtualcam_output_get_name,
    .create = virtualcam_output_create,
    .destroy = virtualcam_output_destroy,
    .start = virtualcam_output_start,
    .stop = virtualcam_output_stop,
    .raw_video = virtualcam_output_raw_video,
};

bool obs_module_load(void)
{
    obs_register_output(&virtualcam_output_info);

    return true;
}

#import <Cocoa/Cocoa.h>

#import "utils.h"
#import "qlgen-client.h"
#import "system-qlgen.h"
#import "qlgen.h"


@interface SFQLGenImpl : NSObject<SFQLGen>

@property SFQLGenClient *client;
@property (copy) NSString *mountPoint;

+ (id)sharedInstance;
- (id)init;
- (BOOL)getMountPoint:(NSString **)mountPoint;
- (BOOL)isFileCached:(NSString *)path;
- (BOOL)askForThumbnail:(NSString *)path output:(NSString **)output;
- (OSStatus)genThumnail:(QLThumbnailRequestRef)thumbnail
                    url:(CFURLRef)url
         contentTypeUTI:(CFStringRef)contentTypeUTI
                options:(CFDictionaryRef)options
                maxSize:(CGSize)maxSize;
- (OSStatus)genPreview:(QLPreviewRequestRef)preview
                    url:(CFURLRef)url
         contentTypeUTI:(CFStringRef)contentTypeUTI
                options:(CFDictionaryRef)options;
@end

@implementation SFQLGenImpl
+ (id)sharedInstance
{
    static SFQLGenImpl *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
      instance = [SFQLGenImpl new];
    });
    return instance;
}

- (id)init
{
    _client = [SFQLGenClient new];
    return self;
}

- (BOOL)getMountPoint:(NSString **)mountPoint
{
    return [self.client getMountPoint:mountPoint];
}

- (BOOL)isFileCached:(NSString *)path output:(BOOL *)output
{
    return [self.client isFileCached:path output:output];
}

- (OSStatus)genThumnail:(QLThumbnailRequestRef)thumbnail
                    url:(CFURLRef)url
         contentTypeUTI:(CFStringRef)contentTypeUTI
                options:(CFDictionaryRef)options
                maxSize:(CGSize)maxSize
{
    NSString *path = ((__bridge NSURL *)url).path;

    DbgLog(@"SFQLGenImpl::genThumnail is called for %@", path);
    // TODO: cached location of mount point should expire, in case the
    // mount point could be changed on searive-gui side.
    if (!self.mountPoint) {
        NSString *mountPoint;
        if (![self getMountPoint:&mountPoint]) {
            return NO;
        }
        self.mountPoint = mountPoint;
    }

    if (QLThumbnailRequestIsCancelled(thumbnail))
        return noErr;

    // Invoke system qlgen when either 1) file is not in seadrive, or
    // 2) file is in seadrive but is already cached.
    BOOL useSystemQLGen =
        ![path hasPrefix:self.mountPoint] || [self isFileCached:path];
    if (useSystemQLGen) {
        DbgLog(@"calling system qlgen for file %@", path);
        SystemQLGen *system = [SystemQLGen sharedInstance];
        return [system genThumnail:thumbnail
                               url:url
                    contentTypeUTI:contentTypeUTI
                           options:options
                           maxSize:maxSize];
    } else {
        NSString *png;
        if ([self askForThumbnail:path output:&png]) {
            DbgLog(@"use api generated thumbnail at path %@", png);
            NSURL *pngURL = SFPathToURL(png);
            QLThumbnailRequestSetImageAtURL(thumbnail, (__bridge CFURLRef)pngURL, nil);
            return noErr;
        } else {
            DbgLog(@"Failed to ask for thumbnail for file %@", path);
        }
    }

    return noErr;
}

- (OSStatus)genPreview:(QLPreviewRequestRef)preview
                    url:(CFURLRef)url
         contentTypeUTI:(CFStringRef)contentTypeUTI
                options:(CFDictionaryRef)options
{
    NSString *path = ((__bridge NSURL *)url).path;

    DbgLog(@"SFQLGenImpl::genPreview is called for %@", path);
    DbgLog(@"calling system qlgen for file %@", path);
    SystemQLGen *system = [SystemQLGen sharedInstance];
    return [system genPreview:preview
                          url:url
               contentTypeUTI:contentTypeUTI
                      options:options];

    return noErr;
}

- (BOOL)isFileCached:(NSString *)path
{
    BOOL cached = NO;
    if (![self.client isFileCached:path output:&cached]) {
        return NO;
    }
    DbgLog(@"file %@ is %s", path, cached? "cached" : "not cached");
    return cached;
}

- (BOOL)askForThumbnail:(NSString *)path output:(NSString **)output
{
    // For now we display no thumbnail for not-cached files.
    return NO;
    // return [self.client askForThumbnail:path output:output];
}
@end


extern "C" {

id<SFQLGen> getDefaultSFQLGen() {
    return [SFQLGenImpl sharedInstance];
}

}

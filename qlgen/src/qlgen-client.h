// -*- mode: objc -*-

#import <Foundation/Foundation.h>

// Interacts with seadrive-gui
@interface SFQLGenClient : NSObject
- (BOOL)getMountPoint:(NSString **)mountPoint;
- (BOOL)isFileCached:(NSString *)path output:(BOOL *)output;
- (BOOL)askForThumbnail:(NSString *)path output:(NSString **)output;
@end

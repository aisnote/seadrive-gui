#include <AvailabilityMacros.h>
#import <Cocoa/Cocoa.h>
#include <QtGlobal>

#import "helper-client.h"
#import "utils/objc-defines.h"

#if !__has_feature(objc_arc)
#error this file must be built with ARC support
#endif

HelperClient::HelperClient()
{
    xpc_client_ = nullptr;
}

void HelperClient::connect()
{
    xpc_client_ = [[MPXPCClient alloc]
        initWithServiceName:@"com.seafile.seadrive.helper"
                 privileged:YES
                readOptions:MPMessagePackReaderOptionsUseOrderedDictionary];
    xpc_client_.retryMaxAttempts = 4;
    xpc_client_.retryDelay = 0.5;
    xpc_client_.timeout = 10.0;
}

void HelperClient::getVersion()
{
    ensureConnected();
    [xpc_client_ sendRequest:@"version"
                      params:nil
                  completion:^(NSError *error, NSDictionary *versions) {
                    if (error) {
                        printf("get version error!\n");
                        NSLog(@"error: %@", [error userInfo]);
                    } else {
                        printf("get version success!\n");
                        NSLog(@"Helper version: %@", versions);
                    }
                  }];
}

void HelperClient::ensureConnected()
{
    if (!xpc_client_) {
        connect();
    }
}

bool HelperClient::installKext(bool *finished, bool *ok)
{
    ensureConnected();

    NSString *source =
        @"/Applications/SeaDrive.app/Contents/Resources/osxfuse.fs";
    NSString *destination = @"/Library/Filesystems/osxfuse.fs";
    NSString *kextID = @"com.github.osxfuse.filesystems.osxfuse";
    NSString *kextPath = @"/Library/Filesystems/osxfuse.fs/Contents/Extensions/"
                         @"10.11/osxfuse.kext";
    NSDictionary *params = @{
        @"source" : source,
        @"destination" : destination,
        @"kextID" : kextID,
        @"kextPath" : kextPath
    };

    [xpc_client_
        sendRequest:@"kextInstall"
             params:@[ params ]
         completion:^(NSError *error, id value) {
           *finished = true;
           if (error) {
               qWarning("error when kextInstall: %s", NSERROR_TO_CSTR(error));
               *ok = false;
           } else {
               qWarning("kextInstall success!");
               *ok = true;
           }
         }];

    return true;
}

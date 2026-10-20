#import <Foundation/Foundation.h>

typedef NS_ENUM(NSInteger, WADevicePlatform) {
    WADevicePlatformUnknown = 0,
    WADevicePlatformMacOS = 1,
    WADevicePlatformWindows = 2,
    WADevicePlatformLinux = 3,
    WADevicePlatformiOS = 4,
    WADevicePlatformAndroid = 5,
    WADevicePlatformWeb = 6
};

@interface WADeviceStore : NSObject

+ (instancetype)shared;

// Device Management
- (NSArray *)pairedDevices; // Array of device dictionaries
- (void)addDevice:(NSString *)deviceID 
             name:(NSString *)name 
         platform:(WADevicePlatform)platform
    encryptionKey:(NSData *)key;
- (void)removeDevice:(NSString *)deviceID;
- (void)updateDeviceLastSeen:(NSString *)deviceID;

// Device Info
- (NSDictionary *)deviceWithID:(NSString *)deviceID;
- (NSString *)platformNameForPlatform:(WADevicePlatform)platform;

@end
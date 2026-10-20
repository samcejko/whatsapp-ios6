#import "WADeviceStore.h"
#import "WACommon.h"

@implementation WADeviceStore {
    NSMutableDictionary *_devices; // deviceID -> device info dict
    NSString *_storePath;
}

+ (instancetype)shared {
    static WADeviceStore *s = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        s = [[WADeviceStore alloc] init];
    });
    return s;
}

- (instancetype)init {
    if (self = [super init]) {
        _storePath = [WADocumentsPath() stringByAppendingPathComponent:@"devices.plist"];
        _devices = [NSMutableDictionary dictionaryWithContentsOfFile:_storePath] ?: [NSMutableDictionary new];
    }
    return self;
}

- (void)save {
    [_devices writeToFile:_storePath atomically:YES];
    [[NSNotificationCenter defaultCenter] postNotificationName:@"WADeviceStoreChanged" object:nil];
}

- (NSArray *)pairedDevices {
    return [_devices allValues];
}

- (void)addDevice:(NSString *)deviceID 
             name:(NSString *)name 
         platform:(WADevicePlatform)platform
    encryptionKey:(NSData *)key {
    
    _devices[deviceID] = @{
        @"id": deviceID,
        @"name": name ?: @"Unknown Device",
        @"platform": @(platform),
        @"encryptionKey": key ?: [NSData data],
        @"lastSeen": @([NSDate timeIntervalSinceReferenceDate]),
        @"pairedAt": @([NSDate timeIntervalSinceReferenceDate])
    };
    
    [self save];
}

- (void)removeDevice:(NSString *)deviceID {
    [_devices removeObjectForKey:deviceID];
    [self save];
}

- (void)updateDeviceLastSeen:(NSString *)deviceID {
    NSMutableDictionary *device = [_devices[deviceID] mutableCopy];
    if (device) {
        device[@"lastSeen"] = @([NSDate timeIntervalSinceReferenceDate]);
        _devices[deviceID] = device;
        [self save];
    }
}

- (NSDictionary *)deviceWithID:(NSString *)deviceID {
    return _devices[deviceID];
}

- (NSString *)platformNameForPlatform:(WADevicePlatform)platform {
    switch (platform) {
        case WADevicePlatformMacOS: return @"macOS";
        case WADevicePlatformWindows: return @"Windows";
        case WADevicePlatformLinux: return @"Linux";
        case WADevicePlatformiOS: return @"iOS";
        case WADevicePlatformAndroid: return @"Android";
        case WADevicePlatformWeb: return @"Web";
        default: return @"Unknown";
    }
}

@end
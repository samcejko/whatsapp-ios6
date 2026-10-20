#import <Foundation/Foundation.h>

@interface WARelayFallback : NSObject

+ (instancetype)sharedInstance;

// Load TSV configuration from app bundle
- (BOOL)loadConfiguration;

// Try fallback connection using provider at index
- (BOOL)tryFallbackConnectionAtIndex:(NSInteger)index error:(NSError **)error;

// Get array of provider names
- (NSArray<NSString *> *)providerNames;

// Get provider info dictionary
- (NSDictionary *)providerInfoAtIndex:(NSInteger)index;

@end
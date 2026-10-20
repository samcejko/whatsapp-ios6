#import "WARelayFallback.h"
#import "WACommon.h"

@interface WARelayFallback ()
@property (nonatomic, strong) NSMutableArray *providers; // Array of NSDictionary
@end

@implementation WARelayFallback

+ (instancetype)sharedInstance {
    static WARelayFallback *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[WARelayFallback alloc] init];
    });
    return shared;
}

- (instancetype)init {
    if (self = [super init]) {
        _providers = [NSMutableArray array];
    }
    return self;
}

- (BOOL)loadConfiguration {
    NSString *tsvPath = [[NSBundle mainBundle] pathForResource:@"domain_fronting_providers"
                                                           ofType:@"tsv"
                                                      inDirectory:@"config"];
    if (!tsvPath) {
        WALog(@"Fallback TSV not found in bundle");
        return NO;
    }

    // Use C parser
    wa_fallback_list list = {0};
    int count = wa_fallback_load([tsvPath UTF8String], &list);
    if (count <= 0) {
        WALog(@"Failed to load fallback providers");
        return NO;
    }

    [_providers removeAllObjects];
    for (int i = 0; i < count; i++) {
        wa_fallback_provider *p = &list.providers[i];
        NSDictionary *dict = @{
            @"name": [NSString stringWithUTF8String:p->provider_name],
            @"url_template": [NSString stringWithUTF8String:p->url_template],
            @"domain": [NSString stringWithUTF8String:p->domain]
        };
        [_providers addObject:dict];
    }

    WALog(@"Loaded %d fallback providers", (int)_providers.count);
    return YES;
}

- (NSArray<NSString *> *)providerNames {
    return [_providers valueForKey:@"name"];
}

- (NSDictionary *)providerInfoAtIndex:(NSInteger)index {
    if (index < 0 || index >= _providers.count) return nil;
    return _providers[index];
}

- (BOOL)tryFallbackConnectionAtIndex:(NSInteger)index error:(NSError **)error {
    if (index < 0 || index >= _providers.count) {
        if (error) *error = [NSError errorWithDomain:@"WARelayFallback" code:1 userInfo:@{NSLocalizedDescriptionKey: @"Invalid provider index"}];
        return NO;
    }

    NSDictionary *provider = _providers[index];
    NSString *name = provider[@"name"];
    NSString *urlTemplate = provider[@"url_template"];
    NSString *domain = provider[@"domain"];

    WALog(@"Trying fallback provider: %@ (%@)", name, domain);

    // For now, just log the attempt. Actual connection logic would go here.
    // This would integrate with wa_net.c / wa_relay.c to use the fallback endpoint.
    
    // Simulate connection attempt
    // In real implementation, this would call into the C layer to establish connection
    // using the fallback endpoint instead of the primary relay.
    
    WALog(@"Fallback connection to %@ would be attempted here", urlTemplate);
    
    // For now, return NO to indicate not actually connected
    // Real implementation would return YES on success
    if (error) {
        *error = [NSError errorWithDomain:@"WARelayFallback" code:2 userInfo:@{NSLocalizedDescriptionKey: @"Fallback connection not fully implemented"}];
    }
    return NO;
}

@end
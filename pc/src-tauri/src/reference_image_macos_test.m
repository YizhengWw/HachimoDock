// Compile separately with clang; synthetic local fixtures only, no personal library or API calls.
#import "reference_image_macos.m"

static int failures;
static void Check(BOOL condition, NSString *name) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", name.UTF8String);
    if (!condition) failures++;
}
static NSData *Fixture(CFStringRef type, BOOL alpha, BOOL oriented) {
    CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(NULL, 80, 40, 8, 80 * 4, color,
        alpha ? kCGImageAlphaPremultipliedLast : kCGImageAlphaNoneSkipLast);
    CGContextSetRGBFillColor(ctx, 0.8, 0.2, 0.1, alpha ? 0.5 : 1);
    CGContextFillRect(ctx, CGRectMake(0, 0, 80, 40));
    CGImageRef image = CGBitmapContextCreateImage(ctx);
    NSMutableData *data = [NSMutableData data];
    CGImageDestinationRef dest = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, type, 1, NULL);
    if (dest) {
        CGImageDestinationAddImage(dest, image, (__bridge CFDictionaryRef)@{
            (id)kCGImagePropertyOrientation: oriented ? @6 : @1,
            (id)kCGImagePropertyGPSDictionary: @{(id)kCGImagePropertyGPSLatitude:@12.3, (id)kCGImagePropertyGPSLatitudeRef:@"N"},
            (id)kCGImagePropertyExifDictionary: @{(id)kCGImagePropertyExifUserComment:@"PRIVATE-FIXTURE"}
        });
        if (!CGImageDestinationFinalize(dest)) data = nil;
        CFRelease(dest);
    } else data = nil;
    CGImageRelease(image); CGContextRelease(ctx); CGColorSpaceRelease(color);
    return data;
}
static NSMutableArray *Events;
@interface FakeResult : NSObject
@property(strong) NSItemProvider *itemProvider;
@end
@implementation FakeResult
@end
@interface RestrictedPhotoProvider : NSItemProvider
@property(strong) NSData *fixture;
@property BOOL failData;
@property BOOL delayData;
@property BOOL objectSuccess;
@property(copy) void (^pendingData)(NSData *,NSError *);
@property(strong) NSMutableArray *requests;
@end
@implementation RestrictedPhotoProvider
- (NSArray *)registeredTypeIdentifiers {return @[@"public.jpeg",@"com.apple.private.photokit.asset-bundle",@"public.movie"];}
- (BOOL)hasItemConformingToTypeIdentifier:(NSString *)type {return [type isEqual:@"public.image"];}
- (BOOL)canLoadObjectOfClass:(Class)aClass {return self.objectSuccess && aClass==NSImage.class;}
- (NSProgress *)loadObjectOfClass:(Class)aClass completionHandler:(void (^)(__kindof id<NSItemProviderReading>,NSError *))completion {
    [self.requests addObject:@"object:NSImage"];completion([[NSImage alloc] initWithData:self.fixture],nil);return [NSProgress progressWithTotalUnitCount:1];
}
- (NSProgress *)loadFileRepresentationForTypeIdentifier:(NSString *)type completionHandler:(void (^)(NSURL *,NSError *))completion {
    [self.requests addObject:[@"file:" stringByAppendingString:type]];
    completion(nil,[NSError errorWithDomain:NSItemProviderErrorDomain code:-1000 userInfo:@{NSLocalizedDescriptionKey:@"PRIVATE-ASSET-NAME",NSFilePathErrorKey:@"/private/user/photo.jpg"}]);return [NSProgress progressWithTotalUnitCount:1];
}
- (NSProgress *)loadDataRepresentationForTypeIdentifier:(NSString *)type completionHandler:(void (^)(NSData *,NSError *))completion {
    [self.requests addObject:[@"data:" stringByAppendingString:type]];
    if(self.delayData)self.pendingData=completion;
    else completion(self.failData?nil:self.fixture,self.failData?[NSError errorWithDomain:NSCocoaErrorDomain code:260 userInfo:@{NSFilePathErrorKey:@"/private/user/photo.jpg"}]:nil);
    return [NSProgress progressWithTotalUnitCount:1];
}
@end
@interface PromiseFixture : NSFilePromiseReceiver
@property(strong) NSData *fixture;
@end
@implementation PromiseFixture
- (NSArray *)fileNames { return @[]; }
- (NSArray *)fileTypes { return @[@"public.heic"]; }
- (void)receivePromisedFilesAtDestination:(NSURL *)destination options:(NSDictionary *)options operationQueue:(NSOperationQueue *)queue reader:(void (^)(NSURL *, NSError *))reader {
    [queue addOperationWithBlock:^{ NSURL *url=[destination URLByAppendingPathComponent:@"fixture.heic"]; [self.fixture writeToURL:url atomically:YES]; reader(url,nil); }];
}
@end
@interface PasteboardFixture : NSObject
@property(strong) id promise;
@end
@implementation PasteboardFixture
- (BOOL)canReadObjectForClasses:(NSArray *)classes options:(NSDictionary *)options { return YES; }
- (NSArray *)readObjectsForClasses:(NSArray *)classes options:(NSDictionary *)options { return [classes containsObject:NSFilePromiseReceiver.class] ? @[self.promise] : @[]; }
@end
@interface DragFixture : NSObject
@property(strong) id draggingPasteboard;
@end
@implementation DragFixture
@end
static void Drain(ReferenceDropView *view) {
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:10];
    while (view.busy && deadline.timeIntervalSinceNow > 0) [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
}
static void Event(const char *json) {
    NSDictionary *data = [NSJSONSerialization JSONObjectWithData:[[@(json) dataUsingEncoding:NSUTF8StringEncoding] copy] options:0 error:nil];
    [Events addObject:data];
}
int main(void) {
    @autoreleasepool {
        for (NSString *type in @[@"public.jpeg", @"public.png", @"public.heic", @"public.tiff", @"com.compuserve.gif"]) {
            NSData *input = Fixture((__bridge CFStringRef)type, [type isEqual:@"public.png"], NO);
            Check(input != nil, [type stringByAppendingString:@" fixture"]);
            NSDictionary *result = NormalizeReference(input);
            Check(result[@"base64"] != nil, [type stringByAppendingString:@" normalize"]);
            NSData *output = [[NSData alloc] initWithBase64EncodedString:result[@"base64"] ?: @"" options:0];
            CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)output, NULL);
            NSDictionary *props = source ? CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, NULL)) : nil;
            Check(props && !props[(id)kCGImagePropertyGPSDictionary] && !props[(id)kCGImagePropertyExifDictionary][(id)kCGImagePropertyExifUserComment], [type stringByAppendingString:@" strips private metadata"]);
            if (source) CFRelease(source);
            if ([type isEqual:@"public.png"]) Check([result[@"mime"] isEqual:@"image/png"], @"alpha preserved as PNG");
        }
        NSDictionary *oriented = NormalizeReference(Fixture(CFSTR("public.jpeg"), NO, YES));
        NSData *bytes = [[NSData alloc] initWithBase64EncodedString:oriented[@"base64"] options:0];
        CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)bytes, NULL);
        NSDictionary *props = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, NULL));
        Check([props[(id)kCGImagePropertyPixelWidth] intValue] == 40 && [props[(id)kCGImagePropertyPixelHeight] intValue] == 80, @"EXIF orientation flattened");
        CFRelease(source);
        Check(NormalizeReference([@"not an image" dataUsingEncoding:NSUTF8StringEncoding])[@"error"] != nil, @"corrupt data rejected");
        Check(NormalizeReference([NSMutableData dataWithLength:InputLimit+1])[@"error"] != nil, @"oversize rejected before decode");
        Check(NormalizeReference(nil)[@"error"] != nil, @"empty rejected");
        Check(ReadReference([NSURL URLWithString:@"https://example.invalid/private.jpg"]) == nil, @"remote URL never fetched");
        // The import state machine is exercised without opening a window or reading the library.
        Events = [NSMutableArray array];
        ReferenceDropView *view = [[ReferenceDropView alloc] initWithFrame:NSZeroRect];
        view.token = @"test"; view.active = YES; view.callback = Event;
        NSUInteger old = [view begin]; [view cancel]; [view finish:@{@"mime":@"image/png", @"base64":@"AA=="} generation:old];
        Check([Events.lastObject[@"status"] isEqual:@"cancelled"], @"cancel ignores late success");
        NSUInteger current = [view begin]; [view finish:@{@"error":@"fixture error"} generation:current];
        Check([Events.lastObject[@"status"] isEqual:@"error"] && !view.busy, @"error releases busy state");
        NSUInteger count = Events.count; view.active = NO; [view finish:@{} generation:current];
        Check(Events.count == count, @"unmounted target ignores events");
        view.active=YES;view.queue=[NSOperationQueue new];view.queue.maxConcurrentOperationCount=1;
        PromiseFixture *promise=[PromiseFixture new];promise.fixture=Fixture(CFSTR("public.heic"),NO,NO);
        PasteboardFixture *pasteboard=[PasteboardFixture new];pasteboard.promise=promise;
        DragFixture *drag=[DragFixture new];drag.draggingPasteboard=pasteboard;
        Check([view performDragOperation:(id)drag], @"file promise accepted before filename is available");
        NSURL *temporary=view.temporaryDirectory;
        Drain(view);
        if (Events.lastObject[@"error"]) printf("promise diagnostic: %s\n", [Events.lastObject[@"error"] UTF8String]);
        Check([Events.lastObject[@"status"] isEqual:@"ready"] && [@[@"image/jpeg", @"image/png"] containsObject:Events.lastObject[@"mime"]], @"promised HEIC normalizes and completes");
        Check(![NSFileManager.defaultManager fileExistsAtPath:temporary.path], @"promise temporary file removed");
        if (@available(macOS 13.0, *)) {
            NSItemProvider *provider=[NSItemProvider new];
            NSData *heic=Fixture(CFSTR("public.heic"),NO,NO);
            [provider registerDataRepresentationForTypeIdentifier:@"public.heic" visibility:NSItemProviderRepresentationVisibilityAll loadHandler:^NSProgress *(void (^completion)(NSData *, NSError *)) { completion(heic,nil);return nil; }];
            FakeResult *result=[FakeResult new];result.itemProvider=provider;
            [view picker:nil didFinishPicking:(id)@[result]];Drain(view);
            Check([Events.lastObject[@"status"] isEqual:@"ready"], @"Photos item-provider HEIC loading path");
            RestrictedPhotoProvider *restricted=[RestrictedPhotoProvider new];restricted.fixture=Fixture(CFSTR("public.jpeg"),NO,NO);restricted.requests=[NSMutableArray array];
            result.itemProvider=restricted;[view picker:nil didFinishPicking:(id)@[result]];Drain(view);
            Check([Events.lastObject[@"status"] isEqual:@"ready"],@"file unavailable falls back to actual JPEG data");
            Check([restricted.requests isEqual:@[@"file:public.jpeg",@"data:public.jpeg"]],@"concrete image format precedes abstract type; no video/private asset requests");
            NSArray *attempts=Events.lastObject[@"diagnostic"][@"attempts"];
            Check(attempts.count==2 && [attempts[0][@"result"] isEqual:@"NSItemProviderErrorDomain/-1000"] && [attempts[1][@"result"] isEqual:@"ok"],@"success retains bounded fallback diagnostics");
            restricted.failData=YES;[restricted.requests removeAllObjects];[view picker:nil didFinishPicking:(id)@[result]];Drain(view);
            Check([Events.lastObject[@"status"] isEqual:@"error"] && restricted.requests.count==4,@"all representations fail once then release busy state");
            NSString *diagnostic=[Events.lastObject description];
            Check(![diagnostic containsString:@"PRIVATE-ASSET"] && ![diagnostic containsString:@"/private/user"] && [diagnostic containsString:@"NSCocoaErrorDomain/260"],@"diagnostics preserve codes but exclude asset names and paths");
            restricted.objectSuccess=YES;[restricted.requests removeAllObjects];[view picker:nil didFinishPicking:(id)@[result]];Drain(view);
            Check([Events.lastObject[@"status"] isEqual:@"ready"] && [restricted.requests.lastObject isEqual:@"object:NSImage"],@"native NSImage fallback normalizes locally when file/data fail");
            restricted.objectSuccess=NO;
            restricted.failData=NO;restricted.delayData=YES;[view picker:nil didFinishPicking:(id)@[result]];
            NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:2];
            while(!restricted.pendingData&&deadline.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            [view cancel];restricted.pendingData(restricted.fixture,nil);
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
            Check([Events.lastObject[@"status"] isEqual:@"cancelled"] && !view.photoProvider,@"late fallback ignored after cancel and provider released");
            [view picker:nil didFinishPicking:@[]];
            Check([Events.lastObject[@"status"] isEqual:@"cancelled"], @"Photos picker cancellation");
        }
        printf("Failures: %d\n", failures);
    }
    return failures ? 1 : 0;
}

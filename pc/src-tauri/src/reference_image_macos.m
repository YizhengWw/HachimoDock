// User-selected reference images only. No Photos library enumeration or cloud/model requests.
// ImageIO flattens orientation and removes source metadata; original files are never modified.
#import <AppKit/AppKit.h>
#import <PhotosUI/PhotosUI.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <ImageIO/ImageIO.h>
#import <objc/runtime.h>

typedef void (*ReferenceEvent)(const char *);
static const NSUInteger InputLimit = 32 * 1024 * 1024;
static const NSUInteger OutputLimit = 16 * 1024 * 1024;
static char ReferenceAssociation;
static NSNumber *ReferenceFreeMiB(void) {
    NSDictionary *attributes=[NSFileManager.defaultManager attributesOfFileSystemForPath:NSHomeDirectory() error:nil];
    NSNumber *bytes=attributes[NSFileSystemFreeSize];
    return bytes?@(bytes.unsignedLongLongValue/(1024*1024)):nil;
}

// Domain/code only: NSError descriptions/userInfo can contain private file paths or asset IDs.
static NSString *ReferenceErrorCode(NSError *error) {
    if (!error) return @"no-error/no-file";
    NSMutableArray *parts = [NSMutableArray array];
    for (NSUInteger depth=0; error && depth<3; depth++) {
        NSString *domain = error.domain ?: @"unknown";
        NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"];
        if (domain.length>100 || [domain rangeOfCharacterFromSet:allowed.invertedSet].location!=NSNotFound) domain=@"other-domain";
        [parts addObject:[NSString stringWithFormat:@"%@/%ld",domain,(long)error.code]];
        id underlying=error.userInfo[NSUnderlyingErrorKey];
        error=[underlying isKindOfClass:NSError.class]?underlying:nil;
    }
    return [parts componentsJoinedByString:@" > "];
}

static NSArray<NSString *> *ReferencePhotoTypes(NSItemProvider *provider) {
    NSMutableArray *types=[NSMutableArray array];
    NSArray *decodable=CFBridgingRelease(CGImageSourceCopyTypeIdentifiers());
    for (NSString *type in provider.registeredTypeIdentifiers) {
        if ([decodable containsObject:type] && ![types containsObject:type]) [types addObject:type];
        if (types.count==4) break;
    }
    // Prefer actual representations advertised by Photos, not only an abstract parent UTI.
    if ([provider hasItemConformingToTypeIdentifier:@"public.image"]) [types addObject:@"public.image"];
    if ([provider canLoadObjectOfClass:NSImage.class]) [types addObject:@"NSImage"];
    return types;
}

static NSData *ReferenceImageObjectData(NSImage *image) {
    NSSize size=image.size;
    if(!isfinite(size.width)||!isfinite(size.height)||size.width<=0||size.height<=0||size.width*size.height>100000000)return nil;
    double scale=MIN(1.,2048./MAX(size.width,size.height));
    NSInteger width=MAX(1,lround(size.width*scale)),height=MAX(1,lround(size.height*scale));
    NSBitmapImageRep *bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
    if(!bitmap)return nil;
    [NSGraphicsContext saveGraphicsState];NSGraphicsContext.currentContext=[NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];
    [image drawInRect:NSMakeRect(0,0,width,height) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1];
    [NSGraphicsContext restoreGraphicsState];
    return [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
}

static NSDictionary *NormalizeReference(NSData *data) {
    if (!data.length || data.length > InputLimit) return @{ @"error": @"图片为空或超过 32 MB，请先导出较小的图片。" };
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data,
        (__bridge CFDictionaryRef)@{(id)kCGImageSourceShouldCache: @NO});
    if (!source) return @{ @"error": @"无法解码这张图片，请从“照片”导出为 JPEG 或 PNG 后重试。" };
    NSDictionary *props = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, NULL));
    double width = [props[(id)kCGImagePropertyPixelWidth] doubleValue];
    double height = [props[(id)kCGImagePropertyPixelHeight] doubleValue];
    if (width <= 0 || height <= 0 || width * height > 100000000) {
        CFRelease(source);
        return @{ @"error": @"图片尺寸无效或超过一亿像素，请缩小后重试。" };
    }
    CGImageRef image = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)@{
        (id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
        (id)kCGImageSourceCreateThumbnailWithTransform: @YES,
        (id)kCGImageSourceThumbnailMaxPixelSize: @2048,
        (id)kCGImageSourceShouldCacheImmediately: @YES
    });
    CFRelease(source);
    if (!image) return @{ @"error": @"图片解码失败；若原图位于 iCloud，请先下载原图后重试。" };
    CGImageAlphaInfo alpha = CGImageGetAlphaInfo(image);
    BOOL transparent = props[(id)kCGImagePropertyHasAlpha] ? [props[(id)kCGImagePropertyHasAlpha] boolValue] :
        (alpha == kCGImageAlphaFirst || alpha == kCGImageAlphaLast ||
        alpha == kCGImageAlphaPremultipliedFirst || alpha == kCGImageAlphaPremultipliedLast);
    NSMutableData *output = [NSMutableData data];
    CGImageDestinationRef dest = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)output,
        transparent ? CFSTR("public.png") : CFSTR("public.jpeg"), 1, NULL);
    if (!dest) { CGImageRelease(image); return @{ @"error": @"无法转换图片。" }; }
    CGImageDestinationAddImage(dest, image, (__bridge CFDictionaryRef)@{(id)kCGImageDestinationLossyCompressionQuality: @0.92});
    BOOL ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CGImageRelease(image);
    if (!ok || output.length > OutputLimit) return @{ @"error": @"图片转换失败或转换后文件过大。" };
    return @{ @"mime": transparent ? @"image/png" : @"image/jpeg",
              @"name": transparent ? @"reference.png" : @"reference.jpg",
              @"base64": [output base64EncodedStringWithOptions:0] };
}

// Bounded read, including providers whose advertised file size is unavailable/untrusted.
static NSData *ReadReferenceWithError(NSURL *url, NSError **error) {
    if (!url.isFileURL) return nil;
    NSNumber *regular = nil;
    [url getResourceValue:&regular forKey:NSURLIsRegularFileKey error:error];
    if (!regular.boolValue) return nil;
    NSFileHandle *file = [NSFileHandle fileHandleForReadingFromURL:url error:error];
    if (!file) return nil;
    NSData *data = [file readDataUpToLength:InputLimit + 1 error:error];
    [file closeAndReturnError:nil];
    return data;
}
static NSData *ReadReference(NSURL *url) {return ReadReferenceWithError(url,nil);}

@interface ReferenceDropView : NSView <PHPickerViewControllerDelegate, NSWindowDelegate>
@property(copy) NSString *token;
@property ReferenceEvent callback;
@property NSUInteger generation;
@property BOOL busy;
@property BOOL active;
@property(strong) NSWindow *pickerWindow;
@property(strong) NSOpenPanel *filePanel;
@property(strong) NSProgress *progress;
@property(strong) NSItemProvider *photoProvider;
@property(strong) NSMutableArray *importAttempts;
@property(strong) NSDate *importStarted;
@property NSUInteger attempt;
@property(strong) NSOperationQueue *queue;
@property(strong) NSURL *temporaryDirectory;
- (void)emit:(NSDictionary *)payload;
- (NSUInteger)begin;
- (void)finish:(NSDictionary *)payload generation:(NSUInteger)generation;
- (void)pick:(BOOL)photos;
- (void)cancel;
- (void)loadPhotoTypes:(NSArray<NSString *> *)types index:(NSUInteger)index data:(BOOL)dataMode generation:(NSUInteger)generation;
@end

@implementation ReferenceDropView
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return NO; }
- (void)mouseDown:(NSEvent *)event { [self pick:NO]; }
- (void)emit:(NSDictionary *)payload {
    NSAssert(NSThread.isMainThread, @"UI event must run on main thread");
    if (!self.active || !self.callback) return;
    NSMutableDictionary *event = [payload mutableCopy];
    event[@"token"] = self.token;
    NSData *json = [NSJSONSerialization dataWithJSONObject:event options:0 error:nil];
    NSString *text = [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];
    if (text) self.callback(text.UTF8String);
}
- (NSUInteger)begin {
    self.busy = YES;
    self.importStarted=NSDate.date;
    self.importAttempts=[NSMutableArray array];
    NSUInteger generation = ++self.generation;
    [self emit:@{@"status": @"loading"}];
    // Timeout invalidates late completions without clearing the user's previous selection.
    __weak ReferenceDropView *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        ReferenceDropView *view = weakSelf;
        if (view.active && view.busy && view.generation == generation) {
            [view.progress cancel];
            [view finish:@{@"error": @"读取照片超过 120 秒，已停止。系统未及时提供图片；请确认该照片能在“照片”中完整打开后重试。"} generation:generation];
        }
    });
    return generation;
}
- (void)finish:(NSDictionary *)payload generation:(NSUInteger)generation {
    if (!self.active || !self.busy || self.generation != generation) return;
    self.busy = NO;
    self.progress = nil;
    self.photoProvider = nil;
    [self closePicker];
    if (self.temporaryDirectory) { [NSFileManager.defaultManager removeItemAtURL:self.temporaryDirectory error:nil]; self.temporaryDirectory = nil; }
    self.generation++;
    NSMutableDictionary *event = [payload mutableCopy];
    event[@"status"] = payload[@"error"] ? @"error" : @"ready";
    if (self.importAttempts.count) {
        NSMutableDictionary *diagnostic=[@{@"attempts":[self.importAttempts copy], @"elapsedMs":@((NSInteger)(-[self.importStarted timeIntervalSinceNow]*1000))} mutableCopy];
        NSNumber *free=ReferenceFreeMiB();if(free)diagnostic[@"diskFreeMiB"]=free;
        event[@"diagnostic"]=diagnostic;
    }
    [self emit:event];
}
- (void)consumeURL:(NSURL *)url generation:(NSUInteger)generation {
    // Must run before a provider callback returns: provider URLs can expire immediately.
    BOOL scoped = [url startAccessingSecurityScopedResource];
    NSData *data = ReadReference(url);
    if (scoped) [url stopAccessingSecurityScopedResource];
    NSDictionary *result = data ? NormalizeReference(data) : @{ @"error": @"无法读取选中的照片，请确认原图已下载，或导出为 JPEG 后重试。" };
    dispatch_async(dispatch_get_main_queue(), ^{ [self finish:result generation:generation]; });
}
- (void)pick:(BOOL)photos {
    if (!self.active || self.busy || self.pickerWindow || self.filePanel || !self.window) return;
    if (photos) {
        if (@available(macOS 13.0, *)) {
            PHPickerConfiguration *config = [[PHPickerConfiguration alloc] init];
            config.filter = PHPickerFilter.imagesFilter;
            config.selectionLimit = 1;
            config.preferredAssetRepresentationMode = PHPickerConfigurationAssetRepresentationModeCurrent;
            PHPickerViewController *picker = [[PHPickerViewController alloc] initWithConfiguration:config];
            picker.delegate = self;
            NSWindow *sheet = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 760, 560)
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                backing:NSBackingStoreBuffered defer:NO];
            sheet.title = @"选择参考照片";
            sheet.releasedWhenClosed = NO; // ARC owns the sheet through pickerWindow/local references.
            picker.view.frame = NSMakeRect(0, 0, 760, 560);
            sheet.contentViewController = picker;
            [sheet setContentSize:NSMakeSize(760, 560)];
            sheet.contentMinSize = NSMakeSize(560, 400);
            sheet.delegate = self;
            self.pickerWindow = sheet;
            [self.window beginSheet:sheet completionHandler:nil];
        } else {
            [self emit:@{@"status": @"error", @"error": @"从图库选择需要 macOS 13 或更新版本；请将照片导出为 JPEG 后选择文件。"}];
        }
        return;
    }
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    if (@available(macOS 11.0, *)) panel.allowedContentTypes = @[UTTypeImage];
    panel.message = @"选择本地图片；导入“照片”图库请使用页面上的“从照片图库选择”。";
    self.filePanel = panel;
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        self.filePanel = nil;
        if (!self.active) return;
        if (response != NSModalResponseOK || !panel.URL) { [self emit:@{@"status": @"cancelled"}]; return; }
        NSUInteger generation = [self begin];
        NSURL *url = panel.URL;
        [self.queue addOperationWithBlock:^{ [self consumeURL:url generation:generation]; }];
    }];
}
- (void)closePicker {
    NSWindow *sheet = self.pickerWindow;
    self.pickerWindow = nil;
    if (sheet.sheetParent) [sheet.sheetParent endSheet:sheet];
    sheet.delegate = nil;
    [sheet close];
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
    [self cancel];
    return NO;
}
- (void)picker:(PHPickerViewController *)picker didFinishPicking:(NSArray<PHPickerResult *> *)results API_AVAILABLE(macos(13)) {
    if (!self.active) return;
    NSItemProvider *provider = results.firstObject.itemProvider;
    if (!provider) { [self cancel]; return; }
    if (self.busy) return;
    self.photoProvider = provider;
    NSUInteger generation = [self begin];
    [self loadPhotoTypes:ReferencePhotoTypes(provider) index:0 data:NO generation:generation];
}
- (void)loadPhotoTypes:(NSArray<NSString *> *)types index:(NSUInteger)index data:(BOOL)dataMode generation:(NSUInteger)generation {
    if (!self.active || !self.busy || self.generation!=generation) return;
    if (index>=types.count) {
        NSNumber *free=ReferenceFreeMiB();
        NSString *message=free&&free.unsignedLongLongValue<6144?@"系统照片服务未能提供图片，且本机可用空间偏低，可能限制原图下载。请先释放磁盘空间，再重试同一张照片；原图和已选图片未修改。":@"系统照片服务未能提供可读取的图片。已尝试兼容读取；请确认该照片能在“照片”中完整打开后重试。展开导入诊断可查看错误码与本机剩余空间。";
        [self finish:@{@"error":message} generation:generation];return;
    }
    NSString *type=types[index];BOOL objectMode=[type isEqual:@"NSImage"];NSUInteger attempt=++self.attempt;
    NSDate *started=NSDate.date;
    __weak ReferenceDropView *weakSelf=self;
    void (^complete)(NSData *,NSError *)=^(NSData *bytes,NSError *error){
        ReferenceDropView *view=weakSelf;if(!view)return;
        // File representations are copied while their callback URL remains valid.
        [view.queue addOperationWithBlock:^{
            NSDictionary *result=bytes?NormalizeReference(bytes):nil;
            NSString *reason=error?ReferenceErrorCode(error):bytes?(result[@"error"]?@"decode-or-size-rejected":@"ok"):@"no-file-data";
            dispatch_async(dispatch_get_main_queue(), ^{
                if(!view.active||!view.busy||view.generation!=generation||view.attempt!=attempt)return;
                view.attempt++;view.progress=nil;
                [view.importAttempts addObject:@{@"method":objectMode?@"object":dataMode?@"data":@"file",@"type":type,@"result":reason,@"elapsedMs":@((NSInteger)(-[started timeIntervalSinceNow]*1000))}];
                if(result&&!result[@"error"]){[view finish:result generation:generation];return;}
                if(bytes.length>InputLimit){[view finish:result generation:generation];return;}
                [view loadPhotoTypes:types index:(dataMode||objectMode)?index+1:index data:objectMode?NO:!dataMode generation:generation];
            });
        }];
    };
    if(objectMode) self.progress=[self.photoProvider loadObjectOfClass:NSImage.class completionHandler:^(id<NSItemProviderReading> object,NSError *error){
        [self.queue addOperationWithBlock:^{complete([object isKindOfClass:NSImage.class]?ReferenceImageObjectData((NSImage *)object):nil,error);}];
    }];
    else if(dataMode) self.progress=[self.photoProvider loadDataRepresentationForTypeIdentifier:type completionHandler:complete];
    else self.progress=[self.photoProvider loadFileRepresentationForTypeIdentifier:type completionHandler:^(NSURL *url,NSError *error){
        BOOL scoped=[url startAccessingSecurityScopedResource];NSError *readError=nil;
        NSData *bytes=ReadReferenceWithError(url,&readError);
        if(scoped)[url stopAccessingSecurityScopedResource];complete(bytes,error?:readError);
    }];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC),dispatch_get_main_queue(),^{
        ReferenceDropView *view=weakSelf;
        if(!view.active||!view.busy||view.generation!=generation||view.attempt!=attempt)return;
        view.attempt++;[view.progress cancel];view.progress=nil;
        [view.importAttempts addObject:@{@"method":objectMode?@"object":dataMode?@"data":@"file",@"type":type,@"result":@"attempt-timeout",@"elapsedMs":@15000}];
        [view loadPhotoTypes:types index:(dataMode||objectMode)?index+1:index data:objectMode?NO:!dataMode generation:generation];
    });
}
- (BOOL)canDrop:(id<NSDraggingInfo>)sender {
    if (!self.active || self.busy || self.pickerWindow || self.filePanel) return NO;
    NSPasteboard *pb = sender.draggingPasteboard;
    return [pb canReadObjectForClasses:@[NSFilePromiseReceiver.class, NSURL.class] options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}] ||
        [pb availableTypeFromArray:@[NSPasteboardTypePNG, NSPasteboardTypeTIFF]] != nil;
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender { return [self canDrop:sender] ? NSDragOperationCopy : NSDragOperationNone; }
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender { return [self draggingEntered:sender]; }
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender { return [self canDrop:sender]; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    if (![self canDrop:sender]) return NO;
    NSPasteboard *pb = sender.draggingPasteboard;
    NSArray *promises = [pb readObjectsForClasses:@[NSFilePromiseReceiver.class] options:@{}];
    NSArray *urls = [pb readObjectsForClasses:@[NSURL.class] options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}];
    if (promises.count > 1 || (!promises.count && urls.count > 1)) {
        [self emit:@{@"status": @"error", @"error": @"每次请选择一张参考图。"}]; return YES;
    }
    NSUInteger generation = [self begin];
    if (promises.count == 1) {
        // Private temporary directory; only the exact generated directory is ever removed.
        NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:[@"hachimidock-reference-" stringByAppendingString:NSUUID.UUID.UUIDString]];
        NSURL *directory = [NSURL fileURLWithPath:path isDirectory:YES];
        NSError *error = nil;
        if (![NSFileManager.defaultManager createDirectoryAtURL:directory withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:&error]) {
            [self finish:@{@"error": @"临时空间不足或不可写，无法接收照片。"} generation:generation]; return YES;
        }
        NSFilePromiseReceiver *receiver = promises.firstObject;
        self.temporaryDirectory = directory;
        // fileNames can be empty until receivePromisedFiles starts (including Photos).
        if (receiver.fileTypes.count > 1) {
            [NSFileManager.defaultManager removeItemAtURL:directory error:nil];
            [self finish:@{@"error": @"每次请选择一张静态参考图。Live Photo 请从图库入口选择。"} generation:generation]; return YES;
        }
        [receiver receivePromisedFilesAtDestination:directory options:@{} operationQueue:self.queue reader:^(NSURL *url, NSError *error) {
            NSString *root = [directory.URLByResolvingSymlinksInPath.path stringByAppendingString:@"/"];
            if (!error && [url.URLByResolvingSymlinksInPath.path hasPrefix:root]) [self consumeURL:url generation:generation];
            else dispatch_async(dispatch_get_main_queue(), ^{ [self finish:@{@"error": @"照片导出失败，请从图库入口选择或导出 JPEG 后重试。"} generation:generation]; });
            [NSFileManager.defaultManager removeItemAtURL:directory error:nil];
        }];
        return YES;
    }
    if (urls.count == 1) {
        NSURL *url = urls.firstObject;
        [self.queue addOperationWithBlock:^{ [self consumeURL:url generation:generation]; }];
        return YES;
    }
    NSData *data = [pb dataForType:NSPasteboardTypePNG] ?: [pb dataForType:NSPasteboardTypeTIFF];
    [self.queue addOperationWithBlock:^{
        NSDictionary *result = NormalizeReference(data);
        dispatch_async(dispatch_get_main_queue(), ^{ [self finish:result generation:generation]; });
    }];
    return YES;
}
- (void)cancel {
    self.generation++;
    self.busy = NO;
    [self.progress cancel]; self.progress = nil;
    self.photoProvider = nil;
    if (self.temporaryDirectory) { [NSFileManager.defaultManager removeItemAtURL:self.temporaryDirectory error:nil]; self.temporaryDirectory = nil; }
    [self.filePanel cancel:nil]; self.filePanel = nil;
    [self closePicker];
    [self emit:@{@"status": @"cancelled"}];
}
@end

void reference_image_mount(void *webview, const char *token, double x, double y, double w, double h, double viewportWidth, ReferenceEvent callback) {
    NSView *host = (__bridge NSView *)webview;
    NSString *key = [NSString stringWithUTF8String:token];
    ReferenceDropView *view = objc_getAssociatedObject(host, &ReferenceAssociation);
    if (view && ![view.token isEqual:key]) { view.active = NO; [view cancel]; [view removeFromSuperview]; view = nil; }
    if (!view) {
        view = [[ReferenceDropView alloc] initWithFrame:NSZeroRect];
        view.token = key; view.active = YES; view.callback = callback;
        view.queue = [[NSOperationQueue alloc] init]; view.queue.maxConcurrentOperationCount = 1;
        NSMutableArray *types = [NSFilePromiseReceiver.readableDraggedTypes mutableCopy];
        [types addObjectsFromArray:@[NSPasteboardTypeFileURL, NSPasteboardTypePNG, NSPasteboardTypeTIFF]];
        [view registerForDraggedTypes:types];
        objc_setAssociatedObject(host, &ReferenceAssociation, view, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [host addSubview:view positioned:NSWindowAbove relativeTo:nil];
    }
    double scale = viewportWidth > 0 ? host.bounds.size.width / viewportWidth : 1;
    NSRect rect = NSMakeRect(x * scale, y * scale, w * scale, h * scale);
    if (!host.isFlipped) rect.origin.y = host.bounds.size.height - NSMaxY(rect);
    view.frame = NSIntersectionRect(host.bounds, rect);
    view.hidden = w <= 0 || h <= 0;
}
void reference_image_action(void *webview, const char *token, int action) {
    NSView *host = (__bridge NSView *)webview;
    ReferenceDropView *view = objc_getAssociatedObject(host, &ReferenceAssociation);
    if (!view || ![view.token isEqual:[NSString stringWithUTF8String:token]]) return;
    if (action == 0) [view pick:NO];
    else if (action == 1) [view pick:YES];
    else if (action == 2) [view cancel];
    else if (action == 3) { view.active = NO; [view cancel]; [view removeFromSuperview]; objc_setAssociatedObject(host, &ReferenceAssociation, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
}

#ifdef REFERENCE_IMAGE_TEST
// Offline test entry: consumes a synthetic fixture, never opens UI or reads a photo library.
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 3) return 2;
        NSDictionary *result = NormalizeReference(ReadReference([NSURL fileURLWithPath:@(argv[1])]));
        if (result[@"error"]) { puts([result[@"error"] UTF8String]); return 1; }
        NSData *bytes = [[NSData alloc] initWithBase64EncodedString:result[@"base64"] options:0];
        return [bytes writeToFile:@(argv[2]) atomically:YES] ? 0 : 3;
    }
}
#endif

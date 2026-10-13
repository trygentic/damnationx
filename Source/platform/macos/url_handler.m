#import <Foundation/Foundation.h>

#include "url_handler.h"

static void (*UrlCallback)(const char *url);

@interface DvlUrlHandler : NSObject
- (void)handleUrlEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply;
@end

@implementation DvlUrlHandler
- (void)handleUrlEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply
{
	NSString *url = [[event paramDescriptorForKeyword:'----'] stringValue];
	if (url != nil && UrlCallback != NULL) UrlCallback([url UTF8String]);
}
@end

void MacInstallUrlHandler(void (*callback)(const char *url))
{
	static DvlUrlHandler *handler;
	if (handler == nil) handler = [[DvlUrlHandler alloc] init];
	UrlCallback = callback;
	// kInternetEventClass / kAEGetURL
	[[NSAppleEventManager sharedAppleEventManager] setEventHandler:handler
	                                                   andSelector:@selector(handleUrlEvent:withReplyEvent:)
	                                                 forEventClass:'GURL'
	                                                    andEventID:'GURL'];
}

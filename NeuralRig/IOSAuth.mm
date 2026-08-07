/**
    Presents the TONE3000 sign-in page on iOS.

    This file belongs to the App target alone, never the framework. The
    framework is linked by the AUv3 extension and so is built extension-safe,
    where UIKit's application singleton is unavailable -- naming it there is a
    build error. Opening a URL is therefore a capability only the containing app
    has, and the app installs it here through nr::net::SetUrlOpener.

    ASWebAuthenticationSession rather than opening Safari:

      * The app stays in the foreground. Handing the URL to Safari would
        background us, and a backgrounded app stops accepting connections,
        which is what rules out the loopback listener the desktop builds use.

      * The redirect is intercepted by the session and delivered straight to
        its completion handler, so no AppDelegate hook is needed -- which
        matters, because the AppDelegate lives in the iPlug2 submodule and is
        not ours to edit.

      * The session runs in a browser the user already trusts and that already
        holds their TONE3000 cookies, so the plugin never sees a password.
*/

#import <AuthenticationServices/AuthenticationServices.h>
#import <UIKit/UIKit.h>

#include <string>

#include "net/LoopbackServer.h"
#include "net/Platform.h"

/// ASWebAuthenticationSession needs a window to hang its sheet from.
@interface NRAuthPresenter : NSObject <ASWebAuthenticationPresentationContextProviding>
@end

@implementation NRAuthPresenter

- (ASPresentationAnchor)presentationAnchorForWebAuthenticationSession:(ASWebAuthenticationSession*)session
{
  for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
  {
    if (![scene isKindOfClass:UIWindowScene.class])
      continue;

    for (UIWindow* window in ((UIWindowScene*)scene).windows)
    {
      if (window.isKeyWindow)
        return window;
    }
  }

  return nil;
}

@end

namespace
{
// The session has to outlive the call that starts it or iOS tears it down
// before the user can finish signing in. Only ever touched on the main thread.
ASWebAuthenticationSession* gSession = nil;
NRAuthPresenter* gPresenter = nil;

/// The scheme half of kUrlSchemeRedirectUri. ASWebAuthenticationSession wants
/// the scheme on its own, without the "://" or any path.
NSString* CallbackScheme()
{
  NSString* uri = @(nr::net::kUrlSchemeRedirectUri);
  const NSRange separator = [uri rangeOfString:@"://"];

  return separator.location == NSNotFound ? uri : [uri substringToIndex:separator.location];
}

void StartSession(NSString* authorizationUrl)
{
  NSURL* url = [NSURL URLWithString:authorizationUrl];

  if (url == nil)
  {
    // Nothing will arrive, so release the waiting worker rather than let it sit
    // out its timeout.
    nr::net::UrlSchemeRedirect::Deliver({});
    return;
  }

  if (gPresenter == nil)
    gPresenter = [[NRAuthPresenter alloc] init];

  gSession = [[ASWebAuthenticationSession alloc]
        initWithURL:url
  callbackURLScheme:CallbackScheme()
  completionHandler:^(NSURL* _Nullable callbackUrl, NSError* _Nullable error) {
        // Deliver either way. An empty string tells the waiting worker the
        // attempt is over -- the user cancelled, or the session failed -- so it
        // reports a failed sign-in now instead of blocking until it times out.
        const std::string result =
          (error == nil && callbackUrl != nil) ? std::string(callbackUrl.absoluteString.UTF8String) : std::string{};

        nr::net::UrlSchemeRedirect::Deliver(result);
        gSession = nil;
      }];

  // Reuse whatever TONE3000 session the user already has in their browser,
  // rather than making them type credentials the plugin has no business seeing.
  gSession.prefersEphemeralWebBrowserSession = NO;
  gSession.presentationContextProvider = gPresenter;

  if (![gSession start])
  {
    nr::net::UrlSchemeRedirect::Deliver({});
    gSession = nil;
  }
}
} // namespace

/// Installs the opener before any sign-in can be attempted. This only stores a
/// callable, so it is safe this early -- nothing here touches UIKit until the
/// opener is actually invoked, long after the app has finished launching.
__attribute__((constructor)) static void NRInstallUrlOpener()
{
  nr::net::SetUrlOpener([](const std::string& url) -> bool {
    NSString* authorizationUrl = @(url.c_str());

    // Called from the worker thread driving the OAuth exchange; UIKit and
    // ASWebAuthenticationSession are main-thread only.
    dispatch_async(dispatch_get_main_queue(), ^{ StartSession(authorizationUrl); });

    // The session was queued. Whether the user completes it is reported
    // separately, through UrlSchemeRedirect.
    return true;
  });
}

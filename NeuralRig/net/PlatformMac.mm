#include "Platform.h"

#ifdef __APPLE__

  #import <CommonCrypto/CommonDigest.h>
  #import <Foundation/Foundation.h>

  #if TARGET_OS_OSX
    #import <AppKit/AppKit.h>
  #else
    #import <UIKit/UIKit.h>
  #endif

  #include <Security/Security.h>

namespace nr::net
{
namespace
{
// Matches the Windows side; a stalled connection must not hang a worker
// thread indefinitely.
constexpr NSTimeInterval kRequestTimeoutSeconds = 60.0;

NSString* ToNSString(const std::string& text)
{
  return [NSString stringWithUTF8String:text.c_str()];
}
} // namespace

void Sha256(const void* data, size_t numBytes, uint8_t digest[32])
{
  CC_SHA256(data, (CC_LONG)numBytes, digest);
}

bool SecureRandomBytes(void* buffer, size_t numBytes)
{
  return SecRandomCopyBytes(kSecRandomDefault, numBytes, buffer) == errSecSuccess;
}

bool OpenUrlInBrowser(const std::string& url)
{
  @autoreleasepool
  {
    NSURL* nsUrl = [NSURL URLWithString:ToNSString(url)];
    if (nsUrl == nil)
      return false;

  #if TARGET_OS_OSX
    return [[NSWorkspace sharedWorkspace] openURL:nsUrl] == YES;
  #else
    [[UIApplication sharedApplication] openURL:nsUrl options:@{} completionHandler:nil];
    return true;
  #endif
  }
}

HttpResponse HttpRequest(const std::string& method,
                         const std::string& url,
                         const std::vector<Header>& headers,
                         const std::string& body)
{
  HttpResponse response;

  @autoreleasepool
  {
    NSURL* nsUrl = [NSURL URLWithString:ToNSString(url)];
    if (nsUrl == nil)
    {
      response.transportError = "Malformed URL";
      return response;
    }

    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:nsUrl
                                                          cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                      timeoutInterval:kRequestTimeoutSeconds];
    request.HTTPMethod = ToNSString(method);

    for (const auto& header : headers)
      [request setValue:ToNSString(header.value) forHTTPHeaderField:ToNSString(header.name)];

    if (!body.empty())
      request.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];

    // NSURLSession is asynchronous; this call is documented as blocking, so
    // the semaphore turns it back into one. Callers already know to keep this
    // off the audio and UI threads.
    __block NSData* responseData = nil;
    __block NSURLResponse* urlResponse = nil;
    __block NSError* error = nil;

    dispatch_semaphore_t finished = dispatch_semaphore_create(0);

    NSURLSessionDataTask* task = [[NSURLSession sharedSession]
      dataTaskWithRequest:request
        completionHandler:^(NSData* data, NSURLResponse* taskResponse, NSError* taskError) {
          responseData = data;
          urlResponse = taskResponse;
          error = taskError;
          dispatch_semaphore_signal(finished);
        }];

    [task resume];
    dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);

    if (error != nil)
    {
      response.transportError = error.localizedDescription.UTF8String;
      return response;
    }

    if ([urlResponse isKindOfClass:[NSHTTPURLResponse class]])
      response.statusCode = (int)((NSHTTPURLResponse*)urlResponse).statusCode;

    if (responseData != nil)
    {
      const auto* bytes = static_cast<const uint8_t*>(responseData.bytes);
      response.body.assign(bytes, bytes + responseData.length);
    }
  }

  return response;
}

} // namespace nr::net

#endif // __APPLE__

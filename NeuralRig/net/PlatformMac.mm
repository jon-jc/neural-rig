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

/// Set once by the containing app before any sign-in can be attempted, and
/// read on whichever thread starts the OAuth flow. Unset in an app extension.
std::function<bool(const std::string&)>& UrlOpenerSlot()
{
  static std::function<bool(const std::string&)> slot;
  return slot;
}
} // namespace

void SetUrlOpener(std::function<bool(const std::string& url)> opener)
{
  UrlOpenerSlot() = std::move(opener);
}

void Sha256(const void* data, size_t numBytes, uint8_t digest[32])
{
  CC_SHA256(data, (CC_LONG)numBytes, digest);
}

bool SecureRandomBytes(void* buffer, size_t numBytes)
{
  return SecRandomCopyBytes(kSecRandomDefault, numBytes, buffer) == errSecSuccess;
}

namespace
{
/// Keychain generic-password items are identified by service + account. The
/// service is fixed so all of NeuralRig's secrets group together in Keychain
/// Access, and the account distinguishes them.
NSString* const kKeychainService = @"NeuralRig";

NSMutableDictionary* KeychainQuery(const std::string& key)
{
  NSMutableDictionary* query = [NSMutableDictionary dictionary];
  query[(__bridge id)kSecClass] = (__bridge id)kSecClassGenericPassword;
  query[(__bridge id)kSecAttrService] = kKeychainService;
  query[(__bridge id)kSecAttrAccount] = ToNSString(key);

  #if TARGET_OS_IPHONE
  // The app and the AUv3 extension have separate keychains unless they name a
  // shared access group. Without this the user would sign in inside the app and
  // the plugin would still consider itself signed out, since only the app can
  // present sign-in -- so the extension would have no way to become authorised
  // at all.
  //
  // The access group is deliberately not named here. A shared group has to
  // carry the team ID as a prefix, and that is only known at build time --
  // $(AppIdentifierPrefix) is substituted into the entitlement, not into
  // source, so writing it in a literal would send the string through verbatim
  // and fail to match anything. Leaving kSecAttrAccessGroup unset makes the
  // item land in the first group listed in keychain-access-groups, so the
  // entitlement puts the shared group first and both processes agree without
  // either of them hardcoding a team ID.

  // Reachable when the device is locked, so a rig can keep loading captures
  // while the iPad sits locked on a stand mid-session.
  query[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlock;
  #endif

  return query;
}
} // namespace

std::string UserDataDirectory()
{
  @autoreleasepool
  {
  #if TARGET_OS_IPHONE
    // An app extension gets its own container, so a capture downloaded in the
    // app would be invisible to the AUv3 plugin -- the user would sign in,
    // build a rig, open their DAW and find an empty cache. The App Group
    // container is the one directory both can see.
    //
    // Must match com.apple.security.application-groups in the entitlements,
    // and the group has to be registered on the Apple Developer account and
    // enabled for both the app and the extension.
    NSURL* shared = [[NSFileManager defaultManager]
      containerURLForSecurityApplicationGroupIdentifier:@"group.com.jonjc.NeuralRig"];

    if (shared != nil)
    {
      NSURL* sharedDirectory = [shared URLByAppendingPathComponent:@"NeuralRig"];
      const std::string sharedPath = sharedDirectory.path.UTF8String;

      if (EnsureDirectory(sharedPath))
        return sharedPath;
    }

    // Deliberately falls through rather than failing. If the group is not
    // provisioned the standalone app should still work on its own, with a
    // private cache, instead of refusing to store anything at all.
  #endif

    NSArray<NSURL*>* urls = [[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory
                                                                   inDomains:NSUserDomainMask];
    if (urls.count == 0)
      return {};

    NSURL* directory = [urls.firstObject URLByAppendingPathComponent:@"NeuralRig"];
    const std::string path = directory.path.UTF8String;

    return EnsureDirectory(path) ? path : std::string{};
  }
}

bool SecretStore(const std::string& key, const std::string& value)
{
  @autoreleasepool
  {
    NSData* data = [NSData dataWithBytes:value.data() length:value.size()];

    // Delete any existing item first: SecItemAdd fails with errSecDuplicateItem
    // rather than overwriting, and an update path would be more code for the
    // same result.
    SecItemDelete((__bridge CFDictionaryRef)KeychainQuery(key));

    NSMutableDictionary* query = KeychainQuery(key);
    query[(__bridge id)kSecValueData] = data;
    // Available once the device has been unlocked, and never migrated to
    // another machine by a backup.
    query[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;

    return SecItemAdd((__bridge CFDictionaryRef)query, nullptr) == errSecSuccess;
  }
}

bool SecretLoad(const std::string& key, std::string& value)
{
  @autoreleasepool
  {
    NSMutableDictionary* query = KeychainQuery(key);
    query[(__bridge id)kSecReturnData] = @YES;
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;

    CFTypeRef result = nullptr;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &result) != errSecSuccess || result == nullptr)
      return false;

    NSData* data = (__bridge_transfer NSData*)result;
    value.assign((const char*)data.bytes, data.length);
    return true;
  }
}

bool SecretErase(const std::string& key)
{
  @autoreleasepool
  {
    return SecItemDelete((__bridge CFDictionaryRef)KeychainQuery(key)) == errSecSuccess;
  }
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
    // Deliberately not UIApplication.sharedApplication. This file compiles into
    // a framework the AUv3 extension links, so it is built extension-safe, and
    // that API is unavailable to app extensions -- naming it at all is a build
    // error, not a runtime one. The containing app installs an opener instead.
    (void)nsUrl;
    if (const auto& opener = UrlOpenerSlot())
      return opener(url);

    return false;
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

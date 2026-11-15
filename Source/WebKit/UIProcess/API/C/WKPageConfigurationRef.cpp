/*
 * Copyright (C) 2015 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WKPageConfigurationRef.h"

#include "APIFrameInfo.h"
#include "APIPageConfiguration.h"
#include "APIWebsitePolicies.h"
#include "BrowsingContextGroup.h"
#include "WKAPICast.h"
#include "WebPageGroup.h"
#include "WebPageProxy.h"
#include "WebProcessPool.h"
#include "WebUserContentControllerProxy.h"
#include "WebURLSchemeHandler.h"
#include "WebURLSchemeTask.h"
#include "WKType.h"
#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SharedBuffer.h>

#if ENABLE(APPLICATION_MANIFEST)
#include "APIApplicationManifest.h"
#include <WebCore/ApplicationManifestParser.h>
#endif

using namespace WebKit;

namespace {

// A custom scheme answered by the embedder through C callbacks.
class WebURLSchemeHandlerC final : public WebKit::WebURLSchemeHandler {
public:
    static Ref<WebURLSchemeHandlerC> create(WKURLSchemeTaskStartCallback start, WKURLSchemeTaskStopCallback stop, const void* context)
    {
        return adoptRef(*new WebURLSchemeHandlerC(start, stop, context));
    }

private:
    WebURLSchemeHandlerC(WKURLSchemeTaskStartCallback start, WKURLSchemeTaskStopCallback stop, const void* context)
        : m_start(start), m_stop(stop), m_context(context) { }

    static WKURLSchemeTaskRef taskRef(WebKit::WebURLSchemeTask& task) { return reinterpret_cast<WKURLSchemeTaskRef>(static_cast<API::Object*>(&task)); }

    void platformStartTask(WebKit::WebPageProxy&, WebKit::WebURLSchemeTask& task) final
    {
        auto request = task.request();
        WKURLRef url = toCopiedURLAPI(request.url());
        WKStringRef method = toCopiedAPI(request.httpMethod());
        WKURLRef frameURL = toCopiedURLAPI(task.frameInfo().request().url());
        if (m_start)
            m_start(taskRef(task), url, method, frameURL, m_context);
        WKRelease(frameURL);
        WKRelease(method);
        WKRelease(url);
    }

    void platformStopTask(WebKit::WebPageProxy&, WebKit::WebURLSchemeTask& task) final
    {
        if (m_stop)
            m_stop(taskRef(task), m_context);
    }

    WKURLSchemeTaskStartCallback m_start;
    WKURLSchemeTaskStopCallback m_stop;
    const void* m_context;
};

} // namespace

void WKPageConfigurationSetURLSchemeHandler(WKPageConfigurationRef configuration, WKStringRef scheme, WKURLSchemeTaskStartCallback start, WKURLSchemeTaskStopCallback stop, const void* context)
{
    toImpl(configuration)->setURLSchemeHandlerForURLScheme(WebURLSchemeHandlerC::create(start, stop, context), toWTFString(scheme));
}

void WKURLSchemeTaskFinish(WKURLSchemeTaskRef taskRef, int httpStatus, WKStringRef mimeType, const void* bytes, size_t length)
{
    auto& task = static_cast<WebKit::WebURLSchemeTask&>(*reinterpret_cast<API::Object*>(const_cast<OpaqueWKURLSchemeTask*>(taskRef)));
    String mime = mimeType ? toWTFString(mimeType) : "application/octet-stream"_s;
    String type = mime;
    String charset;
    if (auto semicolon = mime.find(';'); semicolon != notFound) {
        type = mime.left(semicolon).trim(isASCIIWhitespace<char16_t>);
        if (auto at = mime.findIgnoringASCIICase("charset="_s); at != notFound)
            charset = mime.substring(at + 8).trim(isASCIIWhitespace<char16_t>);
    }
    WebCore::ResourceResponse response { URL { task.request().url() }, String { type }, static_cast<long long>(length), String { charset } };
    response.setHTTPStatusCode(httpStatus);
    response.setHTTPHeaderField(WebCore::HTTPHeaderName::ContentType, mime);
    response.setHTTPHeaderField(WebCore::HTTPHeaderName::AccessControlAllowOrigin, "*"_s);
    if (task.didReceiveResponse(WTF::move(response)) != WebKit::WebURLSchemeTask::ExceptionType::None)
        return;
    if (length)
        task.didReceiveData(WebCore::SharedBuffer::create(std::span { static_cast<const uint8_t*>(bytes), length }));
    task.didComplete({ });
}

WKTypeID WKPageConfigurationGetTypeID()
{
    return toAPI(API::PageConfiguration::APIType);
}

WKPageConfigurationRef WKPageConfigurationCreate()
{
    return toAPILeakingRef(API::PageConfiguration::create());
}

WKContextRef WKPageConfigurationGetContext(WKPageConfigurationRef configuration)
{
    return toAPI(protect(protect(toImpl(configuration))->processPool()).get());
}

void WKPageConfigurationSetContext(WKPageConfigurationRef configuration, WKContextRef context)
{
    protect(toImpl(configuration))->setProcessPool(protect(toImpl(context)));
}

WKPageGroupRef WKPageConfigurationGetPageGroup(WKPageConfigurationRef)
{
    return nullptr;
}

void WKPageConfigurationSetPageGroup(WKPageConfigurationRef, WKPageGroupRef)
{
}

WKUserContentControllerRef WKPageConfigurationGetUserContentController(WKPageConfigurationRef configuration)
{
    return toAPI(protect(protect(toImpl(configuration))->userContentController()).get());
}

void WKPageConfigurationSetUserContentController(WKPageConfigurationRef configuration, WKUserContentControllerRef userContentController)
{
    protect(toImpl(configuration))->setUserContentController(protect(toImpl(userContentController)));
}

WKPreferencesRef WKPageConfigurationGetPreferences(WKPageConfigurationRef configuration)
{
    return toAPI(protect(protect(toImpl(configuration))->preferences()).get());
}

void WKPageConfigurationSetPreferences(WKPageConfigurationRef configuration, WKPreferencesRef preferences)
{
    protect(toImpl(configuration))->setPreferences(protect(toImpl(preferences)));
}

WKPageRef WKPageConfigurationGetRelatedPage(WKPageConfigurationRef configuration)
{
    return toAPI(protect(toImpl(configuration)->relatedPage()).get());
}

void WKPageConfigurationSetRelatedPage(WKPageConfigurationRef configuration, WKPageRef relatedPage)
{
    toImpl(configuration)->setRelatedPage(protect(toImpl(relatedPage)));
}

WKWebsiteDataStoreRef WKPageConfigurationGetWebsiteDataStore(WKPageConfigurationRef configuration)
{
    return toAPI(protect(protect(toImpl(configuration))->websiteDataStore()).get());
}

void WKPageConfigurationSetWebsiteDataStore(WKPageConfigurationRef configuration, WKWebsiteDataStoreRef websiteDataStore)
{
    protect(toImpl(configuration))->setWebsiteDataStore(protect(toImpl(websiteDataStore)));
}

void WKPageConfigurationSetInitialCapitalizationEnabled(WKPageConfigurationRef configuration, bool enabled)
{
    toImpl(configuration)->setInitialCapitalizationEnabled(enabled);
}

void WKPageConfigurationSetBackgroundCPULimit(WKPageConfigurationRef configuration, double cpuLimit)
{
    toImpl(configuration)->setCPULimit(cpuLimit);
}

void WKPageConfigurationSetAllowTestOnlyIPC(WKPageConfigurationRef configuration, bool allowTestOnlyIPC)
{
    toImpl(configuration)->setAllowTestOnlyIPC(allowTestOnlyIPC);
}

void WKPageConfigurationSetShouldSendConsoleLogsToUIProcessForTesting(WKPageConfigurationRef configuration, bool should)
{
    toImpl(configuration)->setShouldSendConsoleLogsToUIProcessForTesting(should);
}

void WKPageConfigurationSetPortsForUpgradingInsecureSchemeForTesting(WKPageConfigurationRef configuration, uint16_t upgradeFromInsecurePort, uint16_t upgradeToSecurePort)
{
    toImpl(configuration)->setPortsForUpgradingInsecureSchemeForTesting(upgradeFromInsecurePort, upgradeToSecurePort);
}

void WKPageConfigurationSetApplicationManifest(WKPageConfigurationRef configuration, WKStringRef json, WKURLRef manifestURL, WKURLRef documentURL)
{
#if ENABLE(APPLICATION_MANIFEST)
    auto manifest = WebCore::ApplicationManifestParser::parse(toWTFString(json), URL(toWTFString(manifestURL)), URL(toWTFString(documentURL)));
    protect(toImpl(configuration))->setApplicationManifest(API::ApplicationManifest::create(manifest));
#else
    UNUSED_PARAM(configuration);
    UNUSED_PARAM(json);
    UNUSED_PARAM(manifestURL);
    UNUSED_PARAM(documentURL);
#endif
}

WKWebsitePoliciesRef WKPageConfigurationGetDefaultWebsitePolicies(WKPageConfigurationRef configuration)
{
    return toAPI(protect(protect(toImpl(configuration))->defaultWebsitePolicies()).get());
}

void WKPageConfigurationSetDefaultWebsitePolicies(WKPageConfigurationRef configuration, WKWebsitePoliciesRef policies)
{
    protect(toImpl(configuration))->setDefaultWebsitePolicies(protect(toImpl(policies)));
}

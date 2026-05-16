#include "nwnx.hpp"
#include "External/httplib.h"

enum RequestMethod
{
    GET    = 0,
    POST   = 1,
    DEL = 2,
    PATCH  = 3,
    PUT    = 5,
    OPTION = 6,
    HEAD   = 7
};
enum AuthenticationType
{
    NONE         = 0,
    BASIC        = 1,
    DIGEST       = 2,
    BEARER_TOKEN = 3
};
enum ContentType
{
    HTML               = 0,
    PLAINTEXT          = 1,
    JSON               = 2,
    URL_ENCODED        = 3,
    XML                = 4,
};

struct Request
{
    int id;
    RequestMethod requestMethod;
    std::string host;
    int port;
    std::string path;
    AuthenticationType authType;
    std::string authUserToken;
    std::string authPassword;
    ContentType contentType;
    std::string data;
    std::string headersString;
    httplib::Headers headers;
    std::string tag;

    Request()
    {
        id = -1;
        requestMethod = RequestMethod::GET;
        port = 443;
        authType = AuthenticationType::NONE;
        contentType = ContentType::HTML;
    }
};

static constexpr const char* ContentTypeToString(const unsigned value)
{
    constexpr const char* TYPE_STRINGS[] =
    {
        "text/html",
        "text/plain",
        "application/json",
        "application/x-www-form-urlencoded",
        "application/xml"
    };
    return (value > 4) ? "text/html" : TYPE_STRINGS[value];
}

namespace Core {
    extern bool g_CoreShuttingDown;
}

using namespace NWNXLib;
using namespace NWNXLib::API;

namespace {

// Parse a duration string like "1.5s", "200ms", "1m30s" into milliseconds.
// Used for OpenAI-style x-ratelimit-reset-* headers. Returns -1 if unparseable.
int64_t ParseDurationToMs(const std::string& s)
{
    int64_t totalMs = 0;
    size_t i = 0;
    bool any = false;
    while (i < s.size())
    {
        size_t numStart = i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.'))
            ++i;
        if (numStart == i) return -1;
        double value;
        try { value = std::stod(s.substr(numStart, i - numStart)); }
        catch (...) { return -1; }
        std::string unit;
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i])))
            unit += static_cast<char>(std::tolower(static_cast<unsigned char>(s[i++])));
        if      (unit == "ms")            totalMs += static_cast<int64_t>(value);
        else if (unit == "s" || unit.empty()) totalMs += static_cast<int64_t>(value * 1000.0);
        else if (unit == "m")             totalMs += static_cast<int64_t>(value * 60.0 * 1000.0);
        else if (unit == "h")             totalMs += static_cast<int64_t>(value * 3600.0 * 1000.0);
        else return -1;
        any = true;
    }
    return any ? totalMs : -1;
}

// Try every retry-hint header we recognize, in priority order, and return ms.
// Returns -1 if no usable signal is present.
//
// Why every provider needs its own probe:
//   - OpenAI 429s (especially insufficient_quota) often omit Retry-After
//     entirely but include `retry-after-ms` and per-bucket `x-ratelimit-reset-*`.
//   - Discord uses float-seconds in `X-RateLimit-Reset-After` (more precise
//     than the integer-seconds `Retry-After`).
//   - RFC 7231 Retry-After can be either delay-seconds OR an HTTP-date; we
//     handle the numeric case and ignore dates rather than crash.
int64_t ComputeRetryAfterMs(const httplib::Response& response)
{
    if (response.has_header("Retry-After-Ms"))
    {
        try { return std::stoll(response.get_header_value("Retry-After-Ms")); }
        catch (...) {}
    }
    if (response.has_header("X-RateLimit-Reset-After"))
    {
        try { return static_cast<int64_t>(std::stof(response.get_header_value("X-RateLimit-Reset-After")) * 1000.0f); }
        catch (...) {}
    }
    if (response.has_header("Retry-After"))
    {
        try { return static_cast<int64_t>(std::stof(response.get_header_value("Retry-After")) * 1000.0f); }
        catch (...) {} // HTTP-date form — fall through
    }
    int64_t openAiMs = -1;
    for (const char* h : {"X-RateLimit-Reset-Requests", "X-RateLimit-Reset-Tokens"})
    {
        if (response.has_header(h))
        {
            int64_t v = ParseDurationToMs(response.get_header_value(h));
            if (v >= 0 && (openAiMs < 0 || v < openAiMs))
                openAiMs = v;
        }
    }
    return openAiMs;
}

// Bounds applied to whatever ComputeRetryAfterMs returns before we emit it.
// Default: a sane wait when no header is parseable (the original bug — the
// scripts saw 0 and busy-looped). Min: never busy-loop. Max: never park
// requests for absurd durations even if a server returns garbage.
constexpr int64_t kRetryDefaultMs = 5000;
constexpr int64_t kRetryMinMs     = 1000;
constexpr int64_t kRetryMaxMs     = 60LL * 60 * 1000;

} // namespace

static std::unique_ptr<httplib::Result> GetResult(const Request&);
static httplib::Headers ParseHeaderString(const std::string&);
static int s_clientRequestId = 0;
static int s_clientTimeout = Config::Get<int>("CLIENT_REQUEST_TIMEOUT", 2000);
static std::unordered_map<std::string, std::unique_ptr<httplib::SSLClient>> s_clientHostCache;
static std::unordered_map<int, Request> s_clientRequests;


std::unique_ptr<httplib::Result> GetResult(const Request &client_req)
{
    std::unique_ptr<httplib::Result> result;
    auto cli = s_clientHostCache[client_req.host].get();
    switch (client_req.requestMethod)
    {
        case RequestMethod::GET:
            result = std::make_unique<httplib::Result>(cli->Get(client_req.path.c_str(), client_req.headers));
            break;
        case RequestMethod::POST:
            result = std::make_unique<httplib::Result>(
                    cli->Post(client_req.path.c_str(), client_req.headers, client_req.data,
                              ContentTypeToString(client_req.contentType)));
            break;
        case RequestMethod::DEL:
            result = std::make_unique<httplib::Result>(
                    cli->Delete(client_req.path.c_str(), client_req.headers, client_req.data,
                                ContentTypeToString(client_req.contentType)));
            break;
        case RequestMethod::PATCH:
            result = std::make_unique<httplib::Result>(
                    cli->Patch(client_req.path.c_str(), client_req.headers, client_req.data,
                               ContentTypeToString(client_req.contentType)));
            break;
        case RequestMethod::PUT:
            result = std::make_unique<httplib::Result>(
                    cli->Put(client_req.path.c_str(), client_req.headers, client_req.data,
                             ContentTypeToString(client_req.contentType)));
            break;
        case RequestMethod::OPTION:
            result = std::make_unique<httplib::Result>(cli->Options(client_req.path.c_str(), client_req.headers));
            break;
        case RequestMethod::HEAD:
            result = std::make_unique<httplib::Result>(cli->Head(client_req.path.c_str(), client_req.headers));
            break;
    }

    return result;
}

void PerformRequest(const Request &client_req)
{
    auto cli = s_clientHostCache.find(client_req.host);
    if (cli == std::end(s_clientHostCache))
    {
        LOG_DEBUG("Creating new SSL client for host %s.", client_req.host);
        cli = s_clientHostCache.insert(std::make_pair(client_req.host,
                                                      std::make_unique<httplib::SSLClient>(client_req.host.c_str(),
                                                                                           client_req.port))).first;
    }

    if (client_req.authType == AuthenticationType::BASIC)
        cli->second->set_basic_auth(client_req.authUserToken.c_str(), client_req.authPassword.c_str());
    else if (client_req.authType == AuthenticationType::DIGEST)
        cli->second->set_digest_auth(client_req.authUserToken.c_str(), client_req.authPassword.c_str());
    else if (client_req.authType == AuthenticationType::BEARER_TOKEN)
        cli->second->set_bearer_token_auth(client_req.authUserToken.c_str());

    if (Core::g_CoreShuttingDown)
    {
        // Shorter timeout when shutting down
        cli->second->set_connection_timeout(0, 300000);
        auto result = GetResult(client_req);
        auto res = result->value();

        if (res.status == 200)
            LOG_INFO("Sent webhook '%s' to '%s%s'.", client_req.data, client_req.host, client_req.path);
        else
            LOG_WARNING("HTTP Client Request to '%s%s' failed, status code '%d'.", client_req.data.c_str(),
                        client_req.host.c_str(), client_req.path.c_str(), res.status);
        return;
    }
    Tasks::QueueOnAsyncThread([cli, client_req]()
                              {
                                  cli->second->set_connection_timeout(0, s_clientTimeout * 1000);
                                  auto result = GetResult(client_req);

                                  if (result == nullptr || result->error() != httplib::Error::Success)
                                  {
                                      auto clientError =
                                              result != nullptr ? result->error() : httplib::Error::Unknown;
                                      Tasks::QueueOnMainThread([client_req, clientError]()
                                                               {
                                                                   MessageBus::Broadcast(
                                                                           "NWNX_EVENT_PUSH_EVENT_DATA",
                                                                           {"REQUEST_ID",
                                                                            std::to_string(client_req.id)});
                                                                   MessageBus::Broadcast(
                                                                           "NWNX_EVENT_PUSH_EVENT_DATA",
                                                                           {"RESPONSE",
                                                                            "Failed to make a client request with server. Is the url/port correct?"});
                                                                   MessageBus::Broadcast("NWNX_EVENT_SIGNAL_EVENT",
                                                                                         {"NWNX_ON_HTTPCLIENT_FAILED",
                                                                                          "0"});
                                                                   LOG_ERROR(
                                                                           "HTTP Client Request to '%s%s' failed, [Error: %d].",
                                                                           client_req.host, client_req.path,
                                                                           clientError);
                                                               });
                                      return;
                                  }
                                  auto response = result->value();
                                  Tasks::QueueOnMainThread([client_req, response]()
                                                           {
                                                               if (Core::g_CoreShuttingDown)
                                                                   return;

                                                               auto moduleOid = "0";

                                                               MessageBus::Broadcast("NWNX_EVENT_PUSH_EVENT_DATA",
                                                                                     {"STATUS", std::to_string(
                                                                                             response.status)});
                                                               MessageBus::Broadcast("NWNX_EVENT_PUSH_EVENT_DATA",
                                                                                     {"RESPONSE", String::FromUTF8(response.body)});
                                                               MessageBus::Broadcast("NWNX_EVENT_PUSH_EVENT_DATA",
                                                                                     {"REQUEST_ID", std::to_string(
                                                                                             client_req.id)});
                                                               if (response.status == 200 ||
                                                                   response.status == 201 ||
                                                                   response.status == 204 || response.status == 429)
                                                               {
                                                                   // Discord rate-limit metadata: emitted on every response (incl.
                                                                   // success) so callers can pace requests preemptively.
                                                                   if (response.has_header("X-RateLimit-Limit"))
                                                                   {
                                                                       MessageBus::Broadcast(
                                                                               "NWNX_EVENT_PUSH_EVENT_DATA",
                                                                               {"RATELIMIT_LIMIT",
                                                                                response.get_header_value(
                                                                                        "X-RateLimit-Limit")});
                                                                       MessageBus::Broadcast(
                                                                               "NWNX_EVENT_PUSH_EVENT_DATA",
                                                                               {"RATELIMIT_REMAINING",
                                                                                response.get_header_value(
                                                                                        "X-RateLimit-Remaining")});
                                                                       MessageBus::Broadcast(
                                                                               "NWNX_EVENT_PUSH_EVENT_DATA",
                                                                               {"RATELIMIT_RESET",
                                                                                response.get_header_value(
                                                                                        "X-RateLimit-Reset")});
                                                                   }

                                                                   // Compute retry delay (ms) from any provider's hint headers.
                                                                   // For non-429 responses we only emit RETRY_AFTER if the server
                                                                   // actually sent a hint (Discord does this on successes too).
                                                                   // For 429 we ALWAYS emit a value, falling back to a sane
                                                                   // default — otherwise downstream scripts read an undefined
                                                                   // event tag, get 0, and busy-loop the main thread.
                                                                   int64_t retryMs = ComputeRetryAfterMs(response);
                                                                   if (response.status == 429 && retryMs <= 0)
                                                                       retryMs = kRetryDefaultMs;
                                                                   if (retryMs > 0)
                                                                   {
                                                                       if (retryMs < kRetryMinMs) retryMs = kRetryMinMs;
                                                                       if (retryMs > kRetryMaxMs) retryMs = kRetryMaxMs;
                                                                       MessageBus::Broadcast(
                                                                               "NWNX_EVENT_PUSH_EVENT_DATA",
                                                                               {"RETRY_AFTER", std::to_string(retryMs)});
                                                                   }

                                                                   if (response.status != 429)
                                                                   {
                                                                       MessageBus::Broadcast(
                                                                               "NWNX_EVENT_SIGNAL_EVENT",
                                                                               {"NWNX_ON_HTTPCLIENT_SUCCESS",
                                                                                moduleOid});
                                                                       LOG_INFO(
                                                                               "HTTP Client Request to '%s%s' succeeded.",
                                                                               client_req.host, client_req.path);
                                                                   }
                                                                   else
                                                                   {
                                                                       MessageBus::Broadcast(
                                                                               "NWNX_EVENT_SIGNAL_EVENT",
                                                                               {"NWNX_ON_HTTPCLIENT_FAILED",
                                                                                moduleOid});
                                                                       LOG_WARNING(
                                                                               "HTTP Client Request to '%s%s' failed, rate limited (retry in %lldms).",
                                                                               client_req.host, client_req.path,
                                                                               static_cast<long long>(retryMs));
                                                                   }
                                                               }
                                                               else
                                                               {
                                                                   MessageBus::Broadcast("NWNX_EVENT_SIGNAL_EVENT",
                                                                                         {"NWNX_ON_HTTPCLIENT_FAILED",
                                                                                          moduleOid});
                                                                   LOG_WARNING(
                                                                           "HTTP Client Request to '%s%s' failed, status code '%d'.",
                                                                           client_req.host, client_req.path,
                                                                           response.status);
                                                               }
                                                           });
                              });
}

httplib::Headers ParseHeaderString(const std::string &headerStr)
{
    httplib::Headers headers;
    if (!headerStr.empty())
    {
        std::string::size_type key_pos = 0;
        std::string::size_type key_end, val_pos, val_end;
        while ((key_end = headerStr.find(':', key_pos)) != std::string::npos)
        {
            if ((val_pos = headerStr.find_first_not_of(": ", key_end)) == std::string::npos)
                break;

            val_end = headerStr.find('|', val_pos);
            headers.emplace(headerStr.substr(key_pos, key_end - key_pos),
                            headerStr.substr(val_pos, val_end - val_pos));

            key_pos = val_end;
            if (key_pos != std::string::npos)
                ++key_pos;
        }
    }
    return headers;
}

NWNX_EXPORT ArgumentStack SendRequest(ArgumentStack &&args)
{
    auto clientReq = Request();
    clientReq.id = s_clientRequestId++;
    clientReq.tag = ScriptAPI::ExtractArgument<std::string>(args);
    clientReq.requestMethod = static_cast<RequestMethod>(ScriptAPI::ExtractArgument<int>(args));
    clientReq.host = ScriptAPI::ExtractArgument<std::string>(args);
    clientReq.path = ScriptAPI::ExtractArgument<std::string>(args);
    clientReq.contentType = static_cast<ContentType>(ScriptAPI::ExtractArgument<int>(args));
    clientReq.data = String::ToUTF8(ScriptAPI::ExtractArgument<std::string>(args));
    clientReq.authType = static_cast<AuthenticationType>(ScriptAPI::ExtractArgument<int>(args));
    clientReq.authUserToken = ScriptAPI::ExtractArgument<std::string>(args);
    clientReq.authPassword = ScriptAPI::ExtractArgument<std::string>(args);
    clientReq.port = ScriptAPI::ExtractArgument<int>(args);
    if (!clientReq.port) clientReq.port = 443;
    clientReq.headersString = ScriptAPI::ExtractArgument<std::string>(args);
    clientReq.headers = ParseHeaderString(clientReq.headersString);
    s_clientRequests[clientReq.id] = clientReq;
    PerformRequest(clientReq);

    return ScriptAPI::Arguments(clientReq.id);
}

NWNX_EXPORT ArgumentStack GetRequest(ArgumentStack &&args)
{
    ScriptAPI::ArgumentStack stack;
    auto requestId = ScriptAPI::ExtractArgument<int>(args);
    auto req = s_clientRequests.find(requestId);
    ASSERT_OR_THROW(req != std::end(s_clientRequests));
    auto clientReq = req->second;

    ScriptAPI::InsertArgument(stack, clientReq.headersString);
    ScriptAPI::InsertArgument(stack, (int32_t) clientReq.port);
    ScriptAPI::InsertArgument(stack, clientReq.authPassword);
    ScriptAPI::InsertArgument(stack, clientReq.authUserToken);
    ScriptAPI::InsertArgument(stack, (int32_t) clientReq.authType);
    ScriptAPI::InsertArgument(stack, clientReq.data);
    ScriptAPI::InsertArgument(stack, (int32_t) clientReq.contentType);
    ScriptAPI::InsertArgument(stack, clientReq.path);
    ScriptAPI::InsertArgument(stack, clientReq.host);
    ScriptAPI::InsertArgument(stack, (int32_t) clientReq.requestMethod);
    ScriptAPI::InsertArgument(stack, clientReq.tag);
    return stack;
}

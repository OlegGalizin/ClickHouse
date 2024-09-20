#pragma once

#include <Common/logger_useful.h>
#include <base/types.h>


namespace DB
{
class HTTPServerRequest;
class HTTPServerResponse;
class WriteBufferFromHTTPServerResponse;

/// Sets "X-ClickHouse-Exception-Code" header and the correspondent HTTP status in the response for an exception.
/// This is a part of what sendExceptionToHTTPClient() does.
void setHTTPResponseStatusAndHeadersForException_A(
    int exception_code, HTTPServerRequest & request, HTTPServerResponse & response, WriteBufferFromHTTPServerResponse * out);

void drainRequstIfNeded(HTTPServerRequest & request, HTTPServerResponse & response) noexcept;

}

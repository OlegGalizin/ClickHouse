#include <Server/HTTP/WriteBufferFromHTTPServerResponse.h>
#include <Server/HTTP/exceptionCodeToHTTPStatus.h>
#include <IO/HTTPCommon.h>
#include <IO/Progress.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <fmt/core.h>
#include <Poco/Net/HTTPResponse.h>
#include <Common/ErrorCodes.h>
#include "Common/Exception.h"
#include "Common/StackTrace.h"
#include "Common/logger_useful.h"
#include "DataTypes/IDataType.h"
#include "IO/WriteIntText.h"
#include "Server/HTTP/sendExceptionToHTTPClient.h"
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace DB
{

namespace ErrorCodes
{
    extern const int CANNOT_WRITE_AFTER_END_OF_BUFFER;
    extern const int REQUIRED_PASSWORD;
    extern const int ABORTED;
}


void WriteBufferFromHTTPServerResponse::startSendHeaders()
{
    LOG_DEBUG(getLogger("WriteBufferFromHTTPServerResponse"), "startSendHeaders");

    if (!headers_started_sending)
    {
        headers_started_sending = true;

        if (!response.getChunkedTransferEncoding() && response.getContentLength() == Poco::Net::HTTPMessage::UNKNOWN_CONTENT_LENGTH)
        {
            /// In case there is no Content-Length we cannot use keep-alive,
            /// since there is no way to know when the server send all the
            /// data, so "Connection: close" should be sent.
            response.setKeepAlive(false);
        }

        if (add_cors_header)
            response.set("Access-Control-Allow-Origin", "*");

        setResponseDefaultHeaders(response);

        std::stringstream header; //STYLE_CHECK_ALLOW_STD_STRING_STREAM
        response.beginWrite(header);
        auto header_str = header.str();
        socketSendBytes(header_str.data(), header_str.size());
    }
}

void WriteBufferFromHTTPServerResponse::writeHeaderProgressImpl(const char * header_name)
{
    LOG_DEBUG(getLogger("WriteBufferFromHTTPServerResponse"), "writeHeaderProgressImpl");

    if (is_http_method_head || headers_finished_sending || !headers_started_sending)
        return;

    WriteBufferFromOwnString progress_string_writer;
    accumulated_progress.writeJSON(progress_string_writer);
    progress_string_writer.finalize();

    socketSendBytes(header_name, strlen(header_name));
    socketSendBytes(progress_string_writer.str().data(), progress_string_writer.str().size());
    socketSendBytes("\r\n", 2);
}

void WriteBufferFromHTTPServerResponse::writeHeaderSummary()
{
    accumulated_progress.incrementElapsedNs(progress_watch.elapsed());
    writeHeaderProgressImpl("X-ClickHouse-Summary: ");
}

void WriteBufferFromHTTPServerResponse::writeHeaderProgress()
{
    writeHeaderProgressImpl("X-ClickHouse-Progress: ");
}

void WriteBufferFromHTTPServerResponse::writeExceptionCode()
{
    LOG_DEBUG(getLogger("WriteBufferFromHTTPServerResponse"), "writeExceptionCode");

    if (headers_finished_sending || !exception_code)
        return;

    if (headers_started_sending)
    {
        static std::string_view header_key = "X-ClickHouse-Exception-Code: ";
        socketSendBytes(header_key.data(), header_key.size());
        auto str_code = std::to_string(exception_code);
        socketSendBytes(str_code.data(), str_code.size());
        socketSendBytes("\r\n", 2);
    }
}

void WriteBufferFromHTTPServerResponse::finishSendHeaders()
{
    LOG_DEBUG(getLogger("WriteBufferFromHTTPServerResponse"), "finishSendHeaders");

    if (headers_finished_sending)
        return;

    if (!headers_started_sending)
    {
        if (compression_method != CompressionMethod::None)
            response.set("Content-Encoding", toContentEncodingName(compression_method));
        startSendHeaders();
    }

    writeHeaderSummary();
    writeExceptionCode();

    headers_finished_sending = true;

    /// Send end of headers delimiter.
    socketSendBytes("\r\n", 2);
}


void WriteBufferFromHTTPServerResponse::nextImpl()
{
    LOG_DEBUG(getLogger("WriteBufferFromHTTPServerResponse"), "nextImpl");

    if (!initialized)
    {
        std::lock_guard lock(mutex);
        /// Initialize as early as possible since if the code throws,
        /// next() should not be called anymore.
        initialized = true;

        if (compression_method != CompressionMethod::None)
        {
            /// If we've already sent headers, just send the `Content-Encoding` down the socket directly
            if (headers_started_sending)
                socketSendStr("Content-Encoding: " + toContentEncodingName(compression_method) + "\r\n");
            else
                response.set("Content-Encoding", toContentEncodingName(compression_method));
        }

        startSendHeaders();
        finishSendHeaders();
    }

    if (!is_http_method_head)
        HTTPWriteBuffer::nextImpl();
}


WriteBufferFromHTTPServerResponse::WriteBufferFromHTTPServerResponse(
    HTTPServerResponse & response_,
    bool is_http_method_head_,
    const ProfileEvents::Event & write_event_)
    : HTTPWriteBuffer(response_.getSocket(), write_event_)
    , response(response_)
    , is_http_method_head(is_http_method_head_)
{
    if (response.getChunkedTransferEncoding())
        setChunked();
}


void WriteBufferFromHTTPServerResponse::onProgress(const Progress & progress)
{
    std::lock_guard lock(mutex);

    /// Cannot add new headers if body was started to send.
    if (headers_finished_sending)
        return;

    accumulated_progress.incrementPiecewiseAtomically(progress);
    if (send_progress && progress_watch.elapsed() >= send_progress_interval_ms * 1000000)
    {
        accumulated_progress.incrementElapsedNs(progress_watch.elapsed());
        progress_watch.restart();

        /// Send all common headers before our special progress headers.
        startSendHeaders();
        writeHeaderProgress();
    }
}

void WriteBufferFromHTTPServerResponse::setExceptionCode_A(int exception_code_)
{
    std::lock_guard lock(mutex);
    if (headers_started_sending)
        exception_code = exception_code_;
    else
        response.set("X-ClickHouse-Exception-Code", toString<int>(exception_code_));
}

// WriteBufferFromHTTPServerResponse::~WriteBufferFromHTTPServerResponse()
// {
//     try
//     {
//         if (!canceled)
//             finalize();
//     }
//     catch (...)
//     {
//         tryLogCurrentException(__PRETTY_FUNCTION__);
//     }
// }

void WriteBufferFromHTTPServerResponse::finalizeImpl()
{
    LOG_DEBUG(getLogger("WriteBufferFromHTTPServerResponse"), "finalizeImpl is_http_method_head {}", is_http_method_head);

    if (!headers_finished_sending)
    {
        std::lock_guard lock(mutex);
        /// If no body data just send header
        startSendHeaders();

        /// `finalizeImpl` must be idempotent, so set `initialized` here to not send stuff twice
        if (!initialized && offset() && compression_method != CompressionMethod::None)
        {
            initialized = true;
            socketSendStr("Content-Encoding: " + toContentEncodingName(compression_method) + "\r\n");
        }

        finishSendHeaders();
    }

    if (!is_http_method_head)
    {
        HTTPWriteBuffer::finalizeImpl();
    }
}

void WriteBufferFromHTTPServerResponse::cancelImpl() noexcept
{
    LOG_DEBUG(getLogger("WriteBufferFromHTTPServerResponse"), "cancel Stack: {}", StackTrace().toString());
    // we have to close connection when it has been canceled
    // client is waiting final empty chunk in the chunk encoding, chient has to receive EOF instead it ASAP
    response.setKeepAlive(false);
    HTTPWriteBuffer::cancelImpl();
}

bool WriteBufferFromHTTPServerResponse::isChunked() const
{
    chassert(response.sent());
    return HTTPWriteBuffer::isChunked();
}

bool WriteBufferFromHTTPServerResponse::isFixedLength() const
{
    chassert(response.sent());
    return HTTPWriteBuffer::isFixedLength();
}

void WriteBufferFromHTTPServerResponse::cancelWithException(HTTPServerRequest & request, int exception_code_, const std::string & message, WriteBuffer * compression_buffer) noexcept
{
    try
    {
        LOG_DEBUG(getLogger("WriteBufferFromHTTPServerResponse"), "cancelWithException isCanceled: {}", isCanceled());
        if (isCanceled())
            throw Exception(ErrorCodes::ABORTED, "Write buffer has been canceled.");

        LOG_DEBUG(getLogger("WriteBufferFromHTTPServerResponse"), "cancelWithException has compression_buffer: {} is canceled: {}", bool(compression_buffer), bool(compression_buffer && compression_buffer->isCanceled()));
        if (compression_buffer && compression_buffer->isCanceled())
            compression_buffer = nullptr;

        // proper senging
        if (!response.sent())
        {
            LOG_DEBUG(getLogger("WriteBufferFromHTTPServerResponse"), "proper senging, {}", ErrorCodes::getName(exception_code_));

            drainRequstIfNeded(request, response);
            // We try to send the exception message when the transmission has not been started yet
            // Set HTTP code and HTTP message. Add "X-ClickHouse-Exception-Code" header.
            // If it is not HEAD request send the message in the body.
            if (exception_code_ == ErrorCodes::REQUIRED_PASSWORD)
                response.requireAuthentication("ClickHouse server HTTP API");
            else
                response.setStatusAndReason(exceptionCodeToHTTPStatus(exception_code_));

            setExceptionCode_A(exception_code_);

            LOG_DEBUG(getLogger("WriteBufferFromHTTPServerResponse"), "write error {}:<{}>", message.size(), message);
            auto & out = compression_buffer ? *compression_buffer : *this;
            writeString(message, out);
            writeChar('\n', out);

            LOG_DEBUG(getLogger("WriteBufferFromHTTPServerResponse"), "do finalize");
            finalize();

            // we do not need it
            if (compression_buffer)
                compression_buffer->cancel();
        }
        else
        {
            LOG_DEBUG(getLogger("WriteBufferFromHTTPServerResponse"), "hard case");

            // We try to send the exception message even when the transmission has been started already
            // In case the chunk encoding: send new chunk which started with CHUNK_ENCODING_ERROR_HEADER and contains the error description
            // it is important to avoid sending the last empty chunk in order to break the http protocol here.
            // In case of fixed lengs we could send error but less that fixedLengthLeft bytes.
            // In case of plain stream all ways are questionable, but lets send the error any way.

            // no point to drain request, transmission has been already started hence the request has been read
            // but make sense to try to send proper `connnection: close` header if headers are not finished yet
            response.setKeepAlive(false);

            // try to send proper header in case headers are not finished yet
            setExceptionCode_A(exception_code_);

            auto data = fmt::format("{}\r\n{}\n",
                EXCEPTION_MARKER,
                message);

            if (compression_buffer)
                compression_buffer->next();
            next();

            if (isFixedLength())
            {
                if (fixedLengthLeft() > EXCEPTION_MARKER.size())
                    /// fixed length buffer drops all excess data
                    /// make sure that we send less than content-lenght bytes at the end
                    breakFixedLength();
                else
                    throw Exception(ErrorCodes::CANNOT_WRITE_AFTER_END_OF_BUFFER, "There is no space left in the fixed length HTTP-write buffer to write the exception header.");
            }

            auto & out = compression_buffer ? *compression_buffer : *this;
            writeString(EXCEPTION_MARKER, out);
            writeCString("\r\n", out);
            writeString(message, out);

            if (compression_buffer)
                compression_buffer->next();
            next();

            cancel();
        }
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__, "Failed to send exception to response write buffer");

        // we do not need it
        if (compression_buffer)
            compression_buffer->cancel();

        cancel();
    }
}

}

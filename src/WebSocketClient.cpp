#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>

#include "WebSocketClient.h"

namespace ModChatTransmitter
{
    // Resolver and socket require an io_context
    WebSocketClient::WebSocketClient(net::io_context& ioc)
        : resolver(net::make_strand(ioc)),
        ws(net::make_strand(ioc)),
        ready(false),
        close(false),
        reconnectDelay(5),
        reconnectAttempts(0),
        workQueue()
    { }

    // Start the asynchronous operation
    void WebSocketClient::Run(const std::string& host, int port, const std::string& path)
    {
        ready = false;
        this->host = host;
        this->port = port;
        this->path = path;

        Resolve();
    }

    void WebSocketClient::QueueMessage(const std::string& text)
    {
        bool idle = workQueue.empty();
        workQueue.push(text);

        if (idle && ready)
        {
            Write();
        }
    }

    bool WebSocketClient::IsReady()
    {
        return ready;
    }

    void WebSocketClient::Resolve()
    {
        resolver.async_resolve(host, std::to_string(port).c_str(), beast::bind_front_handler(&WebSocketClient::OnResolve, shared_from_this()));
    }

    void WebSocketClient::Write()
    {
        if (workQueue.empty())
        {
            return;
        }

        ws.async_write(net::buffer(workQueue.front()), beast::bind_front_handler(&WebSocketClient::OnWrite, shared_from_this()));
    }

    void WebSocketClient::Read()
    {
        ws.async_read(buffer, beast::bind_front_handler(&WebSocketClient::OnRead, shared_from_this()));
    }

    void WebSocketClient::Close()
    {
        ready = false;
        close = true;

        websocket::close_reason cr;
        cr.code = 4010;
        cr.reason = "Server closed";
        ws.async_close(cr, beast::bind_front_handler(&WebSocketClient::OnClose, shared_from_this()));
    }

    void WebSocketClient::OnResolve(beast::error_code err, tcp::resolver::results_type results)
    {
        if (err)
        {
            return OnError(err, "resolve");
        }

        // Set the timeout for the operation
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(30));

        // Make the connection on the IP address we get from a lookup
        beast::get_lowest_layer(ws).async_connect(results, beast::bind_front_handler(&WebSocketClient::OnConnect, shared_from_this()));
    }

    void WebSocketClient::OnConnect(beast::error_code err, tcp::resolver::results_type::endpoint_type ep)
    {
        if (err)
        {
            return OnError(err, "connect");
        }

        // Turn off the timeout on the tcp_stream, because
        // the websocket stream has its own timeout system.
        beast::get_lowest_layer(ws).expires_never();

        // Set suggested timeout settings for the websocket
        ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

        // This will provide the value of the
        // Host HTTP header during the WebSocket handshake.
        // See https://tools.ietf.org/html/rfc7230#section-5.4
        std::string handshakeHost = host + ':' + std::to_string(ep.port());

        // Perform the websocket handshake
        ws.async_handshake(handshakeHost, path, beast::bind_front_handler(&WebSocketClient::OnHandshake, shared_from_this()));
    }

    void WebSocketClient::OnHandshake(beast::error_code err)
    {
        if (err)
        {
            return OnError(err, "handshake");
        }

        ready = true;
        reconnectDelay = 5;
        reconnectAttempts = 0;
        LOG_INFO("server", "[ModChatTransmitter] Connected to WebSocket server.");

        Read();
        Write();
    }

    void WebSocketClient::OnWrite(beast::error_code err, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (err)
        {
            return OnError(err, "write");
        }

        workQueue.pop();
        Write();
    }

    void WebSocketClient::OnRead(beast::error_code err, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (err)
        {
            return OnError(err, "read");
        }

        std::cout << beast::make_printable(buffer.data()) << std::endl;

        buffer.clear();
        Read();
    }

    void WebSocketClient::OnClose(beast::error_code err)
    {
        if (err)
        {
            return OnError(err, "close");
        }
    }

    void WebSocketClient::OnError(beast::error_code err, const char* operation)
    {
        if (!strcmp(operation, "read"))
        {
            websocket::close_reason cr = ws.reason();
            if (cr.code == 4000)
            {
                LOG_ERROR("server", "[ModChatTransmitter] Incorrect WebSocket key!");
                return;
            }
            if (cr.code == 4001)
            {
                LOG_ERROR("server", "[ModChatTransmitter] Access denied to the WebSocket server.");
                return;
            }
        }

        ready = false;
        LOG_ERROR("server", "[ModChatTransmitter] WebSocket %s error: %s", operation, err.message().c_str());
        LOG_INFO("server", "[ModChatTransmitter] Reconnecting to WebSocket server in %d seconds.", reconnectDelay);
        if (close)
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(reconnectDelay));
        reconnectDelay *= 2;
        ++reconnectAttempts;

        if (reconnectAttempts < 10)
        {
            Resolve();
        }
    }
}
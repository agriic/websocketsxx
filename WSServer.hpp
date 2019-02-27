#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <list>
#include <map>
#include "libwebsockets.h"

namespace agriic {

class WSServer 
{
private:

    struct WSConnection {
        bool closeRequested = false;
        std::list<std::string> msgQueue;
    };

    typedef std::pair<std::string, std::function< std::string(std::string) >> HttpHandler;

    int _port = 0;
    std::string _keyPath;
    std::string _certPath;
    lws_context  *_context;
    std::vector<lws_protocols> _protocols;

    std::vector<HttpHandler> _httpHandlers;

    std::map<lws*, WSConnection> _connections;

    std::vector<uint8_t> _tmpBuf;

public:

    explicit WSServer(int port, const std::string& certPath = "", const std::string& keyPath = "")
    {
        this->_port     = port;
        this->_certPath = certPath;
        this->_keyPath  = keyPath;

        _tmpBuf.resize(2048);
    }

    ~WSServer( )
    {}

    void handleGetAny(std::function< std::string(std::string) > lambda)
    {
        _httpHandlers.push_back(HttpHandler("", lambda));
    }

    void handleGet(const std::string& url, std::function< std::string(std::string) > lambda)
    {
        _httpHandlers.push_back(HttpHandler(url, lambda));
    }

    void send(void *id, const std::string& data)
    {
        auto it = _connections.find((lws*)id);
        if(it != _connections.end()) {
            _connections[(lws*)id].msgQueue.push_back(data);
            lws_callback_on_writable((lws*)id);
        }
    }

    void close(void* id)
    {
        auto it = _connections.find((lws*)id);
        if(it != _connections.end()) {
            _connections[(lws*)id].closeRequested = true;
            lws_callback_on_writable((lws*)id);
        }
    }

protected:

    virtual void onWsConnect(void *id) = 0;
    virtual void onWsDisconnect(void *id) = 0;
    virtual void onWsMessage(void *id, const std::string& data) = 0;

private:

    int handleHttpCallback(lws *wsi)
    {
        // check callbacks and headers
        auto hdrSize = lws_hdr_total_length(wsi, WSI_TOKEN_GET_URI);
        if (_tmpBuf.size() < hdrSize + LWS_PRE) _tmpBuf.resize(hdrSize + LWS_PRE);

        int sz = lws_hdr_copy(wsi, (char*)_tmpBuf.data(), (int)_tmpBuf.size(), WSI_TOKEN_GET_URI);
        if (sz > 0) {
            std::string url((char*)_tmpBuf.data(), sz);

            for (auto& h : _httpHandlers) {
                if (h.first == "" || h.first == url) {

                    auto body = h.second(url);

                    unsigned char *start = (unsigned char *)&_tmpBuf[LWS_PRE],
                    *p = start,
                    *end = (unsigned char *)&_tmpBuf[_tmpBuf.size() - LWS_PRE - 1];

                    p = start;

                    if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK, "text/plain", body.size(), &p, end)) {
                        return 1;
                    }

                    if (lws_finalize_write_http_header(wsi, start, &p, end)) {
                        return 1;
                    }

                    if (_tmpBuf.size() < body.size() + 1 + LWS_PRE) {
                        _tmpBuf.resize(body.size() + 1 + LWS_PRE);
                        start = (unsigned char *)&_tmpBuf[LWS_PRE];
                    }

                    memcpy(start, body.data(), body.size());
                    if (lws_write(wsi, start, body.size(), LWS_WRITE_HTTP_FINAL) != body.size()) {
                        return 1;
                    }

                    if (lws_http_transaction_completed(wsi)) {
                        return 1;
                    }

                    return 0;
                }
            }

        } else {
            return 1;
        }

        return 1;
    }

    static int callback(lws *wsi,
                        enum lws_callback_reasons reason,
                        void *user,
                        void *in,
                        size_t len)
    {
        auto ctx = lws_get_context(wsi);
        WSServer* server = (WSServer*) lws_context_user(ctx);

        switch (reason) {
            case LWS_CALLBACK_HTTP: {
                return server->handleHttpCallback(wsi);
            }
            break;

            case LWS_CALLBACK_ESTABLISHED: {
                // we can check request uri
                // lws_hdr_copy(wsi, (char*)_tmpBuf.data(), (int)_tmpBuf.size(), WSI_TOKEN_GET_URI);

                server->_connections[wsi] = WSConnection();
                server->onWsConnect(wsi);
            }
            break;

            case LWS_CALLBACK_SERVER_WRITEABLE: {
                if (server->_connections[wsi].closeRequested) {
                    return -1;
                } else if (!server->_connections[wsi].msgQueue.empty()) {
                    auto message = server->_connections[wsi].msgQueue.front();
                    auto msgLen = message.size();

                    if (server->_tmpBuf.size() < LWS_PRE + 1 + msgLen) {
                        server->_tmpBuf.resize(LWS_PRE + 1 + msgLen);
                    }

                    unsigned char *start = (unsigned char *)&server->_tmpBuf[LWS_PRE];

                    memcpy(start, message.c_str(), msgLen);

                    int charsSent = lws_write(wsi, start, msgLen, LWS_WRITE_TEXT);
                    if (charsSent != msgLen) return -1;
                    else server->_connections[wsi].msgQueue.pop_front();
                }
            }
            break;

            case LWS_CALLBACK_RECEIVE: {
                server->onWsMessage(wsi, std::string((const char *)in, len));
            }
            break;

            case LWS_CALLBACK_CLOSED: {
                server->_connections.erase(wsi);
                server->onWsDisconnect(wsi);
            }
            break;

            default:
                break;
        }

        return 0;
    }

    void start()
    {
        struct lws_protocols p;
        memset(&p, 0, sizeof(lws_protocols));

        p.user = this;
        p.name = "";
        p.per_session_data_size = 0;
        p.callback = WSServer::callback;

        _protocols.clear();
        _protocols.push_back(p);
        _protocols.push_back({ NULL, NULL, 0, 0 });

        lws_set_log_level( 0, lwsl_emit_syslog ); // We'll do our own logging, thank you.
        struct lws_context_creation_info info;
        memset( &info, 0, sizeof info );

        info.port = this->_port;
        info.iface = NULL;
        info.protocols = _protocols.data();

        if(!_certPath.empty() && !_keyPath.empty()) {
            info.ssl_cert_filepath = _certPath.c_str();
            info.ssl_private_key_filepath = _keyPath.c_str();
        } else {
            info.ssl_cert_filepath = NULL;
            info.ssl_private_key_filepath = NULL;
        }
        info.gid = -1;
        info.uid = -1;
        info.user = this;
        info.options = 0;

        // keep alive
        info.ka_time = 60; // 60 seconds until connection is suspicious
        info.ka_probes = 10; // 10 probes after ^ time
        info.ka_interval = 10; // 10s interval for sending probes
        _context = lws_create_context( &info );
        if (!_context)
            throw std::runtime_error("Websocket init failed. Port taken?");
    }

public:

    void run(int timeout = 10)
    {
        while (true) {
            this->wait(timeout);
        }
    }

    void wait(int timeout = 10)
    {
        if (!_context) {
            start();
        }

        for (auto& c : _connections) {
            if (c.second.closeRequested || !c.second.msgQueue.empty()) {
                lws_callback_on_writable(c.first);
            }
        }

        if (lws_service(_context, timeout) < 0) {
            lws_context_destroy(_context);
            _context = nullptr;
            throw std::runtime_error("Error polling for socket activity");
        }
    }
};
}

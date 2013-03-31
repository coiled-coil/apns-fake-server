/*
   Copyright 2013 Coiled Coil

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include <signal.h>
#include <stdexcept>
#include <string>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/array.hpp>
#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include "coroutine.hpp"

using namespace std;

int g_invalid_token = -1;

enum { HEADER_SIZE = 1 + 4 + 4 + 2 + 32 + 2, MAX_PAYLOAD_SIZE = 255 };

typedef boost::asio::ssl::stream<boost::asio::ip::tcp::socket> ssl_socket;

struct header_rec
{
    unsigned char command;
    unsigned int identifier;
    unsigned int expiry;
    unsigned int token_length;
    unsigned char *device_token;
    unsigned int payload_length;
};

class apns_fake_server;
class apns_session;

class response_handler : coroutine
{
public:
    explicit response_handler(apns_fake_server *);
    void operator()(const boost::system::error_code& ec = boost::system::error_code(), std::size_t bytes_transferred = -1);
private:
    apns_fake_server *server_;
    boost::shared_ptr<apns_session> session_;
};

class apns_fake_server
{
public:
    apns_fake_server(boost::asio::io_service& io_service, unsigned short port)
    :   io_service_(io_service)
    ,   port_(port)
    ,   acceptor_(io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port))
    ,   context_(boost::asio::ssl::context::sslv23)
    ,   socket_(io_service, context_)
    ,   handler_(this)
    {
        context_.set_options(boost::asio::ssl::context::default_workarounds);
        context_.use_certificate_chain_file("server.pem");
        context_.use_private_key_file("server-no-password.pem", boost::asio::ssl::context::pem);
    }

    void start_accept()
    {
        handler_(boost::system::error_code());
    }

private:
    boost::asio::io_service& io_service_;
    unsigned short port_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ssl::context context_;
    ssl_socket socket_;
    response_handler handler_;

    friend class response_handler;
};

class apns_session
{
public:
    apns_session(boost::asio::io_service& io_service, boost::asio::ssl::context& context)
    :   io_service_(io_service)
    ,   context_(context)
    ,   socket_(io_service, context)
    ,   buf_()
    ,   hdr_()
    {}

private:
    boost::asio::io_service& io_service_;
    boost::asio::ssl::context& context_;
    ssl_socket socket_;
    unsigned char buf_[MAX_PAYLOAD_SIZE + 1];
    header_rec hdr_;

    friend class response_handler;
};

inline
response_handler::response_handler(apns_fake_server *server)
:   server_(server)
,   session_()
{
}

void parse_header(unsigned char *buf, header_rec *r)
{
    r->command = *buf++;
    if (r->command != 1)
        throw runtime_error("command must be 1");

    r->identifier = (*buf++) << 24 | (*buf++) << 16 | (*buf++) << 8 | (*buf++);
    r->expiry = (*buf++) << 24 | (*buf++) << 16 | (*buf++) << 8 | (*buf++);
    r->token_length = (*buf++) << 8 | (*buf++);
    if (r->token_length != 32)
        throw runtime_error("token_length must be 32");

    r->device_token = buf;
    buf += r->token_length;
    r->payload_length = (*buf++) << 8 | (*buf++);
    if (r->payload_length >= 256)
        throw runtime_error("payload length too big");

}

void print_header(header_rec const& r)
{
    cout << "command: " << static_cast<unsigned int>(r.command) << "\n"
         << "identifier: " << r.identifier << "\n"
         << "payload_length: " << r.payload_length << endl;
}

//
// See
// http://developer.apple.com/library/mac/#documentation/NetworkingInternet/Conceptual/RemoteNotificationsPG/CommunicatingWIthAPS/CommunicatingWIthAPS.html#//apple_ref/doc/uid/TP40008194-CH101-SW4
//
#include "yield.hpp"
inline
void response_handler::operator()(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec)
        return;


    reenter(this) {
        do {
            session_.reset(new apns_session(server_->io_service_, server_->context_));
            yield server_->acceptor_.async_accept(session_->socket_.lowest_layer(), *this);
            cout << "new connection" << endl;
            fork response_handler(*this)();
        } while(is_parent());

        yield session_->socket_.async_handshake(boost::asio::ssl::stream_base::server, *this);
        cout << "handshake" << endl;
        while (1) {
            cout << "-------------------------------" << endl;
            yield boost::asio::async_read(session_->socket_, boost::asio::buffer(session_->buf_, HEADER_SIZE), boost::asio::transfer_at_least(HEADER_SIZE), *this);

            parse_header(session_->buf_, &session_->hdr_);
            print_header(session_->hdr_);

            yield boost::asio::async_read(session_->socket_, boost::asio::buffer(session_->buf_, session_->hdr_.payload_length), boost::asio::transfer_at_least(session_->hdr_.payload_length), *this);
            session_->buf_[session_->hdr_.payload_length] = 0;
            cout << reinterpret_cast<char *>(session_->buf_) << endl;

            if (session_->hdr_.identifier == g_invalid_token) {
                {
                    boost::system::error_code err;
                    session_->socket_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_receive, err);
                }

                yield {
                    unsigned char response_packet[6] = { 8, 8, (g_invalid_token >> 24) & 0xff, (g_invalid_token >> 16) & 0xff, (g_invalid_token >> 8) & 0xff, g_invalid_token & 0xff };
                    boost::asio::async_write(session_->socket_, boost::asio::buffer(response_packet), *this);
                }
                // yield session_->socket_.async_shutdown(*this);
                break;
            }
        }
    }
}
#include "unyield.hpp"

int main(int argc, char *argv[])
{
    if (argc == 2) {
        g_invalid_token = atoi(argv[1]);
    }
    else if (argc > 2) {
        cerr << "Invalid option" << endl;
        return 1;
    }


    boost::asio::io_service io_service;

    // Wait for signals indicating time to shut down.
    boost::asio::signal_set signals(io_service);
    signals.add(SIGINT);
    signals.add(SIGTERM);
#if defined(SIGQUIT)
    signals.add(SIGQUIT);
#endif // defined(SIGQUIT)
    signals.async_wait(boost::bind(
          &boost::asio::io_service::stop, &io_service));

    apns_fake_server s(io_service, 12195);
    s.start_accept();

    io_service.run();

    return 0;
}

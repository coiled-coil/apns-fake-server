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

#include <string>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/array.hpp>
#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>
#include "coroutine.hpp"

class apns_fake_server;

class response_handler : coroutine
{
public:
    explicit response_handler(apns_fake_server *);
    void operator()(const boost::system::error_code& ec, std::size_t bytes_transferred = -1);
private:
    apns_fake_server *server_;
};

class apns_fake_server
{
public:
    typedef boost::asio::ssl::stream<boost::asio::ip::tcp::socket> ssl_socket;

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

inline
response_handler::response_handler(apns_fake_server *server)
:   server_(server)
{
}

#include "yield.hpp"
inline
void response_handler::operator()(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec)
        return;

    char data[100];
    int size=100;

    reenter(this) {
        yield server_->acceptor_.async_accept(server_->socket_.lowest_layer(), *this);
        yield server_->socket_.async_handshake(boost::asio::ssl::stream_base::server, *this);
        while (1) {
            yield boost::asio::async_read(server_->socket_, boost::asio::buffer(data, size), boost::asio::transfer_at_least(32), *this);
            yield boost::asio::async_read(server_->socket_, boost::asio::buffer(data, size), boost::asio::transfer_at_least(32), *this);
        }
    }
}
#include "unyield.hpp"

int main()
{
    boost::asio::io_service io_service;
    apns_fake_server s(io_service, 12195);
    s.start_accept();
    io_service.run();

    return 0;
}

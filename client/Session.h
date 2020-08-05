//
// Created by giacomo on 04/08/20.
//

#ifndef CLIENT_SESSION_H
#define CLIENT_SESSION_H


#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <cstdlib>
#include <iostream>
#include <string>


#include "client.h"
#include "configuration.h"

namespace beast = boost::beast;     // from <boost/beast.hpp>
namespace http = beast::http;       // from <boost/beast/http.hpp>
namespace net = boost::asio;        // from <boost/asio.hpp>
using tcp = net::ip::tcp;           // from <boost/asio/ip/tcp.hpp>


// Report a failure
void
fail(beast::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}


// Performs an HTTP GET and saves the response
class Session : public std::enable_shared_from_this<Session>
{
    tcp::resolver resolver_;
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_; // (Must persist between reads)
    http::request<http::string_body> req_;
    http::response<http::string_body> res_;

public:
    // Objects are constructed with a strand to
    // ensure that handlers do not execute concurrently.
    explicit
    Session(net::io_context& ioc)
            : resolver_(net::make_strand(ioc))
            , stream_(net::make_strand(ioc))
    {
    }

    // Start the asynchronous operation
    void
    run(
            http::request<http::string_body> *req
            )
    {
        // Set up an HTTP request message
        req_ = std::move(*req);
        req_.set(http::field::host, configuration::address);
        req_.version(11);
        req_.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        // authorization to be implemented
        req_.set(http::field::authorization, "aaa");

        // Look up the domain name
        resolver_.async_resolve(
                configuration::address,
                configuration::port,
                beast::bind_front_handler(
                        &Session::on_resolve,
                        shared_from_this()));
    }

    void
    on_resolve(
            beast::error_code ec,
            tcp::resolver::results_type results)
    {
        if(ec)
            return fail(ec, "resolve");

        // Set a timeout on the operation
        stream_.expires_after(std::chrono::seconds(30));

        // Make the connection on the IP address we get from a lookup
        stream_.async_connect(
                results,
                beast::bind_front_handler(
                        &Session::on_connect,
                        shared_from_this()));
    }

    void
    on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type)
    {
        if(ec)
            return fail(ec, "connect");

        // Set a timeout on the operation
        stream_.expires_after(std::chrono::seconds(30));

        // Send the HTTP request to the remote host
        http::async_write(stream_, req_,
                          beast::bind_front_handler(
                                  &Session::on_write,
                                  shared_from_this()));
    }

    void
    on_write(
            beast::error_code ec,
            std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if(ec)
            return fail(ec, "write");

        // Receive the HTTP response
        http::async_read(stream_, buffer_, res_,
                         beast::bind_front_handler(
                                 &Session::on_read,
                                 shared_from_this()));
    }

    void
    on_read(
            beast::error_code ec,
            std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if(ec)
            return fail(ec, "read");

        // Save the response
        handle_response(&res_);

        // Gracefully close the socket
        stream_.socket().shutdown(tcp::socket::shutdown_both, ec);

        // not_connected happens sometimes so don't bother reporting it.
        if(ec && ec != beast::errc::not_connected)
            return fail(ec, "shutdown");

        // If we get here then the connection is closed gracefully
    }
};


#endif //CLIENT_SESSION_H
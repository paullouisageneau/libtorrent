/*

Copyright (c) 2019, Paul-Louis Ageneau
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef TORRENT_WEBSOCKET_STREAM_HPP_INCLUDED
#define TORRENT_WEBSOCKET_STREAM_HPP_INCLUDED

#ifdef TORRENT_USE_OPENSSL
// there is no forward declaration header for asio
namespace boost {
namespace asio {
namespace ssl {
	class context;
}
}
}
#endif

#include <functional>
#include <vector>
#include <string>

#include "libtorrent/socket.hpp"
#include "libtorrent/deadline_timer.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/close_reason.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/io.hpp"
#include "libtorrent/io_context.hpp"
#include "libtorrent/optional.hpp"
#include "libtorrent/resolver_interface.hpp"
#include "libtorrent/ssl_stream.hpp"
#include "libtorrent/time.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

namespace libtorrent {
namespace aux {

namespace websocket = boost::beast::websocket;

struct TORRENT_EXTRA_EXPORT websocket_stream
	: std::enable_shared_from_this<websocket_stream>
{
	using lowest_layer_type = websocket_stream;
	using endpoint_type = std::string; // endpoint is a URL
	using protocol_type = tcp::socket::protocol_type;

	using executor_type = tcp::socket::executor_type;
	executor_type get_executor() { return m_io_service.get_executor(); }

	websocket_stream(io_context& ios
		, resolver_interface& resolver
#ifdef TORRENT_USE_OPENSSL
		, ssl::context* ssl_ctx = nullptr
#endif
		);

	~websocket_stream();
	websocket_stream& operator=(websocket_stream const&) = delete;
	websocket_stream(websocket_stream const&) = delete;
	websocket_stream& operator=(websocket_stream&&) noexcept = delete;
	websocket_stream(websocket_stream&&) noexcept = delete;

	lowest_layer_type& lowest_layer() { return *this; }

#ifndef BOOST_NO_EXCEPTIONS
	template <class IO_Control_Command>
	void io_control(IO_Control_Command&) {}
#endif

	template <class IO_Control_Command>
	void io_control(IO_Control_Command&, error_code&) {}

#ifndef BOOST_NO_EXCEPTIONS
	void non_blocking(bool) {}
#endif

	void non_blocking(bool, error_code&) {}

#ifndef BOOST_NO_EXCEPTIONS
	void bind(endpoint_type const& /*endpoint*/) {}
#endif

	void bind(endpoint_type const&, error_code&);

#ifndef BOOST_NO_EXCEPTIONS
	template <class SettableSocketOption>
	void set_option(SettableSocketOption const&) {}
#endif

	template <class SettableSocketOption>
	void set_option(SettableSocketOption const&, error_code&) { }

#ifndef BOOST_NO_EXCEPTIONS
	template <class GettableSocketOption>
	void get_option(GettableSocketOption&) {}
#endif

	template <class GettableSocketOption>
	void get_option(GettableSocketOption&, error_code&) {}

	void cancel(error_code&)
	{
		cancel_handlers(boost::asio::error::operation_aborted);
	}

	void close();
	void close(error_code const&) { close(); }

	close_reason_t get_close_reason();

	bool is_open() const { return m_open; }
	bool is_connecting() const { return m_connecting; }

	endpoint_type local_endpoint() const
	{
		error_code ec;
		return local_endpoint(ec);
	}

	endpoint_type local_endpoint(error_code& ec) const;

	endpoint_type remote_endpoint() const
	{
		error_code ec;
		return remote_endpoint(ec);
	}

	endpoint_type remote_endpoint(error_code& ec) const;

/*
	std::size_t available() const;
	std::size_t available(error_code& ec) const { return available(); }
*/

	template <class Handler>
	void async_connect(const std::string &url, Handler const& handler)
	{
		m_connect_handler = handler;
		do_connect(url);
	}

	template <class Mutable_Buffers, class Handler>
	void async_read_some(Mutable_Buffers const& buffers, Handler const& handler)
	{
		if (!m_open)
		{
			post(m_io_service, std::bind<void>(handler, boost::asio::error::not_connected, std::size_t(0)));
			return;
		}

		TORRENT_ASSERT(!m_read_handler);
		if (m_read_handler)
		{
			post(m_io_service, std::bind<void>(handler, boost::asio::error::operation_not_supported, std::size_t(0)));
			return;
		}
		m_read_handler = handler;

		using namespace std::placeholders;
		m_stream.async_read(buffers, std::bind(&websocket_stream::on_read, shared_from_this(), _1, _2));
	}

	template <class Const_Buffers, class Handler>
	void async_write_some(Const_Buffers const& buffers, Handler const& handler)
	{

		if (!m_open)
		{
			post(m_io_service, std::bind<void>(handler
				, boost::asio::error::not_connected, std::size_t(0)));
			return;
		}

		TORRENT_ASSERT(!m_write_handler);
		if (m_write_handler)
		{
			post(m_io_service, std::bind<void>(handler
				, boost::asio::error::operation_not_supported, std::size_t(0)));
			return;
		}
		m_write_handler = handler;

		using namespace std::placeholders;
		m_stream.async_write(buffers, std::bind(&websocket_stream::on_write, shared_from_this(), _1, _2));
	}

	template <class Mutable_Buffers>
    std::size_t read_some(Mutable_Buffers const& /* buffers */, error_code& /* ec */)
    {
		TORRENT_ASSERT(false && "not implemented!");
        return 0;
    }

    template <class Const_Buffers>
    std::size_t write_some(Const_Buffers const& /* buffers */, error_code& /* ec */)
    {
        TORRENT_ASSERT(false && "not implemented!");
        return 0;
    }

private:
	void do_connect(std::string url);

	void do_resolve(std::string hostname, std::uint16_t port);
	void on_resolve(error_code const& e, std::vector<address> const& addresses);
	void do_tcp_connect(std::vector<tcp::endpoint> const& endpoints);
	void on_tcp_connect(error_code const& e);
	void do_tls_handshake();
	void on_tls_handshake(error_code const& e);
	void do_handshake();
	void on_handshake(error_code const& e);

	void on_read(error_code const& e, std::size_t bytes_written);
	void on_write(error_code const& e, std::size_t bytes_written);

	void cancel_handlers(error_code const& e);

	std::function<void(error_code const&)> m_connect_handler;
	std::function<void(error_code const&, std::size_t)> m_read_handler;
	std::function<void(error_code const&, std::size_t)> m_write_handler;

	io_context& m_io_service;

	close_reason_t m_close_reason = close_reason_t::none;

	std::string m_url;
	std::string m_hostname;
	std::uint16_t m_port;
	std::string m_target;

	websocket::stream<ssl::stream<tcp::socket>> m_stream;
/*
#ifdef TORRENT_USE_OPENSSL
	ssl::context* m_ssl_ctx;
	bool m_own_ssl_context;
#endif
*/
	resolver_interface& m_resolver;
	resolver_flags m_resolve_flags;

	// specifies whether or not the connection is
	// configured to use a proxy
	//aux::proxy_settings m_proxy;

	bool m_open;
	bool m_connecting;
};

}
}

#endif

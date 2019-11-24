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

#include "libtorrent/assert.hpp"
#include "libtorrent/close_reason.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/io.hpp"
#include "libtorrent/io_context.hpp"
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
	using connect_handler = std::function<void(error_code const&)>;
	using read_handler = std::function<void(error_code const&, std::size_t)>;
	using write_handler = std::function<void(error_code const&, std::size_t)>;

	websocket_stream(io_context& ios
		, resolver_interface& resolver
		, ssl::context* ssl_ctx
	);

	~websocket_stream();
	websocket_stream& operator=(websocket_stream const&) = delete;
	websocket_stream(websocket_stream const&) = delete;
	websocket_stream& operator=(websocket_stream&&) noexcept = delete;
	websocket_stream(websocket_stream&&) noexcept = delete;

	void close();
	void close(error_code const&) { close(); }
	close_reason_t get_close_reason();

	bool is_open() const { return m_open; }
	bool is_connecting() const { return m_connecting; }

	void set_user_agent(std::string user_agent);

	template <class Handler>
	void async_connect(const std::string &url, Handler const& handler)
	{
		m_connect_handler = handler;
		do_connect(url);
	}

	template <class Mutable_Buffer, class Handler>
	void async_read_some(Mutable_Buffer& buffer, Handler const& handler)
	{
		if (!m_open)
		{
			post(m_io_service, std::bind<void>(handler, boost::asio::error::not_connected, std::size_t(0)));
			return;
		}

		using namespace std::placeholders;
		m_stream.async_read(buffer, std::bind(&websocket_stream::on_read,
					shared_from_this(),
					_1,
					_2,
					read_handler(handler)));
	}

	template <class Const_Buffer, class Handler>
	void async_write_some(Const_Buffer const& buffer, Handler const& handler)
	{
		if (!m_open)
		{
			post(m_io_service, std::bind<void>(handler
				, boost::asio::error::not_connected, std::size_t(0)));
			return;
		}

		using namespace std::placeholders;
		m_stream.async_write(buffer, std::bind(&websocket_stream::on_write,
					shared_from_this(),
					_1,
					_2,
					write_handler(handler)));
	}

private:
	void do_connect(std::string url);
	void do_resolve(std::string hostname, std::uint16_t port);
	void on_resolve(error_code const& ec, std::vector<address> const& addresses);
	void do_tcp_connect(std::vector<tcp::endpoint> endpoints);
	void on_tcp_connect(error_code const& ec);
	void do_tls_handshake();
	void on_tls_handshake(error_code const& ec);
	void do_handshake();
	void on_handshake(error_code const& ec);
	void on_read(error_code const& ec, std::size_t bytes_written, read_handler handler);
	void on_write(error_code const& ec, std::size_t bytes_written, write_handler handler);

	io_context& m_io_service;
	resolver_interface& m_resolver;
	websocket::stream<ssl::stream<tcp::socket>> m_stream;

	std::string m_url;
	std::string m_hostname;
	std::uint16_t m_port;
	std::string m_target;
	std::string m_user_agent;
	std::vector<tcp::endpoint> m_endpoints;

	connect_handler m_connect_handler;

	bool m_open;
	bool m_connecting;
};

}
}

#endif

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

#ifndef TORRENT_RTC_STREAM_HPP_INCLUDED
#define TORRENT_RTC_STREAM_HPP_INCLUDED

#include "libtorrent/aux_/packet_buffer.hpp"
#include "libtorrent/close_reason.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/io.hpp"
#include "libtorrent/io_context.hpp"
#include "libtorrent/proxy_base.hpp"
#include "libtorrent/time.hpp"
#include "libtorrent/udp_socket.hpp"

#include <functional>
#include <memory>
#include <vector>

#ifndef BOOST_NO_EXCEPTIONS
#include "libtorrent/aux_/disable_warnings_push.hpp"
#include <boost/system/system_error.hpp>
#include "libtorrent/aux_/disable_warnings_pop.hpp"
#endif

namespace rtc {
	class PeerConnection;
	class DataChannel;
}

namespace libtorrent {
namespace aux {

struct TORRENT_EXTRA_EXPORT rtc_stream_init
{
	std::shared_ptr<rtc::PeerConnection> peer_connection;
	std::shared_ptr<rtc::DataChannel> data_channel;
};

// This is the user-level stream interface to WebRTC DataChannels.
struct TORRENT_EXTRA_EXPORT rtc_stream
{
	using lowest_layer_type = rtc_stream;
	using endpoint_type = tcp::socket::endpoint_type;
	using protocol_type = tcp::socket::protocol_type;

	using executor_type = tcp::socket::executor_type;
	executor_type get_executor() { return m_io_context.get_executor(); }

	explicit rtc_stream(io_context& ioc, rtc_stream_init const& init);
	~rtc_stream();
	rtc_stream& operator=(rtc_stream const&) = delete;
	rtc_stream(rtc_stream const&) = delete;
	rtc_stream& operator=(rtc_stream&&) noexcept = delete;
	rtc_stream(rtc_stream&&) noexcept = delete;

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
	void bind(endpoint_type const&) {}
#endif

	void bind(endpoint_type const&, error_code&) {}

#ifndef BOOST_NO_EXCEPTIONS
	template <class SettableSocketOption>
	void set_option(SettableSocketOption const&) {}
#endif

	template <class SettableSocketOption>
	void set_option(SettableSocketOption const&, error_code&) {}

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

	bool is_open() const;

	int read_buffer_size() const;
	void add_read_buffer(void* buf, std::size_t len);
	void issue_read();
	void add_write_buffer(void const* buf, std::size_t len);
	void issue_write();
	std::size_t read_some(bool clear_buffers);

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

	std::size_t available() const;
	std::size_t available(error_code& /*ec*/) const { return available(); }

	template <class Handler>
	void async_connect(endpoint_type const& endpoint, Handler const& handler)
	{
		handler(boost::asio::error::operation_not_supported);
	}

	template <class Mutable_Buffers, class Handler>
	void async_read_some(Mutable_Buffers const& buffers, Handler const& handler)
	{
		if (!is_open())
		{
			post(m_io_context, std::bind<void>(handler, boost::asio::error::not_connected, std::size_t(0)));
			return;
		}

		TORRENT_ASSERT(!m_read_handler);
		if (m_read_handler)
		{
			post(m_io_context, std::bind<void>(handler, boost::asio::error::operation_not_supported, std::size_t(0)));
			return;
		}
		std::size_t bytes_added = 0;
		for (auto i = buffer_sequence_begin(buffers)
			, end(buffer_sequence_end(buffers)); i != end; ++i)
		{
			if (i->size() == 0) continue;
			add_read_buffer(i->data(), i->size());
			bytes_added += i->size();
		}
		if (bytes_added == 0)
		{
			// if we're reading 0 bytes, post handler immediately
			// asio's SSL layer depends on this behavior
			post(m_io_context, std::bind<void>(handler, error_code(), std::size_t(0)));
			return;
		}

		m_read_handler = handler;
		issue_read();
	}

	template <class Protocol>
	void open(Protocol const&, error_code&)
	{ /* dummy */ }

	template <class Protocol>
	void open(Protocol const&)
	{ /* dummy */ }

	template <class Mutable_Buffers>
	std::size_t read_some(Mutable_Buffers const& buffers, error_code& ec)
	{
		TORRENT_ASSERT(!m_read_handler);
		if (!is_open())
		{
			ec = boost::asio::error::not_connected;
			return 0;
		}

		if (read_buffer_size() == 0)
		{
			ec = boost::asio::error::would_block;
			return 0;
		}
#if TORRENT_USE_ASSERTS
		size_t buf_size = 0;
#endif

		for (auto i = buffer_sequence_begin(buffers)
			, end(buffer_sequence_end(buffers)); i != end; ++i)
		{
			add_read_buffer(i->data(), i->size());
#if TORRENT_USE_ASSERTS
			buf_size += i->size();
#endif
		}
		std::size_t ret = read_some(true);
		TORRENT_ASSERT(ret <= buf_size);
		TORRENT_ASSERT(ret > 0);
		return ret;
	}

	template <class Const_Buffers>
	std::size_t write_some(Const_Buffers const& /* buffers */, error_code& /* ec */)
	{
		TORRENT_ASSERT(false && "not implemented!");
		// TODO: implement blocking write. Low priority since it's not used (yet)
		return 0;
	}

#ifndef BOOST_NO_EXCEPTIONS
	template <class Mutable_Buffers>
	std::size_t read_some(Mutable_Buffers const& buffers)
	{
		error_code ec;
		std::size_t ret = read_some(buffers, ec);
		if (ec)
			boost::throw_exception(boost::system::system_error(ec));
		return ret;
	}

	template <class Const_Buffers>
	std::size_t write_some(Const_Buffers const& buffers)
	{
		error_code ec;
		std::size_t ret = write_some(buffers, ec);
		if (ec)
			boost::throw_exception(boost::system::system_error(ec));
		return ret;
	}
#endif

	template <class Const_Buffers, class Handler>
	void async_write_some(Const_Buffers const& buffers, Handler const& handler)
	{
		if (!m_data_channel)
		{
			post(m_io_context, std::bind<void>(handler
				, boost::asio::error::not_connected, std::size_t(0)));
			return;
		}

		TORRENT_ASSERT(!m_write_handler);
		if (m_write_handler)
		{
			post(m_io_context, std::bind<void>(handler
				, boost::asio::error::operation_not_supported, std::size_t(0)));
			return;
		}

		std::size_t bytes_added = 0;
		for (auto i = buffer_sequence_begin(buffers)
			, end(buffer_sequence_end(buffers)); i != end; ++i)
		{
			if (i->size() == 0) continue;
			add_write_buffer(i->data(), i->size());
			bytes_added += i->size();
		}
		if (bytes_added == 0)
		{
			// if we're writing 0 bytes, post handler immediately
			// asio's SSL layer depends on this behavior
			post(m_io_context, std::bind<void>(handler, error_code(), std::size_t(0)));
			return;
		}
		m_write_handler = handler;
		issue_write();
	}

private:
	void cancel_handlers(error_code const&);

	void on_message(error_code const& ec, std::vector<char> const& data);

	std::function<void(error_code const&, std::size_t)> m_read_handler;
	std::function<void(error_code const&, std::size_t)> m_write_handler;

	io_context& m_io_context;
	std::shared_ptr<rtc::PeerConnection> m_peer_connection;
	std::shared_ptr<rtc::DataChannel> m_data_channel;

	close_reason_t m_close_reason = close_reason_t::none;

	struct iovec_t
	{
		iovec_t(void* b, std::size_t l): buf(b), len(l) {}
		void* buf;
		std::size_t len;
	};

	std::vector<iovec_t> m_write_buffer;
	std::vector<iovec_t> m_read_buffer;
	std::size_t m_write_buffer_size = 0;
	std::size_t m_read_buffer_size = 0;
};

}
}

#endif

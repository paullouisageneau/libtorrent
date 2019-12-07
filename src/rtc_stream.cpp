/*

Copyright (c) 2019 Paul-Louis Ageneau
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

#include "libtorrent/aux_/rtc_stream.hpp"
#include "libtorrent/config.hpp"
#include "libtorrent/error.hpp"

#include <rtc/rtc.hpp>

namespace libtorrent {
namespace aux {

namespace ip = boost::asio::ip;
namespace errc = boost::system::errc;

using boost::asio::const_buffer;
using boost::asio::mutable_buffer;

rtc_stream_impl::rtc_stream_impl(io_context& ioc, rtc_stream_init const& init)
	: m_io_context(ioc)
	, m_peer_connection(init.peer_connection)
    , m_data_channel(init.data_channel)
{

}

rtc_stream_impl::~rtc_stream_impl()
{

}

void rtc_stream_impl::init()
{
	auto weak_this = weak_from_this();

	m_data_channel->onAvailable([this, weak_this]()
	{
		// Warning: this is called from another thread
		auto self = weak_this.lock();
		if(!self) return;

		post(m_io_context, std::bind(&rtc_stream_impl::on_message
			, self
			, error_code{}
		));
	});

	m_data_channel->onSent([this, weak_this]()
	{
		// Warning: this is called from another thread
		auto self = weak_this.lock();
		if(!self) return;

		post(m_io_context, std::bind(&rtc_stream_impl::on_sent
			, self
			, error_code{}
		));
	});

	m_data_channel->onClosed([this, weak_this]()
	{
		// Warning: this is called from another thread
		auto self = weak_this.lock();
		if(!self) return;

		post(m_io_context, std::bind(&rtc_stream_impl::cancel_handlers
			, self
			, boost::asio::error::connection_reset
		));
	});
}

void rtc_stream_impl::close()
{
	if(m_data_channel && !m_data_channel->isClosed())
		m_data_channel->close();

	cancel_handlers(boost::asio::error::operation_aborted);
}

void rtc_stream_impl::on_message(error_code const& ec)
{
	if(!m_read_handler) return;

	if(ec)
	{
		clear_read_buffers();
		post(m_io_context, std::bind(std::exchange(m_read_handler, nullptr), ec, 0));
		return;
	}

	// Fulfil pending read
	issue_read();
}

void rtc_stream_impl::on_sent(error_code const& ec)
{
	if(!m_write_handler) return;

	std::size_t bytes_written = ec ? 0 : m_write_buffer_size;

	m_write_buffer.clear();
	m_write_buffer_size = 0;
    post(m_io_context, std::bind(std::exchange(m_write_handler, nullptr), ec, bytes_written));
}

bool rtc_stream_impl::is_open() const
{
	return m_data_channel && m_data_channel->isOpen();
}

std::size_t rtc_stream_impl::available() const
{
	return m_incoming.size() + (m_data_channel ? m_data_channel->availableSize() : 0);
}

rtc_stream::endpoint_type rtc_stream_impl::remote_endpoint(error_code& ec) const
{
    if (!is_open())
    {
        ec = boost::asio::error::not_connected;
        return endpoint_type();
    }

	auto addr = m_peer_connection->remoteAddress();
	if(!addr)
	{
		ec = boost::asio::error::operation_not_supported;
		return endpoint_type();
	}

	std::size_t pos = addr->find_last_of(':');
	if(pos == std::string::npos)
	{
		ec = boost::asio::error::address_family_not_supported;
		return endpoint_type();
	}

	return endpoint_type(ip::make_address(addr->substr(0, pos), ec)
			, std::stoul(addr->substr(pos+1)));
}

rtc_stream::endpoint_type rtc_stream_impl::local_endpoint(error_code& ec) const
{
	if (!is_open())
    {
        ec = boost::asio::error::not_connected;
        return endpoint_type();
    }

	auto addr = m_peer_connection->localAddress();
	if(!addr)
	{
		ec = boost::asio::error::operation_not_supported;
		return endpoint_type();
	}

	std::size_t pos = addr->find_last_of(':');
	if(pos == std::string::npos)
	{
		ec = boost::asio::error::address_family_not_supported;
		return endpoint_type();
	}

	return endpoint_type(ip::make_address(addr->substr(0, pos), ec)
			, std::stoul(addr->substr(pos+1)));
}

void rtc_stream_impl::cancel_handlers(error_code const& ec)
{
	TORRENT_ASSERT(ec);

	auto read_handler = std::exchange(m_read_handler, nullptr);
	auto write_handler = std::exchange(m_write_handler, nullptr);

	m_read_handler = nullptr;
	m_read_buffer.clear();
	m_read_buffer_size = 0;

	m_write_handler = nullptr;
	m_write_buffer.clear();
	m_write_buffer_size = 0;

	if(read_handler) read_handler(ec, 0);
	if(write_handler) write_handler(ec, 0);
}

bool rtc_stream_impl::ensure_open()
{
	if(is_open()) return true;

    cancel_handlers(boost::asio::error::not_connected);
    return false;
}


void rtc_stream_impl::issue_read()
{
	TORRENT_ASSERT(m_read_handler);
	TORRENT_ASSERT(m_read_buffer_size > 0);

	if(!ensure_open()) return;

	error_code ec;
	std::size_t bytes_read = read_some(ec);
	if(ec || bytes_read > 0) // error or synchronous read
	{
		clear_read_buffers();
		post(m_io_context, std::bind(std::exchange(m_read_handler, nullptr), ec, bytes_read));
	}
}

void rtc_stream_impl::issue_write()
{
	TORRENT_ASSERT(m_write_handler);
	TORRENT_ASSERT(m_write_buffer_size > 0);

	if(!ensure_open()) return;

	for(auto target = m_write_buffer.begin(); target != m_write_buffer.end(); ++target)
		m_data_channel->send(static_cast<rtc::byte const*>(target->data()), target->size());
}

std::size_t rtc_stream_impl::read_some(error_code& ec)
{
	ec.clear();
	if(!ensure_open()) return 0;

	std::size_t bytes_read = 0;

	if(!m_incoming.empty())
	{
		std::size_t copied = read_data(m_incoming.data(), m_incoming.size());
		bytes_read += copied;
		if(copied < m_incoming.size())
		{
			m_incoming.erase(m_incoming.begin(), m_incoming.begin() + copied);
			return bytes_read;
		}

		m_incoming.clear();
	}

	while(!m_read_buffer.empty() && !ec)
	{
		auto message = m_data_channel->receive();
		if(!message) break;

		std::visit(rtc::overloaded
		{
			[&](rtc::binary const& bin)
			{
				char const *data = reinterpret_cast<char const*>(bin.data());
				std::size_t size = bin.size();
				std::size_t copied = read_data(data, size);
				bytes_read += copied;
				if(copied < size)
					m_incoming.assign(data + copied, data + size);
			},
			[&](rtc::string const&)
			{
				ec = errc::make_error_code(errc::bad_message);
			}
        }
        , *message);
	}

	return bytes_read;
}

void rtc_stream_impl::clear_read_buffers()
{
	m_read_buffer.clear();
    m_read_buffer_size = 0;
}

std::size_t rtc_stream_impl::read_data(char const *data, std::size_t size)
{
	std::size_t bytes_read = 0;
	auto target = m_read_buffer.begin();
	while(target != m_read_buffer.end() && size > 0) {
		std::size_t to_copy = std::min(size, target->size());
        std::memcpy(target->data(), data, to_copy);
        data += to_copy;
        size -= to_copy;
        (*target)+= to_copy;
        TORRENT_ASSERT(m_read_buffer_size >= to_copy);
        m_read_buffer_size -= to_copy;
        bytes_read += to_copy;
        if (target->size() == 0) target = m_read_buffer.erase(target);
    }
    return bytes_read;
}

rtc_stream::rtc_stream(io_context& ioc, rtc_stream_init const& init)
      : m_io_context(ioc)
      , m_impl(std::make_shared<rtc_stream_impl>(ioc, init))
{
	m_impl->init();
}

rtc_stream::rtc_stream(rtc_stream&& rhs) noexcept
	: m_io_context(rhs.m_io_context)
{
	std::swap(m_impl, rhs.m_impl);
}

rtc_stream::~rtc_stream()
{
	if(m_impl) m_impl->close();
}

}
}


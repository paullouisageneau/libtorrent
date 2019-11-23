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

rtc_stream::rtc_stream(io_context& ioc, rtc_stream_init const& init)
	: m_io_context(ioc)
	, m_peer_connection(init.peer_connection)
    , m_data_channel(init.data_channel)
{
	m_data_channel->onMessage([this](std::variant<rtc::binary, rtc::string> const& message) {
		// Warning: this is called from another thread
		std::visit([this, &message](auto const &data) {
			char const *raw = reinterpret_cast<char const*>(data.data());
			post(m_io_context, std::bind(&rtc_stream::on_message
				, this
				, boost::system::error_code{}
				, std::vector<char>(raw, raw + data.size())
			));
        }, message);
	});
}

rtc_stream::~rtc_stream()
{
	close();
}

void rtc_stream::on_message(error_code const& ec, std::vector<char> data)
{
	if(ec)
	{
		// Ignore
		return;
	}

	m_incoming_size += data.size();
	m_incoming.emplace(std::move(data));

	// Fulfil pending read if any
	if(m_read_handler) issue_read();
}

close_reason_t rtc_stream::get_close_reason()
{
	return close_reason_t::none;
}

void rtc_stream::close()
{
	m_data_channel->onMessage([](std::variant<rtc::binary, rtc::string> const&) {});
	m_data_channel->close();

	cancel_handlers(boost::asio::error::operation_aborted);
}

void rtc_stream::cancel_handlers(error_code const& ec)
{
	TORRENT_ASSERT(ec);

	if(m_read_handler) m_read_handler(ec, 0);
	if(m_write_handler) m_write_handler(ec, 0);

	m_read_handler = nullptr;
	m_read_buffer.clear();
	m_read_buffer_size = 0;

	m_write_handler = nullptr;
	m_write_buffer.clear();
	m_write_buffer_size = 0;
}

bool rtc_stream::ensure_open()
{
	if(is_open()) return true;

    cancel_handlers(boost::asio::error::not_connected);
    return false;
}

bool rtc_stream::is_open() const
{
	return !m_data_channel->isClosed();
}

std::size_t rtc_stream::available() const
{
	return m_incoming_size;
}

rtc_stream::endpoint_type rtc_stream::remote_endpoint(error_code& ec) const
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

	size_t pos = addr->find_last_of(':');
	if(pos == std::string::npos)
	{
		ec = boost::asio::error::address_family_not_supported;
		return endpoint_type();
	}

	return endpoint_type(ip::make_address(addr->substr(0, pos), ec)
			, std::stoul(addr->substr(pos+1)));
}

rtc_stream::endpoint_type rtc_stream::local_endpoint(error_code& ec) const
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

	size_t pos = addr->find_last_of(':');
	if(pos == std::string::npos)
	{
		ec = boost::asio::error::address_family_not_supported;
		return endpoint_type();
	}

	return endpoint_type(ip::make_address(addr->substr(0, pos), ec)
			, std::stoul(addr->substr(pos+1)));
}

int rtc_stream::read_buffer_size() const
{
    return m_read_buffer_size;
}

void rtc_stream::add_read_buffer(void* buf, std::size_t const len)
{
    TORRENT_ASSERT(len < INT_MAX);
    TORRENT_ASSERT(len > 0);
    TORRENT_ASSERT(buf);

    m_read_buffer.emplace_back(buf, len);
    m_read_buffer_size += len;
}

void rtc_stream::add_write_buffer(void const* buf, std::size_t const len)
{
    TORRENT_ASSERT(len < INT_MAX);
    TORRENT_ASSERT(len > 0);
    TORRENT_ASSERT(buf);

    m_write_buffer.emplace_back(const_cast<void*>(buf), len);
	m_write_buffer_size += len;
}

void rtc_stream::issue_read()
{
	TORRENT_ASSERT(m_read_handler);
	TORRENT_ASSERT(m_read_buffer_size > 0);

	if(!ensure_open()) return;

	std::size_t bytes_read = read_some(false);
	if(bytes_read > 0)
	{
		post(m_io_context, std::bind(m_read_handler, error_code{}, bytes_read));
		m_read_handler = nullptr;

		m_read_buffer_size = 0;
        m_read_buffer.clear();
	}
}

std::size_t rtc_stream::read_some(bool const clear_buffers)
{
	if(!ensure_open()) return 0;

	std::size_t ret = 0;
	auto target = m_read_buffer.begin();
	while(!m_incoming.empty() && target != m_read_buffer.end())
	{
		auto& message = m_incoming.front();
		std::size_t to_copy = std::min(message.size(), target->len);
		std::memcpy(target->buf, message.data(), to_copy);
		ret += to_copy;
		target->buf = static_cast<char*>(target->buf) + to_copy;
		TORRENT_ASSERT(target->len >= to_copy);
		target->len -= to_copy;
		TORRENT_ASSERT(m_read_buffer_size >= to_copy);
		m_read_buffer_size -= to_copy;

		// Move to next target
		if (target->len == 0) target = m_read_buffer.erase(target);

		if (to_copy == message.size())
		{
			// Consumed entire message
			m_incoming.pop();
		}
		else {
			message.erase(message.begin(), message.begin() + to_copy);
		}

		TORRENT_ASSERT(m_incoming_size >= to_copy);
		m_incoming_size -= to_copy;
	}

	if (clear_buffers)
	{
		m_read_buffer_size = 0;
		m_read_buffer.clear();
	}
	return ret;
}

void rtc_stream::issue_write()
{
	TORRENT_ASSERT(m_write_handler);
	TORRENT_ASSERT(m_write_buffer_size > 0);

	if(!ensure_open()) return;

	std::size_t bytes_written = 0;
	auto target = m_write_buffer.begin();
	while(target != m_write_buffer.end())
	{
		m_data_channel->send(static_cast<rtc::byte const*>(target->buf), target->len);
		bytes_written += target->len;
		TORRENT_ASSERT(m_write_buffer_size >= target->len);
		m_write_buffer_size -= target->len;
		target = m_write_buffer.erase(target);
	}

	post(m_io_context, std::bind(m_write_handler, error_code{}, bytes_written));
	m_write_handler = nullptr;
}

}
}


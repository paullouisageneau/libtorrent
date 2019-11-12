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
}

void rtc_stream::on_message(error_code const& ec, std::vector<char> const& data)
{
	// TODO
}

close_reason_t rtc_stream::get_close_reason()
{
	return m_close_reason;
}

void rtc_stream::close()
{
	// TODO
}

bool rtc_stream::is_open() const {
	return !m_data_channel->isClosed();
}

std::size_t rtc_stream::available() const
{
	return 0; // TODO
}

rtc_stream::endpoint_type rtc_stream::remote_endpoint(error_code& ec) const
{
    if (!m_data_channel)
    {
        ec = boost::asio::error::not_connected;
        return endpoint_type();
    }
    return endpoint_type(); // TODO
}

rtc_stream::endpoint_type rtc_stream::local_endpoint(error_code& ec) const
{
    if (!m_data_channel)
    {
        ec = boost::asio::error::not_connected;
        return endpoint_type();
    }
    return endpoint_type(); // TODO
}

int rtc_stream::read_buffer_size() const
{
    return 0; // TODO
}

void rtc_stream::add_read_buffer(void* buf, std::size_t const len)
{
    TORRENT_ASSERT(len < INT_MAX);
    TORRENT_ASSERT(len > 0);
    TORRENT_ASSERT(buf);
    m_read_buffer.emplace_back(buf, len);
    m_read_buffer_size += int(len);
}

void rtc_stream::add_write_buffer(void const* buf, std::size_t const len)
{
    TORRENT_ASSERT(len < INT_MAX);
    TORRENT_ASSERT(len > 0);
    TORRENT_ASSERT(buf);

#if TORRENT_USE_ASSERTS
    int write_buffer_size = 0;
    for (auto const& i : m_write_buffer)
    {
        TORRENT_ASSERT(std::numeric_limits<int>::max() - int(i.len) > write_buffer_size);
        write_buffer_size += int(i.len);
    }
    TORRENT_ASSERT(m_write_buffer_size == write_buffer_size);
#endif

    m_write_buffer.emplace_back(const_cast<void*>(buf), len);
	m_write_buffer_size += int(len);

#if TORRENT_USE_ASSERTS
    write_buffer_size = 0;
    for (auto const& i : m_write_buffer)
    {
        TORRENT_ASSERT(std::numeric_limits<int>::max() - int(i.len) > write_buffer_size);
        write_buffer_size += int(i.len);
    }
    TORRENT_ASSERT(m_write_buffer_size == write_buffer_size);
#endif
}

void rtc_stream::issue_read()
{
	// TODO
}

std::size_t rtc_stream::read_some(bool const /*clear_buffers*/)
{
	return 0; // TODO
}

void rtc_stream::issue_write()
{
	// TODO
}

}
}


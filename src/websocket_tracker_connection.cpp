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

#if TORRENT_USE_RTC

#include "libtorrent/websocket_tracker_connection.hpp"
#include "libtorrent/aux_/escape_string.hpp"
#include "libtorrent/aux_/session_settings.hpp"
#include "libtorrent/aux_/websocket_stream.hpp"
#include "libtorrent/config.hpp"
#include "libtorrent/io.hpp"
#include "libtorrent/ip_filter.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/socket_io.hpp"
#include "libtorrent/tracker_manager.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio> // for snprintf
#include <functional>
#include <list>
#include <locale>
#include <string>
#include <vector>

namespace libtorrent {

using namespace std::placeholders;

using websocket_stream = aux::websocket_stream;
using json = nlohmann::json;

websocket_tracker_connection::websocket_tracker_connection(io_context& ios
		, tracker_manager& man
		, tracker_request const& req
		, std::weak_ptr<request_callback> cb)
	: tracker_connection(man, req, ios, cb)
	  , m_io_context(ios)
	  , m_websocket(std::make_shared<websocket_stream>(m_io_context, m_man.host_resolver(), req.ssl_ctx))
	  , m_sending(false)
{
	queue_request(req, cb);
}

void websocket_tracker_connection::start()
{
	if(m_websocket->is_open() || m_websocket->is_connecting())
		return;

    std::shared_ptr<request_callback> cb = requester();
    if (cb)
    {
        cb->debug_log("*** WEBSOCKET_TRACKER_CONNECTING [ url: %s ]", tracker_req().url.c_str());
    }

	std::shared_ptr<websocket_tracker_connection> me(shared_from_this());
	m_websocket->async_connect(tracker_req().url, std::bind(&websocket_tracker_connection::on_connect, me, _1));
}

void websocket_tracker_connection::close()
{
	// TODO
}

void websocket_tracker_connection::queue_request(tracker_request const& req, std::weak_ptr<request_callback> cb)
{
	m_pending_requests.push({req, cb});
	if(m_websocket->is_open()) send_pending();
}

void websocket_tracker_connection::send_pending()
{
	if(!m_sending && !m_pending_requests.empty())
	{
		send(std::get<0>(m_pending_requests.front()));
	}
}

// RFC 4627: JSON text SHALL be encoded in Unicode. The default encoding is UTF-8.

std::string to_latin1(std::string const& s) {
	std::string r;
	// TODO
	return r;
}

std::string from_latin1(std::string const& s) {
	// Convert ISO-8859-1 (aka latin1) input to UTF-8
	std::string r;
	r.reserve(s.size()*2);
	for(unsigned char c : s)
	{
		if (c & 0x80)
		{
			// 2 bytes needed for code point
			r.push_back(0xC0 | (c >> 6));
			r.push_back(0x80 | (c & 0x3F));
		}
		else {
			r.push_back(c);
		}
	}
	return r;
}

void websocket_tracker_connection::send(tracker_request const& req)
{
	json payload;
	payload["action"] = "announce";
	payload["info_hash"] = from_latin1({req.info_hash.data(), std::size_t(req.info_hash.size())});
	payload["uploaded"] = req.uploaded;
	payload["downloaded"] = req.downloaded;
	payload["left"] = req.left;
	payload["corrupt"] = req.corrupt;
	payload["numwant"] = req.num_want;

	char str_key[9];
	std::snprintf(str_key, sizeof(str_key), "%08X", req.key);
	payload["key"] = str_key;

	static const char* event_string[] = {"completed", "started", "stopped", "paused"};
	if(req.event != tracker_request::none)
		payload["event"] = event_string[static_cast<int>(req.event) - 1];

	payload["peer_id"] = from_latin1({req.pid.data(), req.pid.size()});

	payload["offers"] = json::array();
	for(auto const& offer : req.offers) {
		json payload_offer;
		payload_offer["offer_id"] = from_latin1({offer.id.data(), offer.id.size()});
		payload_offer["offer"]["type"] = "offer";
		payload_offer["offer"]["sdp"] = offer.sdp;
		payload["offers"].push_back(payload_offer);
	}

	m_sending = true;
	std::string const data = payload.dump();

	std::shared_ptr<request_callback> cb = requester();
	if (cb)
	{
		cb->debug_log("*** WEBSOCKET_TRACKER_SEND [ %s ]", data.c_str());
	}

	std::shared_ptr<websocket_tracker_connection> me(shared_from_this());
    m_websocket->async_write_some(boost::asio::const_buffer(data.data(), data.size())
     		, std::bind(&websocket_tracker_connection::on_write, me, _1, _2));
}
/*
void websocket_tracker_connection::answer()
{
	auto const& req = tracker_req();

	json payload;
	payload["action"] = "announce";
    payload["info_hash"] = from_latin1({req.info_hash.data(), std::size_t(req.info_hash.size())});

    std::vector<char> offer_id; // TODO
    payload["offer_id"] = from_latin1({offer_id.data(), offer_id.size()});
    payload["peer_id"] = ""; // TODO
    payload["to_peer_id"] = ""; // TODO
    payload["answer"]["type"] = "answer";
    payload["answer"]["sdp"] = ""; // TODO
}
*/
void websocket_tracker_connection::on_connect(error_code const &ec)
{
	if(ec)
	{
		// TODO
		return;
	}

	send_pending();
}

void websocket_tracker_connection::on_timeout(error_code const& ec)
{
	if(ec)
	{
		// TODO
		return;
	}
}

void websocket_tracker_connection::on_read(error_code const& ec, std::size_t /* bytes_read */)
{
	if(ec)
    {
        // TODO
        return;
    }
}

void websocket_tracker_connection::on_write(error_code const& ec, std::size_t /* bytes_written */)
{
	m_sending = false;

	if(!m_pending_requests.empty())
	{
		// Update requester
		m_requester = std::get<1>(m_pending_requests.front());

		m_pending_requests.pop();
	}

	if(ec)
	{
		// TODO
		return;
	}

	// Continue sending
	send_pending();

	// TODO: read
}

}

#endif


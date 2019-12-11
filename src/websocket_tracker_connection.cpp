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

#include "libtorrent/config.hpp" // for TORRENT_USE_RTC

#if TORRENT_USE_RTC

#include "libtorrent/aux_/escape_string.hpp"
#include "libtorrent/aux_/session_settings.hpp"
#include "libtorrent/aux_/latin1.hpp"
#include "libtorrent/io.hpp"
#include "libtorrent/ip_filter.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/socket_io.hpp"
#include "libtorrent/span.hpp"
#include "libtorrent/tracker_manager.hpp"
#include "libtorrent/websocket_tracker_connection.hpp"

#include "nlohmann/json.hpp"

#include <boost/system/system_error.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio> // for snprintf
#include <exception>
#include <functional>
#include <list>
#include <locale>
#include <string>
#include <string_view>
#include <vector>

namespace libtorrent {

using namespace std::placeholders;
namespace errc = boost::system::errc;

using json = nlohmann::json;

websocket_tracker_connection::websocket_tracker_connection(io_context& ios
		, tracker_manager& man
		, tracker_request const& req
		, std::weak_ptr<request_callback> cb)
	: tracker_connection(man, req, ios, cb)
	  , m_io_context(ios)
	  , m_websocket(std::make_shared<aux::websocket_stream>(m_io_context, m_man.host_resolver(), req.ssl_ctx))
{
	aux::session_settings const& settings = m_man.settings();

	// in anonymous mode we omit the user agent to mitigate fingerprinting of
    // the client. Private torrents is an exception because some private
    // trackers may require the user agent
    std::string const user_agent = settings.get_bool(settings_pack::anonymous_mode)
			&& !tracker_req().private_torrent ? "" : settings.get_str(settings_pack::user_agent);
	m_websocket->set_user_agent(user_agent);

	queue_request(req, cb);
}

websocket_tracker_connection::~websocket_tracker_connection()
{
	close();
}

void websocket_tracker_connection::start()
{
	if(m_websocket->is_open() || m_websocket->is_connecting())
		return;

#ifndef TORRENT_DISABLE_LOGGING
    std::shared_ptr<request_callback> cb = requester();
    if (cb) cb->debug_log("*** WEBSOCKET_TRACKER_CONNECT [ url: %s ]", tracker_req().url.c_str());
#endif

	std::shared_ptr<websocket_tracker_connection> self(shared_from_this());
	m_websocket->async_connect(tracker_req().url, std::bind(&websocket_tracker_connection::on_connect, self, _1));
}

void websocket_tracker_connection::close()
{
	if(m_websocket->is_open() || m_websocket->is_connecting())
	{
		m_websocket->close();
	}
}

void websocket_tracker_connection::queue_request(tracker_request req, std::weak_ptr<request_callback> cb)
{
	m_pending.emplace(tracker_message{std::move(req)}, cb);
	if(m_websocket->is_open()) send_pending();
}

void websocket_tracker_connection::queue_answer(tracker_answer ans)
{
	m_pending.emplace(tracker_message{std::move(ans)}, std::weak_ptr<request_callback>{});
	if(m_websocket->is_open()) send_pending();
}

void websocket_tracker_connection::send_pending()
{
	if(m_sending || m_pending.empty()) return;

	m_sending = true;

	tracker_message msg;
    std::weak_ptr<request_callback> cb;
    std::tie(msg, cb) = std::move(m_pending.front());
	m_pending.pop();

	boost::apply_visitor([&](auto const& m)
		{
			// Update requester and store callback
			if(cb.lock())
			{
				m_requester = cb;
				m_callbacks[m.info_hash] = cb;
			}

			do_send(m);
		}
		, msg
	);
}

void websocket_tracker_connection::do_send(tracker_request const& req)
{
	// Update request
	m_req = req;

	json payload;
	payload["action"] = "announce";
	payload["info_hash"] = aux::from_latin1(req.info_hash);
	payload["uploaded"] = req.uploaded;
	payload["downloaded"] = req.downloaded;
	payload["left"] = req.left;
	payload["corrupt"] = req.corrupt;
	payload["numwant"] = req.num_want;

	char str_key[9];
	std::snprintf(str_key, sizeof(str_key), "%08X", req.key);
	payload["key"] = str_key;

	static const char* event_string[] = {"completed", "started", "stopped", "paused"};
	if(req.event != event_t::none)
		payload["event"] = event_string[static_cast<int>(req.event) - 1];

	payload["peer_id"] = aux::from_latin1(req.pid);

	payload["offers"] = json::array();
	for(auto const& offer : req.offers)
	{
		json payload_offer;
		payload_offer["offer_id"] = aux::from_latin1(offer.id);
		payload_offer["offer"]["type"] = "offer";
		payload_offer["offer"]["sdp"] = offer.sdp;
		payload["offers"].push_back(payload_offer);
	}

	std::string const data = payload.dump();

#ifndef TORRENT_DISABLE_LOGGING
	std::shared_ptr<request_callback> cb = requester();
	if (cb) cb->debug_log("*** WEBSOCKET_TRACKER_WRITE [ %s ]", data.c_str());
#endif

	std::shared_ptr<websocket_tracker_connection> self(shared_from_this());
    m_websocket->async_write(boost::asio::const_buffer(data.data(), data.size())
     		, std::bind(&websocket_tracker_connection::on_write, self, _1, _2));
}

void websocket_tracker_connection::do_send(tracker_answer const& ans)
{
    json payload;
    payload["action"] = "announce";
    payload["info_hash"] = aux::from_latin1(ans.info_hash);
    payload["offer_id"] = aux::from_latin1(ans.answer.offer_id);
    payload["to_peer_id"] = aux::from_latin1(ans.answer.pid);
    payload["peer_id"] =  aux::from_latin1(ans.pid);
    payload["answer"]["type"] = "answer";
    payload["answer"]["sdp"] = ans.answer.sdp;

	std::string const data = payload.dump();

#ifndef TORRENT_DISABLE_LOGGIN
	std::shared_ptr<request_callback> cb = requester();
	if (cb) cb->debug_log("*** WEBSOCKET_TRACKER_WRITE [ %s ]", data.c_str());
#endif

	std::shared_ptr<websocket_tracker_connection> self(shared_from_this());
	m_websocket->async_write(boost::asio::const_buffer(data.data(), data.size())
			, std::bind(&websocket_tracker_connection::on_write, self, _1, _2));
}

void websocket_tracker_connection::do_read()
{
	if(!m_websocket->is_open()) return;

	// Can be replaced by m_read_buffer.clear() with boost 1.70+
	if(m_read_buffer.size() > 0) m_read_buffer.consume(m_read_buffer.size());

	std::shared_ptr<websocket_tracker_connection> self(shared_from_this());
	m_websocket->async_read(m_read_buffer
            , std::bind(&websocket_tracker_connection::on_read, self, _1, _2));
}

void websocket_tracker_connection::on_connect(error_code const& ec)
{
	if(ec)
	{
#ifndef TORRENT_DISABLE_LOGGIN
		std::shared_ptr<request_callback> cb = requester();
		if (cb) cb->debug_log("*** WEBSOCKET_TRACKER_CONNECT ERROR [ url: %s, error: %d ]"
				, tracker_req().url.c_str(), int(ec.value()));
#endif
		m_pending = {};
		return;
	}

#ifndef TORRENT_DISABLE_LOGGIN
	std::shared_ptr<request_callback> cb = requester();
    if (cb) cb->debug_log("*** WEBSOCKET_TRACKER_CONNECT SUCCESS [ url: %s ]"
    		, tracker_req().url.c_str());
#endif

	send_pending();
	do_read();
}

void websocket_tracker_connection::on_timeout(error_code const& /*ec*/)
{
	// Dummy
}

void websocket_tracker_connection::on_read(error_code const& ec, std::size_t /* bytes_read */)
{
	std::shared_ptr<request_callback> cb = requester();

	if(ec)
    {
		if(cb) cb->tracker_request_error(
					tracker_req()
					, ec
					, ec.message()
					, seconds32(120));
        return;
    }

    try {
    	auto const& buf = m_read_buffer.data();
		auto const data = static_cast<char const*>(buf.data());
    	auto const size = buf.size();

		json payload;
		payload = json::parse(data, data + size);

#ifndef TORRENT_DISABLE_LOGGING
		std::string str(data, size);
		if(cb) cb->debug_log("*** WEBSOCKET_TRACKER_READ [ size: %d data: %s ]", int(str.size()), str.c_str());
#endif

		auto it = payload.find("info_hash");
		if(it == payload.end())
			throw std::invalid_argument("no info hash in message");

		auto const raw_info_hash = aux::to_latin1(it->get<std::string>());
		if(raw_info_hash.size() != 20)
			throw std::invalid_argument("invalid info hash size " + std::to_string(raw_info_hash.size()));

    	auto const info_hash = sha1_hash(span<char const>{raw_info_hash.data(), 20});

		// Find the correct callback given the info_hash
		std::shared_ptr<request_callback> locked;
		if(auto c = m_callbacks.find(info_hash); c != m_callbacks.end()) locked = c->second.lock();
		if (!locked)
		{
			m_callbacks.erase(info_hash);
			throw std::invalid_argument("no callback for info hash");
		}
		cb = locked;

		if(auto it = payload.find("offer"); it != payload.end())
		{
			auto const &payload_offer = *it;
			auto sdp = payload_offer["sdp"].get<std::string>();
			auto id = aux::to_latin1(payload["offer_id"].get<std::string>());
			auto pid = aux::to_latin1(payload["peer_id"].get<std::string>());

			std::shared_ptr<websocket_tracker_connection> self(shared_from_this());
			aux::rtc_offer offer{aux::rtc_offer_id(span<char const>(id)), peer_id(pid), std::move(sdp)
				, [this, info_hash, id, pid](peer_id const& local_pid, aux::rtc_answer const& answer) {
					queue_answer({std::move(info_hash), std::move(local_pid), std::move(answer)});
				}
			};
			cb->on_rtc_offer(offer);
		}

		if(auto it = payload.find("answer"); it != payload.end())
		{
			auto const &payload_answer = *it;
			auto sdp = payload_answer["sdp"].get<std::string>();
			auto id = aux::to_latin1(payload["offer_id"].get<std::string>());
			auto pid = aux::to_latin1(payload["peer_id"].get<std::string>());

			aux::rtc_answer answer{aux::rtc_offer_id(span<char const>(id)), peer_id(pid), std::move(sdp)};
			cb->on_rtc_answer(answer);
		}

		if(payload.find("interval") != payload.end())
		{
			tracker_response resp;
			resp.interval = std::max(seconds32{payload.value<int>("interval", 120)}
				, seconds32{m_man.settings().get_int(settings_pack::min_websocket_announce_interval)});
			resp.min_interval = seconds32{payload.value<int>("min_interval", 60)};
			resp.complete = payload.value<int>("complete", -1);
			resp.incomplete = payload.value<int>("incomplete", -1);
			resp.downloaded = payload.value<int>("downloaded", -1);

			cb->tracker_response(tracker_req(), {}, {}, resp);
		}
	}
	catch(std::exception const& e)
	{
#ifndef TORRENT_DISABLE_LOGGING
        if(cb) cb->debug_log("*** WEBSOCKET_TRACKER_READ ERROR [ %s ]", e.what());
#endif
		if(cb) cb->tracker_request_error(
                    tracker_req()
                    , errc::make_error_code(errc::bad_message)
                    , e.what()
                    , seconds32(120));
	}

	// Continue reading
	do_read();
}

void websocket_tracker_connection::on_write(error_code const& ec, std::size_t /* bytes_written */)
{
	m_sending = false;

	if(ec)
	{
		std::shared_ptr<request_callback> cb = requester();
		if(cb) cb->tracker_request_error(
					tracker_req()
					, ec
					, ec.message()
					, seconds32(120));
		return;
	}

	// Continue sending
	send_pending();
}

}

#endif // TORRENT_USE_RTC


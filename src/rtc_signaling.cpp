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

#include "libtorrent/aux_/rtc_signaling.hpp"
#include "libtorrent/aux_/generate_peer_id.hpp"
#include "libtorrent/aux_/rtc_stream.hpp"
#include "libtorrent/random.hpp"
#include "libtorrent/torrent.hpp"

#include "rtc/rtc.hpp"

#include <cstdarg>

namespace libtorrent {
namespace aux {

rtc_signaling::rtc_signaling(io_context& ioc, torrent* t, rtc_stream_handler handler)
	: m_io_context(ioc)
	, m_torrent(t)
	, m_rtc_stream_handler(handler)
{
	debug_log("*** RTC signaling created");
}

rtc_signaling::~rtc_signaling()
{
	// TODO
}

alert_manager& rtc_signaling::alerts() const
{
    return m_torrent->alerts();
}

rtc_offer_id rtc_signaling::generate_offer_id() const
{
	rtc_offer_id id;
	do {
		aux::random_bytes({id.data(), int(id.size())});
	}
	while(m_connections.find(id) != m_connections.end());

	return id;
}

void rtc_signaling::generate_offers(int count, offers_handler handler)
{
	debug_log("*** RTC signaling generating %d offers", count);

	m_offer_batches.push({count, handler});

	while(count--)
	{
		rtc_offer_id offer_id = generate_offer_id();
		peer_id pid = aux::generate_peer_id(m_torrent->settings());

		auto& conn = create_connection(offer_id, [this, offer_id, pid](std::string const& sdp) {
			rtc_offer offer{std::move(offer_id), std::move(pid), sdp, {}};
			post(m_io_context, std::bind(&rtc_signaling::on_generated_offer
				, this
                , boost::system::error_code{}
                , offer
            ));
		});

		auto dc = conn.peer_connection->createDataChannel("webtorrent");
		dc->onOpen([this, offer_id, dc]() {
			// Warning: this is called from another thread
			post(m_io_context, std::bind(&rtc_signaling::on_data_channel
				, this
				, boost::system::error_code{}
				, offer_id
				, dc
			));
		});

		// We need to maintain the DataChannel alive
		conn.data_channel = dc;
	}
}

void rtc_signaling::process_offer(rtc_offer const& offer)
{
	debug_log("*** RTC signaling processing remote offer");

	auto& conn = create_connection(offer.id, [this, offer](std::string const& sdp) {
		rtc_answer answer{offer.id, offer.pid, sdp};
        post(m_io_context, std::bind(&rtc_signaling::on_generated_answer
			, this
            , boost::system::error_code{}
            , answer
            , offer
        ));
	});

	conn.pid = offer.pid;
	conn.peer_connection->setRemoteDescription({offer.sdp, "offer"});
}

void rtc_signaling::process_answer(rtc_answer const& answer)
{
	debug_log("*** RTC signaling processing remote answer");

	auto it = m_connections.find(answer.offer_id);
	if(it == m_connections.end())
	{
		debug_log("*** OOPS: Remote RTC answer does not match an offer");
		return;
	}

	connection& conn = it->second;
	if(conn.pid)
	{
		debug_log("*** OOPS: Local RTC offer already got an answer");
		return;
	}

	conn.pid = answer.pid;
	conn.peer_connection->setRemoteDescription({answer.sdp, "answer"});
}

rtc_signaling::connection& rtc_signaling::create_connection(rtc_offer_id const& offer_id, description_handler handler)
{
	if(auto it = m_connections.find(offer_id); it != m_connections.end())
	{
		debug_log("*** WARNING: An RTC connection already exists for offer id");
		return it->second;
	}

	debug_log("*** RTC signaling creating connection");

	rtc::Configuration config;
	config.iceServers.emplace_back("stun.l.google.com:19302");

	auto pc = std::make_shared<rtc::PeerConnection>(config);
	pc->onStateChange([this, pc](rtc::PeerConnection::State state) {
		post(m_io_context, [this, state, pc]() {
			switch (state) {
			case rtc::PeerConnection::State::Connecting:
				debug_log("*** RTC peer connection state: CONNECTING");
				break;
			case rtc::PeerConnection::State::Connected:
				debug_log("*** RTC peer connection state: CONNECTED");
				break;
			case rtc::PeerConnection::State::Failed:
				debug_log("*** RTC peer connection state: FAILED");
				break;
			default:
				// Ignore
				break;
			}
        });
    });
	pc->onGatheringStateChange([this, offer_id, handler, pc](
			rtc::PeerConnection::GatheringState state)
	{
		// Warning: this is called from another thread
		if(state == rtc::PeerConnection::GatheringState::Complete)
		{
			post(m_io_context, std::bind(handler, *pc->localDescription()));
		}
	});
	pc->onDataChannel([this, offer_id](
				std::shared_ptr<rtc::DataChannel> dc)
	{
        // Warning: this is called from another thread
		post(m_io_context, std::bind(&rtc_signaling::on_data_channel
        	, this
        	, boost::system::error_code{}
        	, offer_id
        	, dc
        ));
    });

	connection conn;
	conn.peer_connection = pc;
	auto it = m_connections.emplace(offer_id, std::move(conn)).first;
	return it->second;
}

void rtc_signaling::on_generated_offer(error_code const& ec, rtc_offer offer)
{
	debug_log("*** RTC signaling generated offer");

	while(!m_offer_batches.empty() && m_offer_batches.front().is_complete())
	{
		m_offer_batches.pop();
	}

	if(m_offer_batches.empty())
	{
		// TODO: should not happen
		return;
	}

	m_offer_batches.front().add(std::forward<rtc_offer>(offer));
}

void rtc_signaling::on_generated_answer(error_code const& ec, rtc_answer answer, rtc_offer offer)
{
    if(ec)
    {
        // TODO
        return;
    }

	debug_log("*** RTC signaling generated answer");

	if(!offer.answer_callback)
	{
		debug_log("*** OOPS: RTC offer has no answer callback");
		return;
	}

	peer_id pid = aux::generate_peer_id(m_torrent->settings());
	offer.answer_callback(pid, answer);
}

void rtc_signaling::on_data_channel(error_code const& ec, rtc_offer_id offer_id, std::shared_ptr<rtc::DataChannel> dc)
{
	if(ec)
	{
		// TODO
		return;
	}

	debug_log("*** RTC signaling data channel open");

	auto it = m_connections.find(offer_id);
    if(it == m_connections.end())
    {
        debug_log("*** OOPS: RTC data channel does not match a connection");
        return;
    }

	connection const& conn = it->second;
    if(!conn.pid)
    {
        debug_log("*** OOPS: RTC data channel has no corresponding peer id");
        return;
    }

	rtc_stream_init init{conn.peer_connection, dc};
	m_rtc_stream_handler(*conn.pid, init);
    m_connections.erase(it);
}

rtc_signaling::offer_batch::offer_batch(int count, rtc_signaling::offers_handler handler)
	: m_count(count)
	, m_handler(handler)
{
	if(m_count == 0)
	{
		m_handler(boost::system::error_code{}, {});
	}
}

void rtc_signaling::offer_batch::add(rtc_offer &&offer)
{
	m_offers.push_back(std::forward<rtc_offer>(offer));

	if(is_complete())
	{
		m_handler(boost::system::error_code{}, m_offers);
	}
}

bool rtc_signaling::offer_batch::is_complete() const
{
	return int(m_offers.size()) == m_count;
}

#ifndef TORRENT_DISABLE_LOGGING
bool rtc_signaling::should_log() const
{
	return alerts().should_post<torrent_log_alert>();
}

TORRENT_FORMAT(2,3)
void rtc_signaling::debug_log(char const* fmt, ...) const noexcept try
{
	if (!alerts().should_post<torrent_log_alert>()) return;

	va_list v;
	va_start(v, fmt);
	alerts().emplace_alert<torrent_log_alert>(const_cast<torrent*>(m_torrent)->get_handle(), fmt, v);
	va_end(v);
}
catch (std::exception const&) {}
#endif

}
}


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
#include "libtorrent/aux_/rtc_stream.hpp"

#include "rtc/rtc.hpp"

namespace libtorrent {
namespace aux {

rtc_signaling::rtc_signaling(io_context& ioc, rtc_stream_handler handler)
	: m_io_context(ioc)
	, m_rtc_stream_handler(handler)
{

}

rtc_signaling::~rtc_signaling()
{
	// TODO
}

void rtc_signaling::generate_offers(int count, offers_handler handler)
{
	m_offer_batches.push({count, handler});

	while(count--)
	{
		rtc_offer_id offer_id; // TODO
		auto& conn = create_connection(offer_id);
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

void rtc_signaling::process_offer(rtc_offer const &offer)
{
	auto& conn = create_connection(offer.id);
	conn.peer_connection->setRemoteDescription({offer.sdp, "offer"});
}

void rtc_signaling::process_answer(rtc_answer const &answer)
{
	auto it = m_connections.find(answer.offer_id);
	if(it == m_connections.end())
	{
		// TODO: error
		return;
	}

	auto& conn = it->second;
	conn.peer_connection->setRemoteDescription({answer.sdp, "answer"});
}

rtc_signaling::connection& rtc_signaling::create_connection(const rtc_offer_id &offer_id)
{
	rtc::Configuration config;

	auto pc = std::make_shared<rtc::PeerConnection>(config);
	pc->onGatheringStateChange([this, offer_id, pc](
			rtc::PeerConnection::GatheringState state)
	{
		// Warning: this is called from another thread
		if(state == rtc::PeerConnection::GatheringState::Complete)
		{
			rtc_offer offer{std::move(offer_id), *pc->localDescription()};
			post(m_io_context, std::bind(&rtc_signaling::on_generated_offer
				, this
				, boost::system::error_code{}
				, std::move(offer)
			));
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

void rtc_signaling::on_data_channel(error_code const& ec, rtc_offer_id offer_id, std::shared_ptr<rtc::DataChannel> dc)
{
	if(ec)
	{
		// TODO
		return;
	}

	auto it = m_connections.find(offer_id);
    if(it == m_connections.end())
    {
        // TODO: error
        return;
    }

    auto pc = it->second.peer_connection;
    m_connections.erase(it);
	m_rtc_stream_handler(rtc_stream(m_io_context, pc, dc));
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

}
}


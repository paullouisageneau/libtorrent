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

#ifndef TORRENT_RTC_SIGNALING_HPP_INCLUDED
#define TORRENT_RTC_SIGNALING_HPP_INCLUDED

#include "libtorrent/error_code.hpp"
#include "libtorrent/io.hpp"
#include "libtorrent/io_context.hpp"
#include "libtorrent/time.hpp"

#include <boost/functional/hash.hpp>
#include <boost/system/error_code.hpp>

#include <functional>
#include <memory>
#include <queue>
#include <vector>

namespace rtc {
	class PeerConnection;
	class DataChannel;
}

namespace libtorrent {
namespace aux {

class rtc_stream;

class rtc_offer_id : public std::vector<char> {};

struct rtc_offer_id_hash
{
	std::size_t operator()(const rtc_offer_id &id) const
	{
        return boost::hash<std::vector<char>>{}(id);
    }
};

struct rtc_offer
{
	rtc_offer_id id;
	std::string sdp;
};

struct rtc_answer
{
	rtc_offer_id offer_id;
	std::string sdp;
};

// This class handles client signaling for WebRTC DataChannels
class TORRENT_EXTRA_EXPORT rtc_signaling
{
public:
	using offers_handler = std::function<void(error_code const&, std::vector<rtc_offer> const&)>;
	using rtc_stream_handler = std::function<void(rtc_stream&&)>;

	explicit rtc_signaling(io_context& ioc, rtc_stream_handler handler);
	~rtc_signaling();
	rtc_signaling& operator=(rtc_signaling const&) = delete;
	rtc_signaling(rtc_signaling const&) = delete;
	rtc_signaling& operator=(rtc_signaling&&) noexcept = delete;
	rtc_signaling(rtc_signaling&&) noexcept = delete;

	void generate_offers(int count, offers_handler handler);
	void process_offer(rtc_offer const &offer);
	void process_answer(rtc_answer const &answer);

private:
	struct connection
	{
		connection() : created(std::chrono::steady_clock::now()) {}

		std::shared_ptr<rtc::PeerConnection> peer_connection;
		std::shared_ptr<rtc::DataChannel> data_channel;
		std::chrono::steady_clock::time_point created;
	};

	connection& create_connection(const rtc_offer_id &offer_id);
	void on_generated_offer(error_code const& ec, rtc_offer offer);
	void on_data_channel(error_code const& ec, rtc_offer_id offer_id, std::shared_ptr<rtc::DataChannel> dc);

	io_context& m_io_context;
	const rtc_stream_handler m_rtc_stream_handler;

	std::unordered_map<rtc_offer_id, connection, rtc_offer_id_hash> m_connections;
	std::queue<rtc_offer_id> m_queue;

	class offer_batch
	{
	public:
		offer_batch(int count, offers_handler handler);
		void add(rtc_offer &&offer);
		bool is_complete() const;

	private:
		const int m_count;
		const offers_handler m_handler;
		std::vector<rtc_offer> m_offers;
	};

	std::queue<offer_batch> m_offer_batches;
};

}
}

#endif

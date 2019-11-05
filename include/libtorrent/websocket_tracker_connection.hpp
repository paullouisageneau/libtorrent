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

#ifndef TORRENT_WEBSOCKET_TRACKER_CONNECTION_HPP_INCLUDED
#define TORRENT_WEBSOCKET_TRACKER_CONNECTION_HPP_INCLUDED

#include <memory>
#include <queue>

#include "libtorrent/config.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/peer_id.hpp"
#include "libtorrent/resolver_interface.hpp"
#include "libtorrent/tracker_manager.hpp" // for tracker_connection

namespace libtorrent {

class tracker_manager;
struct peer_entry;

namespace aux {
	class websocket_stream;
}

class TORRENT_EXTRA_EXPORT websocket_tracker_connection
	: public tracker_connection
{
	friend class tracker_manager;

public:
	websocket_tracker_connection(
		io_context& ios
		, tracker_manager& man
		, tracker_request const& req
		, std::weak_ptr<request_callback> cb);

	void start() override;
	void close() override;

	void queue_request(tracker_request const& req, std::weak_ptr<request_callback> cb);

private:
	std::shared_ptr<websocket_tracker_connection> shared_from_this()
	{
		return std::static_pointer_cast<websocket_tracker_connection>(
			tracker_connection::shared_from_this());
	}

	void send_pending();
	void send(tracker_request const& req);
	void on_connect(error_code const& ec);
	void on_timeout(error_code const& ec);
	void on_read(error_code const& ec, std::size_t bytes_read);
	void on_write(error_code const& ec, std::size_t bytes_written);

	io_context& m_io_context;
	std::shared_ptr<aux::websocket_stream> m_websocket;

	std::queue<tracker_request> m_pending_requests;
	bool m_sending;
};

}

#endif // TORRENT_WEBSOCKET_TRACKER_CONNECTION_HPP_INCLUDED

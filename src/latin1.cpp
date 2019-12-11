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

#include "libtorrent/aux_/latin1.hpp"

namespace libtorrent {
namespace aux {

std::string from_latin1(span<char const> s)
{
	std::string r;
	r.reserve(s.size()*2);
	for(auto it = s.begin(); it != s.end(); ++it)
	{
		unsigned char c = *it;
		if (!(c & 0x80))
		{
			r.push_back(c);
		}
		else {
			// 2 bytes needed for code point
			r.push_back(0xC0 | (c >> 6));
			r.push_back(0x80 | (c & 0x3F));
		}
	}
	return r;
}

std::string to_latin1(std::string_view sv)
{
	std::string r;
	r.reserve(sv.size());
	auto it = sv.begin();
	while(it != sv.end())
	{
		unsigned char c = *it;

		// Find the number of bytes
		size_t len;
		if(!(c & 0x80)) len = 1;
		else if (!(c & 0x20)) len = 2;
		else if (!(c & 0x10)) len = 3;
		else len = 4;

		// Read the code point
		unsigned int cp;
		if(len == 1) cp = c; // fast path
		else {
			c &= 0xFF >> (len + 1);
			cp = c << ((len - 1) * 6);
			while(--len)
			{
				if(++it == sv.end())
					throw std::invalid_argument("truncated UTF-8 string");
				c = *it;
				c &= 0x7F;
				cp |= c << ((len - 1) * 6);
			}
		}

		if(cp > 0xFF)
			throw std::invalid_argument("code point out of latin1 range: " + std::to_string(cp));

		r.push_back(static_cast<unsigned char>(cp));
		++it;
	}
	return r;
}

}
}


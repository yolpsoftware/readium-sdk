//
//  filter_chain.cpp
//  ePub3
//
//  Created by Jim Dovey on 2013-08-27.
//  Copyright (c) 2014 Readium Foundation and/or its licensees. All rights reserved.
//  
//  This program is distributed in the hope that it will be useful, but WITHOUT ANY 
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  
//  
//  Licensed under Gnu Affero General Public License Version 3 (provided, notwithstanding this notice, 
//  Readium Foundation reserves the right to license this material under a different separate license, 
//  and if you have done so, the terms of that separate license control and the following references 
//  to GPL do not apply).
//  
//  This program is free software: you can redistribute it and/or modify it under the terms of the GNU 
//  Affero General Public License as published by the Free Software Foundation, either version 3 of 
//  the License, or (at your option) any later version. You should have received a copy of the GNU 
//  Affero General Public License along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "filter_chain_byte_stream.h"
#include "../ePub/manifest.h"
#include "filter.h"
#include "byte_buffer.h"
#include "make_unique.h"
#include <iostream>

#if !EPUB_OS(WINDOWS)
# define memcpy_s(dst, dstLen, src, srcLen) memcpy(dst, src, srcLen)
#endif

EPUB3_BEGIN_NAMESPACE

FilterChainByteStream::~FilterChainByteStream()
{
}

FilterChainByteStream::FilterChainByteStream(std::vector<ContentFilterPtr>& filters, ConstManifestItemPtr &manifestItem)
: _filters(), _needs_cache(false), _cache(), _read_cache()
{
    _input = NULL;
	_cache.SetUsesSecureErasure();
	_read_cache.SetUsesSecureErasure();
    
	for (auto& filter : filters)
	{
		_filters.emplace_back(filter, std::unique_ptr<FilterContext>(filter->MakeFilterContext(manifestItem)));
        if (filter->GetOperatingMode() == ContentFilter::OperatingMode::RequiresCompleteData && !_needs_cache)
			_needs_cache = true;
	}
}

FilterChainByteStream::FilterChainByteStream(std::unique_ptr<ByteStream>&& input, std::vector<ContentFilterPtr>& filters, ConstManifestItemPtr manifestItem)
: _input(std::move(input)), _filters(), _needs_cache(false), _cache(), _read_cache()
{
	_cache.SetUsesSecureErasure();
	_read_cache.SetUsesSecureErasure();

	for (auto& filter : filters)
	{
		_filters.emplace_back(filter, std::unique_ptr<FilterContext>(filter->MakeFilterContext(manifestItem)));
		if (filter->GetOperatingMode() == ContentFilter::OperatingMode::RequiresCompleteData && !_needs_cache)
			_needs_cache = true;
	}
}

ByteStream::size_type FilterChainByteStream::ReadBytes(void* bytes, size_type len)
{
    if (len == 0) return 0;

	if (_needs_cache)
	{
		if (_cache.GetBufferSize() == 0 && _input->AtEnd() == false)
			CacheBytes();

		return ReadBytesFromCache(bytes, len);
	}

	if (_read_cache.GetBufferSize() > 0)
	{
		size_type toMove = std::min(len, _read_cache.GetBufferSize());
		::memcpy_s(bytes, len, _read_cache.GetBytes(), toMove);
		_read_cache.RemoveBytes(toMove);
		return toMove;
	}
	else 
	{
		size_type result = _input->ReadBytes(bytes, len);
        if (result == 0) return 0;

		result = FilterBytes(bytes, result);
		size_type toMove = std::min(len, _read_cache.GetBufferSize());
		::memcpy_s(bytes, len, _read_cache.GetBytes(), toMove);
		_read_cache.RemoveBytes(toMove);
		return toMove;
	}
}

ByteStream::size_type FilterChainByteStream::FilterBytes(void* bytes, size_type len)
{
    if (len == 0) return 0;

	size_type result = len;
	ByteBuffer buf(reinterpret_cast<uint8_t*>(bytes), len);
	buf.SetUsesSecureErasure();

	for (auto& pair : _filters)
	{
		size_type filteredLen = 0;
		void* filteredData = pair.first->FilterData(pair.second.get(), buf.GetBytes(), buf.GetBufferSize(), &filteredLen);
		if (filteredData == nullptr || filteredLen == 0) {
			if (filteredData != nullptr && filteredData != buf.GetBytes())
				delete[] reinterpret_cast<uint8_t*>(filteredData);
			throw std::logic_error("ChainLinkProcessor: ContentFilter::FilterData() returned no data!");
		}

		result = filteredLen;

		if (filteredData != buf.GetBytes())
		{
			buf = ByteBuffer(reinterpret_cast<uint8_t*>(filteredData), result);
			delete[] reinterpret_cast<uint8_t*>(filteredData);
		}
		else if (result != buf.GetBufferSize())
		{
			buf = ByteBuffer(reinterpret_cast<uint8_t*>(filteredData), result);
		}
	}
	
	_read_cache = std::move(buf);

	return result;
}

ByteStream::size_type FilterChainByteStream::ReadBytesFromCache(void* bytes, size_type len)
{
    if (len == 0) return 0;

	size_type numToRead = std::min(len, size_type(_cache.GetBufferSize()));
	::memcpy_s(bytes, len, _cache.GetBytes(), numToRead);
	_cache.RemoveBytes(numToRead);
	return numToRead;
}

void FilterChainByteStream::CacheBytes()
{
	// read everything from the input stream
#define _TMP_BUF_LEN 16*1024
	uint8_t buf[_TMP_BUF_LEN] = {};
	while (_input->AtEnd() == false)
	{
		size_type numRead = _input->ReadBytes(buf, _TMP_BUF_LEN);
		if (numRead == 0)
			break;
		if (numRead > 0)
			_cache.AddBytes(buf, numRead);
	}

    if (_cache.GetBufferSize() == 0) return;

	// filter everything completely
	size_type filtered = FilterBytes(_cache.GetBytes(), _cache.GetBufferSize());

	if (filtered > 0)
	{
		_cache = std::move(_read_cache);
		_read_cache.RemoveBytes(_read_cache.GetBufferSize());
	}

	// this potentially contains decrypted data, so use secure erasure
	_cache.SetUsesSecureErasure();
}

EPUB3_END_NAMESPACE
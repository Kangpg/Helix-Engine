#pragma once

// =============================================================================
// Phase 2 — 길이 프리픽스 프레이밍 (header-only)
//
// TCP는 스트림이라 메시지 경계가 없다. 헤더(길이)를 붙여 완결 메시지를 잘라낸다.
// 설정 가능: 헤더 크기(2/4바이트), 엔디안, 최대 메시지 크기(손상/과대 패킷 가드).
//
// "길이+데이터"만으로는 손상 감지가 안 되므로 maxMessageSize로 방어한다.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace helix::proto
{
	struct FrameCodec
	{
		std::uint32_t headerSize = 4;            // 2 또는 4
		bool bigEndian = true;                   // 네트워크 바이트 순서
		std::uint32_t maxMessageSize = 1u << 20; // 1 MiB
	};

	// 페이로드 길이를 헤더(headerSize 바이트)로 out에 기록. 한계 초과 시 false.
	inline bool EncodeHeader(const FrameCodec& c, std::uint32_t len, std::byte* out)
	{
		if (len > c.maxMessageSize) { return false; }
		if (c.headerSize == 2)
		{
			if (len > 0xFFFFu) { return false; }
			if (c.bigEndian) { out[0] = std::byte((len >> 8) & 0xFF); out[1] = std::byte(len & 0xFF); }
			else { out[0] = std::byte(len & 0xFF); out[1] = std::byte((len >> 8) & 0xFF); }
		}
		else // 4
		{
			if (c.bigEndian)
			{
				out[0] = std::byte((len >> 24) & 0xFF); out[1] = std::byte((len >> 16) & 0xFF);
				out[2] = std::byte((len >> 8) & 0xFF);  out[3] = std::byte(len & 0xFF);
			}
			else
			{
				out[0] = std::byte(len & 0xFF);        out[1] = std::byte((len >> 8) & 0xFF);
				out[2] = std::byte((len >> 16) & 0xFF); out[3] = std::byte((len >> 24) & 0xFF);
			}
		}
		return true;
	}

	inline std::uint32_t DecodeHeader(const FrameCodec& c, const std::byte* in)
	{
		auto b = [&](int i) { return static_cast<std::uint32_t>(std::to_integer<unsigned char>(in[i])); };
		if (c.headerSize == 2)
		{
			return c.bigEndian ? ((b(0) << 8) | b(1)) : ((b(1) << 8) | b(0));
		}
		return c.bigEndian ? ((b(0) << 24) | (b(1) << 16) | (b(2) << 8) | b(3))
		                   : ((b(3) << 24) | (b(2) << 16) | (b(1) << 8) | b(0));
	}

	// 수신 스트림에서 완결 프레임을 잘라낸다. 부분 헤더/페이로드는 내부 버퍼에 누적.
	class FrameReader
	{
	public:
		explicit FrameReader(const FrameCodec& codec) : _codec(codec) {}

		// data/len을 누적하고, 완결 프레임마다 cb(payloadPtr, payloadLen) 호출.
		// 길이 > maxMessageSize 이면 프로토콜 오류로 false 반환(호출자는 세션 종료).
		bool Feed(const std::byte* data, std::uint32_t len,
		          const std::function<void(const std::byte*, std::uint32_t)>& cb)
		{
			_buf.insert(_buf.end(), data, data + len);

			const std::size_t hs = _codec.headerSize;
			std::size_t pos = 0;
			while (_buf.size() - pos >= hs)
			{
				const std::uint32_t plen = DecodeHeader(_codec, _buf.data() + pos);
				if (plen > _codec.maxMessageSize)
				{
					_buf.clear();
					return false;
				}
				if (_buf.size() - pos < hs + plen)
				{
					break;  // 페이로드 더 필요
				}
				cb(_buf.data() + pos + hs, plen);
				pos += hs + plen;
			}
			if (pos > 0)
			{
				_buf.erase(_buf.begin(), _buf.begin() + static_cast<std::ptrdiff_t>(pos));
			}
			return true;
		}

	private:
		FrameCodec _codec;
		std::vector<std::byte> _buf;
	};
}

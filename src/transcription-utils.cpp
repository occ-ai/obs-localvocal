#include "transcription-utils.h"

#include <sstream>


#define is_lead_byte(c) (((c)&0xe0) == 0xc0 || ((c)&0xf0) == 0xe0 || ((c)&0xf8) == 0xf0)
#define is_trail_byte(c) (((c)&0xc0) == 0x80)

inline int lead_byte_length(const uint8_t c)
{
	if ((c & 0xe0) == 0xc0) {
		return 2;
	} else if ((c & 0xf0) == 0xe0) {
		return 3;
	} else if ((c & 0xf8) == 0xf0) {
		return 4;
	} else {
		return 1;
	}
}

inline bool is_valid_lead_byte(const uint8_t *c)
{
	const int length = lead_byte_length(c[0]);
	if (length == 1) {
		return true;
	}
	if (length == 2 && is_trail_byte(c[1])) {
		return true;
	}
	if (length == 3 && is_trail_byte(c[1]) && is_trail_byte(c[2])) {
		return true;
	}
	if (length == 4 && is_trail_byte(c[1]) && is_trail_byte(c[2]) && is_trail_byte(c[3])) {
		return true;
	}
	return false;
}

std::string fix_utf8(const std::string &str)
{
#ifdef _WIN32
	// Some UTF8 charsets on Windows output have a bug, instead of 0xd? it outputs
	// 0xf?, and 0xc? becomes 0xe?, so we need to fix it.
	std::stringstream ss;
	uint8_t *c_str = (uint8_t *)str.c_str();
	for (size_t i = 0; i < str.size(); ++i) {
		if (is_lead_byte(c_str[i])) {
			// this is a unicode leading byte
			// if the next char is 0xff - it's a bug char, replace it with 0x9f
			if (c_str[i + 1] == 0xff) {
				c_str[i + 1] = 0x9f;
			}
			if (!is_valid_lead_byte(c_str + i)) {
				// This is a bug lead byte, because it's length 3 and the i+2 byte is also
				// a lead byte
				c_str[i] = c_str[i] - 0x20;
			}
		} else {
			if (c_str[i] >= 0xf8) {
				// this may be a malformed lead byte.
				// lets see if it becomes a valid lead byte if we "fix" it
				uint8_t buf_[4];
				buf_[0] = c_str[i] - 0x20;
				buf_[1] = c_str[i + 1];
				buf_[2] = c_str[i + 2];
				buf_[3] = c_str[i + 3];
				if (is_valid_lead_byte(buf_)) {
					// this is a malformed lead byte, fix it
					c_str[i] = c_str[i] - 0x20;
				}
			}
		}
	}

	return std::string((char*)c_str);
#else
	return str;
#endif
}
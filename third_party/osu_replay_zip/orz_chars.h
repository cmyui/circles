#pragma once

// Not part of osu_replay_zip: added for circles. libc++'s floating-point <charconv> needs macOS 13.3 (to_chars) and
// 26.0 (from_chars); these stand in for the calls the library makes, through the C library in the C locale, and give
// the same values and the same digits.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <xlocale.h>

namespace orz {

namespace chars {

	inline double parse(const char* text, char** end, double) { return strtod_l(text, end, nullptr); }
	inline float parse(const char* text, char** end, float) { return strtof_l(text, end, nullptr); }

	// as std::from_chars: the value is left alone unless a number starts the text. The C library also takes leading
	// spaces, a plus sign and hexadecimal, which from_chars does not.
	template<typename T>
	inline const char* from_chars(const char* first, const char* last, T& value) {

		char b[128];

		const size_t n{ (size_t)(last - first) };

		if (n == 0 || n >= sizeof b || *first == '+' || *first == ' ' || (*first >= '\t' && *first <= '\r'))
			return first;

		memcpy(b, first, n);
		b[n] = 0;

		for (size_t i{}; i < n; ++i)
			if (b[i] == 'x' || b[i] == 'X')
				b[i] = 0;

		char* end{};

		const T v{ parse(b, &end, T{}) };

		if (end == b)
			return first;

		value = v;

		return first + (end - b);
	}

	// as std::to_chars(v, scientific, 40): "d.ddd...e-05", every digit exact
	inline char* to_chars_40(char* out, size_t size, double v) {
		return out + snprintf_l(out, size, nullptr, "%.40e", v);
	}

	// as std::to_chars(v, scientific): the fewest digits that read back as v, and of those the nearest to it
	inline char* to_chars_shortest(char* out, size_t size, float v) {

		for (int p{}; p < 9; ++p) {

			int n{ snprintf_l(out, size, nullptr, "%.*e", p, (double)v) };

			if (strtof_l(out, nullptr, nullptr) == v || p == 8)
				return out + n;

			// the nearest decimal of this many digits can lie outside the values that round to v while its neighbour
			// on v's side lies inside (at a power of two, where floats below are twice as dense)
			const double d{ strtod_l(out, nullptr, nullptr) };
			const double step{ std::pow(10., atoi(strchr(out, 'e') + 1) - p) };

			char c[32];

			n = snprintf_l(c, sizeof c, nullptr, "%.*e", p, d < (double)v ? d + step : d - step);

			if (strtof_l(c, nullptr, nullptr) == v) {

				memcpy(out, c, (size_t)n + 1);

				return out + n;
			}

		}

		return out;
	}

}

}

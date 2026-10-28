#pragma once
#include <string>
#include <vector>

namespace sa {

// One keyframe of data/timecyc.dat (column order from CTimeCycle::Initialise; field meaning from gta-reversed TimeCycle.cpp).
struct TimecycKey {
	float ambient[3], ambientObj[3], directional[3], skyTop[3], skyBottom[3];
	float sunCore[3], sunCorona[3];
	float water[4] = {}; // rgb 0..255 + alpha (timecyc water columns)
	float farClip = 0, fogStart = 0;
	bool ok = false;
};

// The file holds NumWeathers blocks of 8 keyframes (hours 0, 5, 6, 7, 12, 19, 20, 22), in weather-id order.
class Timecyc {
public:
	static constexpr int kHours = 8;
	static const int kHourOfKey[kHours];

	bool Load(const std::string &path);
	int NumWeathers() const { return (int)m_keys.size() / kHours; }
	// Linear blend between the two keyframes around `hour` (0..24), wrapping 22:00 -> 00:00.
	TimecycKey Sample(int weather, float hour) const;
	// Weather transition: CWeather::Update (0x72B850) blends old->new weather by the fraction of the hour elapsed.
	static TimecycKey Lerp(const TimecycKey &a, const TimecycKey &b, float t);

private:
	std::vector<TimecycKey> m_keys;
};

} // namespace sa

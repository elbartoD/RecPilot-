#include "macos-ocr.h"

std::string recognize_text_rgba_macos(const std::vector<uint8_t> &, uint32_t, uint32_t)
{
	return {};
}

std::string recognize_text_observations_json_rgba_macos(const std::vector<uint8_t> &, uint32_t, uint32_t)
{
	return "[]";
}

bool save_rgba_jpeg_macos(const std::vector<uint8_t> &, uint32_t, uint32_t, const std::string &)
{
	return false;
}

bool recognize_clip_name_box_rgba_macos(const std::vector<uint8_t> &, uint32_t, uint32_t, double &, double &, double &,
					double &, std::string &)
{
	return false;
}

#pragma once

#include <cstdint>
#include <string>
#include <vector>

std::string recognize_text_rgba_macos(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height);
std::string recognize_text_observations_json_rgba_macos(const std::vector<uint8_t> &rgba, uint32_t width,
							uint32_t height);
bool save_rgba_jpeg_macos(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height,
			  const std::string &path);
bool recognize_clip_name_box_rgba_macos(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height,
					double &x, double &y, double &box_width, double &box_height,
					std::string &text);

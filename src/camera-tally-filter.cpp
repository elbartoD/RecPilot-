#include "camera-tally-filter.h"
#include "macos-ocr.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <graphics/matrix4.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

extern "C" {
const char *config_get_string(config_t *config, const char *section, const char *name);
void config_set_string(config_t *config, const char *section, const char *name, const char *value);
int config_save(config_t *config);
}

namespace {

enum class TallyState {
	Unknown,
	Rolling,
	Idle,
};

struct Rgb {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

struct OcrState {
	std::mutex mutex;
	std::string latest_clip_name;
	std::future<void> worker;
	bool inflight = false;
	bool alive = true;
};

struct MetadataOcrState {
	std::mutex mutex;
	std::future<void> worker;
	std::string pending_json;
	bool inflight = false;
	bool ready = false;
	bool alive = true;
};

struct ClapperboardState {
	std::mutex mutex;
	std::future<void> worker;
	std::string pending_json;
	std::string pending_image_path;
	std::string pending_image_paths_json;
	std::string pending_crop_json;
	bool inflight = false;
	bool ready = false;
	bool alive = true;
};

struct MetadataField {
	std::string name;
	std::string value;
	double x = 0.0;
	double y = 0.0;
	double width = 0.12;
	double height = 0.05;
};

struct FrameSnapshot {
	std::vector<uint8_t> pixels;
	uint32_t width = 0;
	uint32_t height = 0;
};

struct NormalizedRect {
	double x = 0.0;
	double y = 0.0;
	double width = 1.0;
	double height = 1.0;
	bool valid = false;
};

struct CameraTallyFilter {
	obs_source_t *context = nullptr;
	obs_source_t *registry_key = nullptr;
	gs_texrender_t *render = nullptr;
	gs_stagesurf_t *stage = nullptr;
	gs_vertbuffer_t *circle = nullptr;
	gs_vertbuffer_t *crosshair = nullptr;
	gs_vertbuffer_t *arrows = nullptr;
	gs_vertbuffer_t *rectangle = nullptr;
	gs_vertbuffer_t *spinner = nullptr;
	uint32_t stage_width = 0;
	uint32_t stage_height = 0;
	bool armed = false;
	bool show_overlay = true;
	bool start_on_red = true;
	bool ocr_enabled = true;
	bool ocr_o_to_zero = true;
	bool ocr_spaces_to_underscores = true;
	bool ocr_remove_spaces = false;
	bool ocr_add_one = false;
	bool auto_folder_enabled = false;
	bool auto_folder_by_date = true;
	bool auto_folder_by_camera = true;
	bool auto_folder_by_card = true;
	bool metadata_overlay_visible = false;
	bool metadata_csv_per_card = false;
	bool clapperboard_save_snapshots = true;
	std::string metadata_fields_json = "[]";
	std::string clapperboard_target = "clapperboard";
	int color_r = 255;
	int color_g = 0;
	int color_b = 0;
	bool color_pick_pending = false;
	bool auto_detect_center_pending = false;
	bool last_auto_detect_center_pending = false;
	bool auto_detect_clip_name_pending = false;
	bool last_auto_detect_clip_name_pending = false;
	bool clapperboard_pending = false;
	bool clapperboard_border_only = false;
	int clapperboard_capture_tick = 0;
	std::vector<FrameSnapshot> clapperboard_snapshots;
	int auto_detect_band_overlay_frames = 0;
	int auto_detect_activity_frames = 0;
	double color_pick_x = 0.5;
	double color_pick_y = 0.5;
	double center_x = 0.08;
	double center_y = 0.08;
	double radius = 0.025;
	double ocr_x = 0.20;
	double ocr_y = 0.06;
	double ocr_width = 0.14;
	double ocr_height = 0.08;
	double red_threshold = 0.48;
	double red_coverage = 0.02;
	int start_frames = 1;
	int stop_frames = 12;
	int red_frames = 0;
	int non_red_frames = 0;
	int ocr_frame_count = 0;
	int metadata_ocr_frame_count = 0;
	bool picking_center = false;
	TallyState state = TallyState::Unknown;
	std::shared_ptr<OcrState> ocr_state = std::make_shared<OcrState>();
	std::shared_ptr<MetadataOcrState> metadata_ocr_state = std::make_shared<MetadataOcrState>();
	std::shared_ptr<ClapperboardState> clapperboard_state = std::make_shared<ClapperboardState>();
};

struct RecordingStartRequest {
	std::string clip_name;
	bool auto_folder_enabled = false;
	bool auto_folder_by_date = true;
	bool auto_folder_by_camera = true;
	bool auto_folder_by_card = true;
	bool csv_per_card = false;
	std::string metadata_json = "[]";
};

struct RecordingExportRequest {
	std::string clip_name;
	bool csv_per_card = false;
	std::string metadata_json = "[]";
};

static void filter_update(void *data, obs_data_t *settings);
static std::vector<MetadataField> parse_metadata_fields_json(const std::string &json);
static void refresh_metadata_ocr_once(CameraTallyFilter *filter, const uint8_t *data, uint32_t linesize, uint32_t width,
				      uint32_t height);

std::mutex g_filters_mutex;
std::unordered_map<obs_source_t *, CameraTallyFilter *> g_filters;
std::mutex g_pending_rename_mutex;
RecordingExportRequest g_current_recording_export;
std::mutex g_filename_format_mutex;
std::string g_previous_filename_format;
bool g_has_previous_filename_format = false;

static std::filesystem::path path_from_utf8(const std::string &value)
{
#ifdef __cpp_char8_t
	return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(value.data()), value.size()));
#else
	return std::filesystem::u8path(value);
#endif
}

static std::filesystem::path path_from_utf8(const char *value)
{
	return value && *value ? path_from_utf8(std::string(value)) : std::filesystem::path();
}

static std::string path_to_utf8(const std::filesystem::path &path)
{
#ifdef __cpp_char8_t
	const auto value = path.u8string();
	return std::string(reinterpret_cast<const char *>(value.data()), value.size());
#else
	return path.u8string();
#endif
}

static std::tm local_time_from_time(std::time_t value)
{
	std::tm out{};
#ifdef _WIN32
	localtime_s(&out, &value);
#else
	localtime_r(&value, &out);
#endif
	return out;
}

static std::tm utc_time_from_time(std::time_t value)
{
	std::tm out{};
#ifdef _WIN32
	gmtime_s(&out, &value);
#else
	gmtime_r(&value, &out);
#endif
	return out;
}

static gs_vertbuffer_t *create_circle_vertex_buffer()
{
	constexpr int segments = 96;

	gs_render_start(true);
	for (int i = 0; i <= segments; ++i) {
		const float angle = static_cast<float>(i) * 2.0f * 3.1415926535f / static_cast<float>(segments);
		gs_vertex2f(cosf(angle), sinf(angle));
	}

	return gs_render_save();
}

static gs_vertbuffer_t *create_crosshair_vertex_buffer()
{
	gs_render_start(true);
	gs_vertex2f(-1.25f, 0.0f);
	gs_vertex2f(-0.45f, 0.0f);
	gs_vertex2f(0.45f, 0.0f);
	gs_vertex2f(1.25f, 0.0f);
	gs_vertex2f(0.0f, -1.25f);
	gs_vertex2f(0.0f, -0.45f);
	gs_vertex2f(0.0f, 0.45f);
	gs_vertex2f(0.0f, 1.25f);

	return gs_render_save();
}

static gs_vertbuffer_t *create_arrow_vertex_buffer()
{
	gs_render_start(true);

	gs_vertex2f(0.0f, -1.75f);
	gs_vertex2f(-0.16f, -1.45f);
	gs_vertex2f(0.0f, -1.75f);
	gs_vertex2f(0.16f, -1.45f);

	gs_vertex2f(0.0f, 1.75f);
	gs_vertex2f(-0.16f, 1.45f);
	gs_vertex2f(0.0f, 1.75f);
	gs_vertex2f(0.16f, 1.45f);

	gs_vertex2f(-1.75f, 0.0f);
	gs_vertex2f(-1.45f, -0.16f);
	gs_vertex2f(-1.75f, 0.0f);
	gs_vertex2f(-1.45f, 0.16f);

	gs_vertex2f(1.75f, 0.0f);
	gs_vertex2f(1.45f, -0.16f);
	gs_vertex2f(1.75f, 0.0f);
	gs_vertex2f(1.45f, 0.16f);

	return gs_render_save();
}

static gs_vertbuffer_t *create_rectangle_vertex_buffer()
{
	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(1.0f, 1.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(0.0f, 0.0f);

	return gs_render_save();
}

static gs_vertbuffer_t *create_spinner_vertex_buffer()
{
	constexpr int segments = 64;
	constexpr float arc = 1.55f * 3.1415926535f;

	gs_render_start(true);
	for (int i = 0; i <= segments; ++i) {
		const float angle = static_cast<float>(i) * arc / static_cast<float>(segments);
		gs_vertex2f(std::cos(angle), std::sin(angle));
	}

	return gs_render_save();
}

static const char *filter_name(void *)
{
	return obs_module_text("CameraTallyFilter.Name");
}

static std::string trim(std::string value)
{
	auto is_space = [](unsigned char ch) {
		return std::isspace(ch) != 0;
	};
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) { return !is_space(ch); }));
	value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) { return !is_space(ch); }).base(),
		    value.end());
	return value;
}

static std::string compact_spaces(std::string value)
{
	std::string out;
	bool previous_space = false;
	for (unsigned char ch : value) {
		const bool space = std::isspace(ch) != 0;
		if (space) {
			if (!previous_space)
				out.push_back(' ');
			previous_space = true;
			continue;
		}
		out.push_back(static_cast<char>(ch));
		previous_space = false;
	}
	return trim(out);
}

static std::string lowercase_ascii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return value;
}

static std::string uppercase_ascii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
	return value;
}

static std::string metadata_slug(std::string value)
{
	value = lowercase_ascii(compact_spaces(value));
	std::string out;
	bool previous_separator = false;
	for (unsigned char ch : value) {
		if (std::isalnum(ch)) {
			out.push_back(static_cast<char>(ch));
			previous_separator = false;
		} else if (!previous_separator && !out.empty()) {
			out.push_back('_');
			previous_separator = true;
		}
	}
	while (!out.empty() && out.back() == '_')
		out.pop_back();
	return out.empty() ? "field" : out;
}

static void add_one_to_clip_counter(std::string &value)
{
	size_t clip_start = std::string::npos;
	size_t clip_size = 0;

	for (size_t i = 0; i < value.size(); ++i) {
		if (!std::isdigit(static_cast<unsigned char>(value[i])))
			continue;

		const size_t start = i;
		while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i])))
			i++;

		clip_start = start;
		clip_size = i - start;
	}

	if (clip_start == std::string::npos || clip_size == 0)
		return;

	const std::string digits = value.substr(clip_start, clip_size);
	try {
		const unsigned long number = std::stoul(digits);
		std::string incremented = std::to_string(number + 1);
		if (incremented.size() < digits.size())
			incremented.insert(incremented.begin(), digits.size() - incremented.size(), '0');
		value.replace(clip_start, digits.size(), incremented);
	} catch (...) {
	}
}

static std::string normalize_clip_name(std::string value, bool o_to_zero, bool spaces_to_underscores,
				       bool remove_spaces, bool add_one)
{
	value = compact_spaces(value);
	std::string lowered = value;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
		       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

	const std::string prefix = "clip name";
	const size_t pos = lowered.find(prefix);
	if (pos != std::string::npos) {
		value = trim(value.substr(pos + prefix.size()));
		if (!value.empty() && (value[0] == ':' || value[0] == '-' || value[0] == '|'))
			value = trim(value.substr(1));
	}

	if (o_to_zero) {
		std::replace(value.begin(), value.end(), 'O', '0');
		std::replace(value.begin(), value.end(), 'o', '0');
	}

	std::string safe;
	for (unsigned char ch : value) {
		if (ch < 32)
			continue;
		switch (ch) {
		case '<':
		case '>':
		case ':':
		case '"':
		case '/':
		case '\\':
		case '|':
		case '?':
		case '*':
		case '%':
			safe.push_back('-');
			break;
		default:
			safe.push_back(static_cast<char>(ch));
			break;
		}
	}

	safe = compact_spaces(safe);
	if (remove_spaces) {
		safe.erase(std::remove_if(safe.begin(), safe.end(),
					  [](unsigned char ch) { return std::isspace(ch) != 0; }),
			   safe.end());
	} else if (spaces_to_underscores) {
		std::replace(safe.begin(), safe.end(), ' ', '_');
	}

	while (safe.find("--") != std::string::npos)
		safe.replace(safe.find("--"), 2, "-");
	if (safe.size() > 120)
		safe.resize(120);
	if (add_one)
		add_one_to_clip_counter(safe);
	return trim(safe);
}

static void apply_recording_filename(const std::string &clip_name)
{
	if (clip_name.empty())
		return;

	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;

	config_set_string(config, "Output", "FilenameFormatting", clip_name.c_str());
	config_save(config);
	obs_log(LOG_INFO, "camera tally OCR clip name applied to OBS filename: %s", clip_name.c_str());
}

static void remember_current_filename_format()
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;

	std::lock_guard<std::mutex> lock(g_filename_format_mutex);
	if (g_has_previous_filename_format)
		return;

	const char *format = config_get_string(config, "Output", "FilenameFormatting");
	g_previous_filename_format = format ? format : "";
	g_has_previous_filename_format = true;
}

static void restore_previous_filename_format()
{
	std::string format;
	{
		std::lock_guard<std::mutex> lock(g_filename_format_mutex);
		if (!g_has_previous_filename_format)
			return;
		format = std::move(g_previous_filename_format);
		g_previous_filename_format.clear();
		g_has_previous_filename_format = false;
	}

	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;

	config_set_string(config, "Output", "FilenameFormatting", format.c_str());
	config_save(config);
	obs_log(LOG_INFO, "RecPilot restored previous OBS filename format");
}

static void ensure_recording_folder_exists()
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;

	const char *mode = config_get_string(config, "Output", "Mode");
	const bool advanced = mode && strcmp(mode, "Advanced") == 0;
	const char *path = advanced ? config_get_string(config, "AdvOut", "RecFilePath")
				    : config_get_string(config, "SimpleOutput", "FilePath");
	if (!path || !*path)
		return;

	try {
		std::filesystem::path folder = path_from_utf8(path);
		if (std::filesystem::exists(folder)) {
			if (!std::filesystem::is_directory(folder))
				obs_log(LOG_WARNING, "RecPilot recording path is not a folder: %s", path);
			return;
		}

		std::filesystem::create_directories(folder);
		obs_log(LOG_INFO, "RecPilot created missing recording folder: %s", path);
	} catch (const std::exception &ex) {
		obs_log(LOG_WARNING, "RecPilot could not create recording folder '%s': %s", path, ex.what());
	}
}

static std::filesystem::path current_recording_root_folder(config_t *config)
{
	if (!config)
		return {};

	const char *root_path = config_get_string(config, "RecPilot", "RootFolder");
	if (root_path && *root_path)
		return path_from_utf8(root_path);

	const char *mode = config_get_string(config, "Output", "Mode");
	const bool advanced = mode && strcmp(mode, "Advanced") == 0;
	const char *path = advanced ? config_get_string(config, "AdvOut", "RecFilePath")
				    : config_get_string(config, "SimpleOutput", "FilePath");
	return path && *path ? path_from_utf8(path) : std::filesystem::path();
}

static void set_recording_folder(config_t *config, const std::filesystem::path &folder)
{
	if (!config || folder.empty())
		return;

	const std::string folder_string = path_to_utf8(folder);
	config_set_string(config, "SimpleOutput", "FilePath", folder_string.c_str());
	config_set_string(config, "AdvOut", "RecFilePath", folder_string.c_str());
	config_set_string(config, "AdvOut", "FFFilePath", folder_string.c_str());
	config_save(config);
}

static std::string recording_date_folder_name()
{
	const std::time_t now = std::time(nullptr);
	std::tm local_time = local_time_from_time(now);

	char buffer[9] = {};
	std::strftime(buffer, sizeof(buffer), "%Y%m%d", &local_time);
	return buffer;
}

static std::string recording_camera_folder_name(const std::string &clip_name)
{
	for (unsigned char ch : clip_name) {
		if (std::isalpha(ch))
			return std::string("CAM_") + static_cast<char>(std::toupper(ch));
	}

	return {};
}

static std::string recording_card_folder_name(const std::string &clip_name)
{
	std::string normalized;
	for (unsigned char ch : clip_name) {
		if (std::isalnum(ch))
			normalized.push_back(static_cast<char>(std::toupper(ch)));
		else if (!normalized.empty())
			break;
	}

	if (normalized.size() < 3 || !std::isalpha(static_cast<unsigned char>(normalized[0])))
		return {};

	size_t end = 1;
	while (end < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[end])) && end < 6)
		end++;

	const size_t digit_count = end > 1 ? end - 1 : 0;
	if (digit_count < 2)
		return {};

	return normalized.substr(0, end);
}

static std::string recording_clip_base_name(const std::filesystem::path &file, const std::string &clip_name)
{
	if (!clip_name.empty())
		return clip_name;
	if (!file.empty())
		return path_to_utf8(file.stem());
	return {};
}

static std::string recording_camera_letter(const std::string &clip_name)
{
	for (unsigned char ch : clip_name) {
		if (std::isalpha(ch))
			return std::string(1, static_cast<char>(std::toupper(ch)));
	}
	return {};
}

static std::string recording_zoelog_clip_number(const std::string &clip_name)
{
	std::string normalized;
	for (unsigned char ch : clip_name) {
		if (std::isalnum(ch))
			normalized.push_back(static_cast<char>(std::toupper(ch)));
	}

	const size_t marker = normalized.rfind('C');
	if (marker == std::string::npos || marker + 1 >= normalized.size())
		return normalized;

	std::string digits = normalized.substr(marker + 1);
	if (!std::all_of(digits.begin(), digits.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
		return normalized;

	const size_t first = digits.find_first_not_of('0');
	if (first == std::string::npos)
		return "0";
	return digits.substr(first);
}

static std::string csv_escape(const std::string &value)
{
	bool quote = value.find_first_of("\",\r\n") != std::string::npos;
	if (!quote)
		return value;

	std::string out = "\"";
	for (char ch : value) {
		if (ch == '"')
			out += "\"\"";
		else
			out.push_back(ch);
	}
	out += "\"";
	return out;
}

static std::string delimited_escape(const std::string &value, char delimiter)
{
	const bool quote = value.find(delimiter) != std::string::npos ||
			   value.find_first_of("\"\r\n") != std::string::npos;
	if (!quote)
		return value;

	std::string out = "\"";
	for (char ch : value) {
		if (ch == '"')
			out += "\"\"";
		else
			out.push_back(ch);
	}
	out += "\"";
	return out;
}

static std::vector<std::string> parse_delimited_line(const std::string &line, char delimiter)
{
	std::vector<std::string> cells;
	std::string cell;
	bool quoted = false;
	for (size_t i = 0; i < line.size(); ++i) {
		const char ch = line[i];
		if (quoted) {
			if (ch == '"' && i + 1 < line.size() && line[i + 1] == '"') {
				cell.push_back('"');
				i++;
			} else if (ch == '"') {
				quoted = false;
			} else {
				cell.push_back(ch);
			}
		} else if (ch == delimiter) {
			cells.push_back(cell);
			cell.clear();
		} else if (ch == '"') {
			quoted = true;
		} else {
			cell.push_back(ch);
		}
	}
	cells.push_back(cell);
	return cells;
}

static std::vector<std::string> parse_csv_line(const std::string &line)
{
	return parse_delimited_line(line, ',');
}

static std::string sanitize_path_component(std::string value)
{
	value = compact_spaces(value);
	if (value.empty())
		value = "RecPilot";
	for (char &ch : value) {
		const unsigned char c = static_cast<unsigned char>(ch);
		if (c < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' ||
		    ch == '|' || ch == '?' || ch == '*' || ch == '%')
			ch = '_';
	}
	if (value.size() > 120)
		value.resize(120);
	return value;
}

static std::vector<MetadataField> metadata_fields_with_values(const std::string &json)
{
	std::vector<MetadataField> out;
	for (MetadataField field : parse_metadata_fields_json(json)) {
		field.name = compact_spaces(field.name);
		field.value = compact_spaces(field.value);
		if (!field.name.empty())
			out.push_back(std::move(field));
	}
	return out;
}

static std::string local_iso8601_now()
{
	const std::time_t now = std::time(nullptr);
	std::tm local_time = local_time_from_time(now);

	char datetime[32] = {};
	std::strftime(datetime, sizeof(datetime), "%Y-%m-%dT%H:%M:%S", &local_time);

#if defined(__APPLE__) || defined(__FreeBSD__)
	const long offset = local_time.tm_gmtoff;
#else
	std::tm utc_time = utc_time_from_time(now);
	const long offset = static_cast<long>(std::difftime(std::mktime(&local_time), std::mktime(&utc_time)));
#endif
	const char sign = offset >= 0 ? '+' : '-';
	const long abs_offset = std::labs(offset);
	char tz[8] = {};
	snprintf(tz, sizeof(tz), "%c%02ld:%02ld", sign, abs_offset / 3600, (abs_offset % 3600) / 60);
	return std::string(datetime) + ".000" + tz;
}

static std::string local_date_now()
{
	const std::time_t now = std::time(nullptr);
	std::tm local_time = local_time_from_time(now);
	char date[16] = {};
	std::strftime(date, sizeof(date), "%Y-%m-%d", &local_time);
	return date;
}

static const std::vector<std::string> &zoelog_csv_headers()
{
	static const std::vector<std::string> headers = {
		"Slate",     "Scene",      "Date",         "Camera",      "Roll",       "Take",        "Clip",
		"Circled",   "Lens",       "Filters",      "Stop",        "Focus",      "Lens Height", "FPS",
		"Shutter",   "Film Stock", "Tilt",         "Description", "Notes",      "Color Temp",  "ISO",
		"Time Code", "Lut",        "Aspect Ratio", "Format",      "Resolution", "Origin Date", "Take Origin",
	};
	return headers;
}

static bool set_zoelog_value(std::map<std::string, std::string> &row, const std::string &column,
			     const std::string &value, bool append = false)
{
	const std::string clean = compact_spaces(value);
	if (column.empty() || clean.empty())
		return false;

	std::string &target = row[column];
	if (append && !target.empty() && target.find(clean) == std::string::npos)
		target += "; " + clean;
	else if (target.empty() || !append)
		target = clean;
	return true;
}

static std::string zoelog_column_for_metadata_field(const std::string &field_name)
{
	const std::string slug = metadata_slug(field_name);
	if (slug == "slate")
		return "Slate";
	if (slug == "scene" || slug == "scenes")
		return "Scene";
	if (slug == "date" || slug == "recording_date" || slug == "shooting_date" || slug == "day_date")
		return "Date";
	if (slug == "camera" || slug == "camera_letter" || slug == "camera_name" || slug == "camera_id" ||
	    slug == "camera_index")
		return "Camera";
	if (slug == "roll" || slug == "reel" || slug == "reel_name" || slug == "card")
		return "Roll";
	if (slug == "take")
		return "Take";
	if (slug == "clip" || slug == "clip_name" || slug == "clipname" || slug == "name")
		return "Clip";
	if (slug == "circled" || slug == "circled_take")
		return "Circled";
	if (slug == "lens" || slug == "lens_model" || slug == "focal_length" || slug == "lens_serial")
		return "Lens";
	if (slug == "filter" || slug == "filters" || slug == "nd" || slug == "nd_filter" || slug == "lens_filter")
		return "Filters";
	if (slug == "stop" || slug == "t_stop" || slug == "tstop" || slug == "aperture" || slug == "iris")
		return "Stop";
	if (slug == "focus" || slug == "focus_distance" || slug == "distance_to_object")
		return "Focus";
	if (slug == "lens_height" || slug == "camera_height")
		return "Lens Height";
	if (slug == "fps" || slug == "sensor_fps")
		return "FPS";
	if (slug == "shutter" || slug == "shutter_angle" || slug == "shutter_speed")
		return "Shutter";
	if (slug == "film_stock" || slug == "stock")
		return "Film Stock";
	if (slug == "tilt")
		return "Tilt";
	if (slug == "description")
		return "Description";
	if (slug == "notes" || slug == "shot_notes" || slug == "comment")
		return "Notes";
	if (slug == "white_balance" || slug == "wb" || slug == "wb_clip" || slug == "wb_current" ||
	    slug == "color_temperature" || slug == "color_temp")
		return "Color Temp";
	if (slug == "iso" || slug == "ei_iso_clip" || slug == "ei_iso_current" || slug == "ei" || slug == "native_iso")
		return "ISO";
	if (slug == "time_code" || slug == "timecode" || slug == "tc")
		return "Time Code";
	if (slug == "lut" || slug == "look")
		return "Lut";
	if (slug == "aspect_ratio")
		return "Aspect Ratio";
	if (slug == "format" || slug == "codec")
		return "Format";
	if (slug == "resolution")
		return "Resolution";
	if (slug == "origin_date" || slug == "created_at" || slug == "created")
		return "Origin Date";
	if (slug == "take_origin")
		return "Take Origin";
	return {};
}

static void update_zoelog_csv(const std::filesystem::path &file, const std::string &clip_name,
			      const std::vector<MetadataField> &fields)
{
	if (file.empty() || fields.empty())
		return;

	const std::string clip_base = recording_clip_base_name(file, clip_name);
	if (clip_base.empty())
		return;

	const std::string card = recording_card_folder_name(clip_base);
	const std::filesystem::path csv_path =
		file.parent_path() / path_from_utf8((card.empty() ? "metadata" : card) + std::string(".csv"));

	const std::vector<std::string> &headers = zoelog_csv_headers();
	std::vector<std::map<std::string, std::string>> rows;

	try {
		if (std::filesystem::exists(csv_path)) {
			std::ifstream in(csv_path);
			std::string line;
			std::vector<std::string> existing_headers;
			if (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				existing_headers = parse_csv_line(line);
			}
			if (!existing_headers.empty()) {
				while (std::getline(in, line)) {
					if (!line.empty() && line.back() == '\r')
						line.pop_back();
					const std::vector<std::string> cells = parse_csv_line(line);
					std::map<std::string, std::string> row;
					for (size_t i = 0; i < cells.size() && i < existing_headers.size(); ++i)
						row[existing_headers[i]] = cells[i];
					if (!row["Clip"].empty())
						rows.push_back(std::move(row));
				}
			}
		}

		std::map<std::string, std::string> current;
		current["Date"] = local_date_now();
		current["Camera"] = recording_camera_letter(clip_base);
		current["Roll"] = card;
		current["Clip"] = recording_zoelog_clip_number(clip_base);
		current["Circled"] = "false";
		current["Origin Date"] = local_iso8601_now();
		current["Take Origin"] = current["Origin Date"];
		for (const MetadataField &field : fields) {
			if (field.name.empty() || field.value.empty())
				continue;
			const std::string column = zoelog_column_for_metadata_field(field.name);
			if (!column.empty())
				set_zoelog_value(current, column, field.value,
						 column == "Notes" || column == "Description");
		}

		bool replaced = false;
		for (auto &row : rows) {
			if (row["Roll"] == current["Roll"] && row["Camera"] == current["Camera"] &&
			    row["Clip"] == current["Clip"]) {
				row = current;
				replaced = true;
				break;
			}
		}
		if (!replaced)
			rows.push_back(current);

		std::filesystem::create_directories(csv_path.parent_path());
		std::ofstream out(csv_path, std::ios::trunc);
		for (size_t i = 0; i < headers.size(); ++i) {
			if (i)
				out << ',';
			out << csv_escape(headers[i]);
		}
		out << '\n';
		for (const auto &row : rows) {
			for (size_t i = 0; i < headers.size(); ++i) {
				if (i)
					out << ',';
				auto it = row.find(headers[i]);
				out << csv_escape(it == row.end() ? std::string() : it->second);
			}
			out << '\n';
		}
		obs_log(LOG_INFO, "RecPilot updated ZoeLog CSV: %s", path_to_utf8(csv_path).c_str());
	} catch (const std::exception &ex) {
		obs_log(LOG_WARNING, "RecPilot could not update ZoeLog CSV '%s': %s", path_to_utf8(csv_path).c_str(),
			ex.what());
	}
}

static void write_recording_exports(const std::filesystem::path &file, const RecordingExportRequest &request)
{
	if (file.empty() || !request.csv_per_card)
		return;

	const std::vector<MetadataField> fields = metadata_fields_with_values(request.metadata_json);
	if (fields.empty()) {
		obs_log(LOG_INFO, "RecPilot ZoeLog export skipped: no filled metadata fields");
		return;
	}

	update_zoelog_csv(file, request.clip_name, fields);
}

static void apply_automatic_recording_folder(const RecordingStartRequest &request)
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;

	std::filesystem::path folder = current_recording_root_folder(config);
	if (folder.empty())
		return;

	const char *root_path = config_get_string(config, "RecPilot", "RootFolder");
	if (!root_path || !*root_path) {
		const std::string folder_string = path_to_utf8(folder);
		config_set_string(config, "RecPilot", "RootFolder", folder_string.c_str());
	}

	if (request.auto_folder_enabled) {
		if (request.auto_folder_by_date)
			folder /= path_from_utf8(recording_date_folder_name());
		if (request.auto_folder_by_camera) {
			const std::string camera_folder = recording_camera_folder_name(request.clip_name);
			if (!camera_folder.empty())
				folder /= path_from_utf8(camera_folder);
		}
		if (request.auto_folder_by_card) {
			const std::string card_folder = recording_card_folder_name(request.clip_name);
			if (!card_folder.empty())
				folder /= path_from_utf8(card_folder);
		}
	}

	set_recording_folder(config, folder);
}

static std::filesystem::path rename_recording_file_at_path(const std::filesystem::path &old_path,
							   const std::string &clip_name)
{
	if (clip_name.empty() || old_path.empty())
		return old_path;
	if (!std::filesystem::exists(old_path)) {
		obs_log(LOG_WARNING, "RecPilot could not rename recording because it does not exist yet: %s",
			path_to_utf8(old_path).c_str());
		return old_path;
	}

	try {
		const std::filesystem::path folder = old_path.parent_path();
		const std::filesystem::path ext = old_path.extension();
		std::filesystem::path new_path = folder / path_from_utf8(clip_name + path_to_utf8(ext));
		if (new_path == old_path)
			return old_path;

		int duplicate = 2;
		while (std::filesystem::exists(new_path)) {
			new_path = folder /
				   path_from_utf8(clip_name + "_" + std::to_string(duplicate) + path_to_utf8(ext));
			duplicate++;
		}

		std::filesystem::rename(old_path, new_path);
		obs_log(LOG_INFO, "RecPilot renamed recording to: %s", path_to_utf8(new_path).c_str());
		return new_path;
	} catch (const std::exception &ex) {
		obs_log(LOG_WARNING, "RecPilot could not rename current recording: %s", ex.what());
	}
	return old_path;
}

static std::filesystem::path last_finished_recording_path()
{
	char *path = obs_frontend_get_last_recording();
	if (!path || !*path) {
		if (path)
			bfree(path);
		obs_log(LOG_WARNING, "RecPilot could not find the finished recording path to rename");
		return {};
	}

	std::filesystem::path result = path_from_utf8(path);
	bfree(path);
	return result;
}

static std::filesystem::path rename_last_finished_recording(const std::string &clip_name)
{
	std::filesystem::path path = last_finished_recording_path();
	if (path.empty() || clip_name.empty())
		return path;
	return rename_recording_file_at_path(path, clip_name);
}

static std::string latest_clip_name(CameraTallyFilter *filter)
{
	if (!filter || !filter->ocr_state)
		return {};

	std::lock_guard<std::mutex> lock(filter->ocr_state->mutex);
	return filter->ocr_state->latest_clip_name;
}

static RecordingExportRequest export_request_from_filter(CameraTallyFilter *filter, const std::string &clip_name)
{
	RecordingExportRequest request;
	request.clip_name = clip_name;
	if (!filter)
		return request;

	request.csv_per_card = filter->metadata_csv_per_card;
	request.metadata_json = filter->metadata_fields_json.empty() ? "[]" : filter->metadata_fields_json;
	return request;
}

static void remember_recording_export(const RecordingStartRequest &request)
{
	std::lock_guard<std::mutex> lock(g_pending_rename_mutex);
	g_current_recording_export.clip_name = request.clip_name;
	g_current_recording_export.csv_per_card = request.csv_per_card;
	g_current_recording_export.metadata_json = request.metadata_json.empty() ? "[]" : request.metadata_json;
}

static void start_recording_task(void *param)
{
	std::unique_ptr<RecordingStartRequest> request(static_cast<RecordingStartRequest *>(param));
	if (request) {
		if (!request->clip_name.empty())
			remember_current_filename_format();
		apply_recording_filename(request->clip_name);
		apply_automatic_recording_folder(*request);
	}
	if (!obs_frontend_recording_active()) {
		ensure_recording_folder_exists();
		if (request)
			remember_recording_export(*request);
		obs_frontend_recording_start();
	}
}

static void stop_recording_task(void *)
{
	if (obs_frontend_recording_active())
		obs_frontend_recording_stop();
}

static void request_recording_start(CameraTallyFilter *filter, const std::string &clip_name = {})
{
	auto *request = new RecordingStartRequest();
	request->clip_name = clip_name;
	if (filter) {
		request->auto_folder_enabled = filter->auto_folder_enabled;
		request->auto_folder_by_date = filter->auto_folder_by_date;
		request->auto_folder_by_camera = filter->auto_folder_by_camera;
		request->auto_folder_by_card = filter->auto_folder_by_card;
		const RecordingExportRequest export_request = export_request_from_filter(filter, clip_name);
		request->csv_per_card = export_request.csv_per_card;
		request->metadata_json = export_request.metadata_json;
	}
	obs_queue_task(OBS_TASK_UI, start_recording_task, request, false);
}

static void request_recording_stop()
{
	obs_queue_task(OBS_TASK_UI, stop_recording_task, nullptr, false);
}

static void recording_stopped_event_task(void *)
{
	RecordingExportRequest export_request;
	{
		std::lock_guard<std::mutex> lock(g_pending_rename_mutex);
		export_request = std::move(g_current_recording_export);
		g_current_recording_export = {};
	}
	std::filesystem::path final_path = rename_last_finished_recording(export_request.clip_name);
	if (final_path.empty())
		final_path = last_finished_recording_path();
	write_recording_exports(final_path, export_request);
	restore_previous_filename_format();
}

static void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED)
		obs_queue_task(OBS_TASK_UI, recording_stopped_event_task, nullptr, false);
}

static int clamp_int(int value, int min_value, int max_value)
{
	return std::max(min_value, std::min(value, max_value));
}

static uint8_t clamp_byte(int value)
{
	return static_cast<uint8_t>(clamp_int(value, 0, 255));
}

static Rgb yuv_to_rgb(int y, int u, int v)
{
	const int c = y - 16;
	const int d = u - 128;
	const int e = v - 128;

	return {
		clamp_byte((298 * c + 409 * e + 128) >> 8),
		clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8),
		clamp_byte((298 * c + 516 * d + 128) >> 8),
	};
}

static uint16_t read_u16(const uint8_t *p)
{
	uint16_t value = 0;
	std::memcpy(&value, p, sizeof(value));
	return value;
}

static uint8_t yuv_wide_to_8bit(const uint8_t *p)
{
	const int value = static_cast<int>(read_u16(p));
	if (value <= 1023)
		return clamp_byte((value + 2) >> 2);
	if (value <= 4095)
		return clamp_byte((value + 8) >> 4);
	return clamp_byte((value + 128) >> 8);
}

static const char *video_format_name(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_NONE:
		return "NONE";
	case VIDEO_FORMAT_I420:
		return "I420";
	case VIDEO_FORMAT_NV12:
		return "NV12";
	case VIDEO_FORMAT_YVYU:
		return "YVYU";
	case VIDEO_FORMAT_YUY2:
		return "YUY2";
	case VIDEO_FORMAT_UYVY:
		return "UYVY";
	case VIDEO_FORMAT_RGBA:
		return "RGBA";
	case VIDEO_FORMAT_BGRA:
		return "BGRA";
	case VIDEO_FORMAT_BGRX:
		return "BGRX";
	case VIDEO_FORMAT_Y800:
		return "Y800";
	case VIDEO_FORMAT_I444:
		return "I444";
	case VIDEO_FORMAT_BGR3:
		return "BGR3";
	case VIDEO_FORMAT_I422:
		return "I422";
	case VIDEO_FORMAT_I40A:
		return "I40A";
	case VIDEO_FORMAT_I42A:
		return "I42A";
	case VIDEO_FORMAT_YUVA:
		return "YUVA";
	case VIDEO_FORMAT_AYUV:
		return "AYUV";
	case VIDEO_FORMAT_I010:
		return "I010";
	case VIDEO_FORMAT_P010:
		return "P010";
	case VIDEO_FORMAT_I210:
		return "I210";
	case VIDEO_FORMAT_I412:
		return "I412";
	case VIDEO_FORMAT_YA2L:
		return "YA2L";
	case VIDEO_FORMAT_P216:
		return "P216";
	case VIDEO_FORMAT_P416:
		return "P416";
	case VIDEO_FORMAT_V210:
		return "V210";
	case VIDEO_FORMAT_R10L:
		return "R10L";
	default:
		return "unknown";
	}
}

static bool read_pixel(const obs_source_frame *frame, uint32_t x, uint32_t y, Rgb &rgb)
{
	if (!frame || !frame->data[0] || x >= frame->width || y >= frame->height)
		return false;

	switch (frame->format) {
	case VIDEO_FORMAT_RGBA: {
		const uint8_t *p = frame->data[0] + y * frame->linesize[0] + x * 4;
		rgb = {p[0], p[1], p[2]};
		return true;
	}
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX: {
		const uint8_t *p = frame->data[0] + y * frame->linesize[0] + x * 4;
		rgb = {p[2], p[1], p[0]};
		return true;
	}
	case VIDEO_FORMAT_BGR3: {
		const uint8_t *p = frame->data[0] + y * frame->linesize[0] + x * 3;
		rgb = {p[2], p[1], p[0]};
		return true;
	}
	case VIDEO_FORMAT_AYUV: {
		const uint8_t *p = frame->data[0] + y * frame->linesize[0] + x * 4;
		rgb = yuv_to_rgb(p[1], p[2], p[3]);
		return true;
	}
	case VIDEO_FORMAT_YUY2: {
		const uint8_t *p = frame->data[0] + y * frame->linesize[0] + (x / 2) * 4;
		const int y_value = (x % 2 == 0) ? p[0] : p[2];
		rgb = yuv_to_rgb(y_value, p[1], p[3]);
		return true;
	}
	case VIDEO_FORMAT_UYVY: {
		const uint8_t *p = frame->data[0] + y * frame->linesize[0] + (x / 2) * 4;
		const int y_value = (x % 2 == 0) ? p[1] : p[3];
		rgb = yuv_to_rgb(y_value, p[0], p[2]);
		return true;
	}
	case VIDEO_FORMAT_YVYU: {
		const uint8_t *p = frame->data[0] + y * frame->linesize[0] + (x / 2) * 4;
		const int y_value = (x % 2 == 0) ? p[0] : p[2];
		rgb = yuv_to_rgb(y_value, p[3], p[1]);
		return true;
	}
	case VIDEO_FORMAT_NV12: {
		if (!frame->data[1])
			return false;
		const uint8_t y_value = frame->data[0][y * frame->linesize[0] + x];
		const uint8_t *uv = frame->data[1] + (y / 2) * frame->linesize[1] + (x / 2) * 2;
		rgb = yuv_to_rgb(y_value, uv[0], uv[1]);
		return true;
	}
	case VIDEO_FORMAT_I420: {
		if (!frame->data[1] || !frame->data[2])
			return false;
		const uint8_t y_value = frame->data[0][y * frame->linesize[0] + x];
		const uint8_t u_value = frame->data[1][(y / 2) * frame->linesize[1] + (x / 2)];
		const uint8_t v_value = frame->data[2][(y / 2) * frame->linesize[2] + (x / 2)];
		rgb = yuv_to_rgb(y_value, u_value, v_value);
		return true;
	}
	case VIDEO_FORMAT_I40A: {
		if (!frame->data[1] || !frame->data[2])
			return false;
		const uint8_t y_value = frame->data[0][y * frame->linesize[0] + x];
		const uint8_t u_value = frame->data[1][(y / 2) * frame->linesize[1] + (x / 2)];
		const uint8_t v_value = frame->data[2][(y / 2) * frame->linesize[2] + (x / 2)];
		rgb = yuv_to_rgb(y_value, u_value, v_value);
		return true;
	}
	case VIDEO_FORMAT_I422: {
		if (!frame->data[1] || !frame->data[2])
			return false;
		const uint8_t y_value = frame->data[0][y * frame->linesize[0] + x];
		const uint8_t u_value = frame->data[1][y * frame->linesize[1] + (x / 2)];
		const uint8_t v_value = frame->data[2][y * frame->linesize[2] + (x / 2)];
		rgb = yuv_to_rgb(y_value, u_value, v_value);
		return true;
	}
	case VIDEO_FORMAT_I42A: {
		if (!frame->data[1] || !frame->data[2])
			return false;
		const uint8_t y_value = frame->data[0][y * frame->linesize[0] + x];
		const uint8_t u_value = frame->data[1][y * frame->linesize[1] + (x / 2)];
		const uint8_t v_value = frame->data[2][y * frame->linesize[2] + (x / 2)];
		rgb = yuv_to_rgb(y_value, u_value, v_value);
		return true;
	}
	case VIDEO_FORMAT_I444: {
		if (!frame->data[1] || !frame->data[2])
			return false;
		const uint8_t y_value = frame->data[0][y * frame->linesize[0] + x];
		const uint8_t u_value = frame->data[1][y * frame->linesize[1] + x];
		const uint8_t v_value = frame->data[2][y * frame->linesize[2] + x];
		rgb = yuv_to_rgb(y_value, u_value, v_value);
		return true;
	}
	case VIDEO_FORMAT_YUVA: {
		if (!frame->data[1] || !frame->data[2])
			return false;
		const uint8_t y_value = frame->data[0][y * frame->linesize[0] + x];
		const uint8_t u_value = frame->data[1][y * frame->linesize[1] + x];
		const uint8_t v_value = frame->data[2][y * frame->linesize[2] + x];
		rgb = yuv_to_rgb(y_value, u_value, v_value);
		return true;
	}
	case VIDEO_FORMAT_P010: {
		if (!frame->data[1])
			return false;
		const uint8_t y_value = yuv_wide_to_8bit(frame->data[0] + y * frame->linesize[0] + x * 2);
		const uint8_t *uv = frame->data[1] + (y / 2) * frame->linesize[1] + (x / 2) * 4;
		rgb = yuv_to_rgb(y_value, yuv_wide_to_8bit(uv), yuv_wide_to_8bit(uv + 2));
		return true;
	}
	case VIDEO_FORMAT_I010: {
		if (!frame->data[1] || !frame->data[2])
			return false;
		const uint8_t y_value = yuv_wide_to_8bit(frame->data[0] + y * frame->linesize[0] + x * 2);
		const uint8_t u_value = yuv_wide_to_8bit(frame->data[1] + (y / 2) * frame->linesize[1] + (x / 2) * 2);
		const uint8_t v_value = yuv_wide_to_8bit(frame->data[2] + (y / 2) * frame->linesize[2] + (x / 2) * 2);
		rgb = yuv_to_rgb(y_value, u_value, v_value);
		return true;
	}
	case VIDEO_FORMAT_P216: {
		if (!frame->data[1])
			return false;
		const uint8_t y_value = yuv_wide_to_8bit(frame->data[0] + y * frame->linesize[0] + x * 2);
		const uint8_t *uv = frame->data[1] + y * frame->linesize[1] + (x / 2) * 4;
		rgb = yuv_to_rgb(y_value, yuv_wide_to_8bit(uv), yuv_wide_to_8bit(uv + 2));
		return true;
	}
	case VIDEO_FORMAT_I210: {
		if (!frame->data[1] || !frame->data[2])
			return false;
		const uint8_t y_value = yuv_wide_to_8bit(frame->data[0] + y * frame->linesize[0] + x * 2);
		const uint8_t u_value = yuv_wide_to_8bit(frame->data[1] + y * frame->linesize[1] + (x / 2) * 2);
		const uint8_t v_value = yuv_wide_to_8bit(frame->data[2] + y * frame->linesize[2] + (x / 2) * 2);
		rgb = yuv_to_rgb(y_value, u_value, v_value);
		return true;
	}
	case VIDEO_FORMAT_P416: {
		if (!frame->data[1])
			return false;
		const uint8_t y_value = yuv_wide_to_8bit(frame->data[0] + y * frame->linesize[0] + x * 2);
		const uint8_t *uv = frame->data[1] + y * frame->linesize[1] + x * 4;
		rgb = yuv_to_rgb(y_value, yuv_wide_to_8bit(uv), yuv_wide_to_8bit(uv + 2));
		return true;
	}
	case VIDEO_FORMAT_I412: {
		if (!frame->data[1] || !frame->data[2])
			return false;
		const uint8_t y_value = yuv_wide_to_8bit(frame->data[0] + y * frame->linesize[0] + x * 2);
		const uint8_t u_value = yuv_wide_to_8bit(frame->data[1] + y * frame->linesize[1] + x * 2);
		const uint8_t v_value = yuv_wide_to_8bit(frame->data[2] + y * frame->linesize[2] + x * 2);
		rgb = yuv_to_rgb(y_value, u_value, v_value);
		return true;
	}
	default:
		return false;
	}
}

static bool is_color_sample_for_target(CameraTallyFilter *filter, Rgb rgb, Rgb target)
{
	const int max_channel = std::max({rgb.r, rgb.g, rgb.b});
	if (max_channel < 40)
		return false;

	const double sample_total = std::max(1, static_cast<int>(rgb.r) + rgb.g + rgb.b);
	const double target_total = std::max(1, static_cast<int>(target.r) + target.g + target.b);
	const double sr = static_cast<double>(rgb.r) / sample_total;
	const double sg = static_cast<double>(rgb.g) / sample_total;
	const double sb = static_cast<double>(rgb.b) / sample_total;
	const double tr = static_cast<double>(target.r) / target_total;
	const double tg = static_cast<double>(target.g) / target_total;
	const double tb = static_cast<double>(target.b) / target_total;
	const double target_norm = std::max(0.001, tr * tr + tg * tg + tb * tb);
	const double color_score = (sr * tr + sg * tg + sb * tb) / target_norm;

	return color_score >= filter->red_threshold;
}

static bool is_color_sample(CameraTallyFilter *filter, Rgb rgb)
{
	const Rgb target{static_cast<uint8_t>(clamp_int(filter->color_r, 0, 255)),
			 static_cast<uint8_t>(clamp_int(filter->color_g, 0, 255)),
			 static_cast<uint8_t>(clamp_int(filter->color_b, 0, 255))};
	return is_color_sample_for_target(filter, rgb, target);
}

static double color_saturation(Rgb rgb)
{
	const int max_channel = std::max({rgb.r, rgb.g, rgb.b});
	const int min_channel = std::min({rgb.r, rgb.g, rgb.b});
	if (max_channel <= 0)
		return 0.0;

	return static_cast<double>(max_channel - min_channel) / max_channel;
}

static double color_hue_degrees(Rgb rgb)
{
	const double r = rgb.r / 255.0;
	const double g = rgb.g / 255.0;
	const double b = rgb.b / 255.0;
	const double max_value = std::max({r, g, b});
	const double min_value = std::min({r, g, b});
	const double delta = max_value - min_value;
	if (delta <= 0.0001)
		return 0.0;

	double hue = 0.0;
	if (max_value == r) {
		hue = 60.0 * std::fmod(((g - b) / delta), 6.0);
	} else if (max_value == g) {
		hue = 60.0 * (((b - r) / delta) + 2.0);
	} else {
		hue = 60.0 * (((r - g) / delta) + 4.0);
	}

	if (hue < 0.0)
		hue += 360.0;
	return hue;
}

static double hue_distance(double a, double b)
{
	const double diff = std::abs(a - b);
	return std::min(diff, 360.0 - diff);
}

static bool is_auto_detect_color_sample(CameraTallyFilter *filter, Rgb rgb, Rgb target)
{
	const int max_channel = std::max({rgb.r, rgb.g, rgb.b});
	const double target_saturation = color_saturation(target);
	const double sample_saturation = color_saturation(rgb);

	if (target_saturation < 0.18)
		return is_color_sample_for_target(filter, rgb, target);

	if (max_channel < 70 || sample_saturation < std::max(0.55, target_saturation * 0.80))
		return false;

	const double hue_diff = hue_distance(color_hue_degrees(rgb), color_hue_degrees(target));
	if (hue_diff > 8.0)
		return false;

	const int dr = static_cast<int>(rgb.r) - target.r;
	const int dg = static_cast<int>(rgb.g) - target.g;
	const int db = static_cast<int>(rgb.b) - target.b;
	const double rgb_distance = std::sqrt(static_cast<double>(dr * dr + dg * dg + db * db));
	if (rgb_distance > 150.0)
		return false;

	const double sample_total = std::max(1, static_cast<int>(rgb.r) + rgb.g + rgb.b);
	const double target_total = std::max(1, static_cast<int>(target.r) + target.g + target.b);
	const double sr = static_cast<double>(rgb.r) / sample_total;
	const double sg = static_cast<double>(rgb.g) / sample_total;
	const double sb = static_cast<double>(rgb.b) / sample_total;
	const double tr = static_cast<double>(target.r) / target_total;
	const double tg = static_cast<double>(target.g) / target_total;
	const double tb = static_cast<double>(target.b) / target_total;
	const double channel_distance =
		std::sqrt((sr - tr) * (sr - tr) + (sg - tg) * (sg - tg) + (sb - tb) * (sb - tb));
	return channel_distance <= 0.12;
}

static bool is_auto_detect_indicator_sample(CameraTallyFilter *filter, Rgb rgb, Rgb target)
{
	return is_auto_detect_color_sample(filter, rgb, target);
}

static void apply_color_state(CameraTallyFilter *filter, bool sees_color, const uint8_t *data = nullptr,
			      uint32_t linesize = 0, uint32_t width = 0, uint32_t height = 0)
{
	if (!filter->armed)
		return;

	if (sees_color) {
		filter->red_frames++;
		filter->non_red_frames = 0;
	} else {
		filter->red_frames = 0;
		filter->non_red_frames++;
	}

	if (filter->red_frames >= filter->start_frames && filter->state != TallyState::Rolling) {
		filter->state = TallyState::Rolling;
		obs_log(LOG_INFO, "RecPilot color detected; starting OBS recording");
		if (filter->metadata_csv_per_card)
			refresh_metadata_ocr_once(filter, data, linesize, width, height);
		request_recording_start(filter, latest_clip_name(filter));
	}

	if (filter->non_red_frames >= filter->stop_frames && filter->state == TallyState::Rolling) {
		filter->state = TallyState::Idle;
		obs_log(LOG_INFO, "RecPilot color lost; stopping OBS recording");
		request_recording_stop();
	}
}

static bool analyse_frame_red(CameraTallyFilter *filter, const obs_source_frame *frame)
{
	if (!frame || frame->width == 0 || frame->height == 0)
		return false;

	const int cx =
		clamp_int(static_cast<int>(filter->center_x * frame->width), 0, static_cast<int>(frame->width - 1));
	const int cy =
		clamp_int(static_cast<int>(filter->center_y * frame->height), 0, static_cast<int>(frame->height - 1));
	const int radius = std::max(2, static_cast<int>(filter->radius * std::min(frame->width, frame->height)));
	const int radius_sq = radius * radius;

	int samples = 0;
	int red_hits = 0;

	for (int y = cy - radius; y <= cy + radius; y += 2) {
		if (y < 0 || y >= static_cast<int>(frame->height))
			continue;

		for (int x = cx - radius; x <= cx + radius; x += 2) {
			if (x < 0 || x >= static_cast<int>(frame->width))
				continue;
			const int dx = x - cx;
			const int dy = y - cy;
			if (dx * dx + dy * dy > radius_sq)
				continue;

			Rgb rgb{};
			if (!read_pixel(frame, static_cast<uint32_t>(x), static_cast<uint32_t>(y), rgb))
				continue;

			samples++;
			if (is_color_sample(filter, rgb))
				red_hits++;
		}
	}

	if (samples == 0)
		return false;

	const double red_ratio = static_cast<double>(red_hits) / samples;
	return red_ratio >= filter->red_coverage;
}

struct ColorComponent {
	int count = 0;
	int min_x = 0;
	int max_x = 0;
	int min_y = 0;
	int max_y = 0;
	double sum_x = 0.0;
	double sum_y = 0.0;
	double score = 0.0;
};

struct CircleCandidate {
	double x = 0.0;
	double y = 0.0;
	double radius = 0.0;
	double score = 0.0;
};

struct AutoDetectStats {
	int readable_samples = 0;
	int color_hits = 0;
	int components = 0;
	double best_component_score = 0.0;
	double best_density_score = 0.0;
};

static bool is_auto_detect_search_row(int gy, int grid_h)
{
	const int band_h = std::max(1, static_cast<int>(std::ceil(grid_h * 0.15)));
	return gy < band_h || gy >= grid_h - band_h;
}

static double score_circle_candidate(const std::vector<uint8_t> &mask, int grid_w, int grid_h, int cx, int cy,
				     int radius)
{
	const int inner_radius_sq = radius * radius;
	const int outer_radius = radius + std::max(1, radius / 2);
	const int outer_radius_sq = outer_radius * outer_radius;
	const int core_radius = std::max(1, radius / 2);
	const int core_radius_sq = core_radius * core_radius;

	int inner_area = 0;
	int inner_hits = 0;
	int outer_area = 0;
	int outer_hits = 0;
	int core_area = 0;
	int core_hits = 0;
	int left_hits = 0;
	int right_hits = 0;
	int top_hits = 0;
	int bottom_hits = 0;

	for (int y = cy - outer_radius; y <= cy + outer_radius; ++y) {
		if (y < 0 || y >= grid_h)
			continue;

		for (int x = cx - outer_radius; x <= cx + outer_radius; ++x) {
			if (x < 0 || x >= grid_w)
				continue;

			const int dx = x - cx;
			const int dy = y - cy;
			const int dist_sq = dx * dx + dy * dy;
			const bool hit = mask[static_cast<size_t>(y) * grid_w + x] != 0;

			if (dist_sq <= core_radius_sq) {
				core_area++;
				if (hit)
					core_hits++;
			}

			if (dist_sq <= inner_radius_sq) {
				inner_area++;
				if (hit) {
					inner_hits++;
					if (dx < 0)
						left_hits++;
					else if (dx > 0)
						right_hits++;
					if (dy < 0)
						top_hits++;
					else if (dy > 0)
						bottom_hits++;
				}
			} else if (dist_sq <= outer_radius_sq) {
				outer_area++;
				if (hit)
					outer_hits++;
			}
		}
	}

	if (inner_area == 0 || inner_hits < 2 || core_area == 0)
		return 0.0;

	const double inner_density = static_cast<double>(inner_hits) / inner_area;
	const double core_density = static_cast<double>(core_hits) / core_area;
	const double outer_density = outer_area > 0 ? static_cast<double>(outer_hits) / outer_area : 0.0;
	if (inner_density < 0.08 || core_density < 0.18)
		return 0.0;

	const double horizontal_balance =
		1.0 - std::abs(left_hits - right_hits) / static_cast<double>(std::max(1, left_hits + right_hits));
	const double vertical_balance =
		1.0 - std::abs(top_hits - bottom_hits) / static_cast<double>(std::max(1, top_hits + bottom_hits));
	const double isolation = std::max(0.0, inner_density - outer_density * 0.45);
	return inner_hits * isolation * std::clamp(horizontal_balance, 0.0, 1.0) *
	       std::clamp(vertical_balance, 0.0, 1.0);
}

static bool find_circle_by_local_density(const std::vector<uint8_t> &mask, int grid_w, int grid_h, int step,
					 uint32_t width, uint32_t height, double min_radius_px, double max_radius_px,
					 CircleCandidate &best)
{
	std::vector<int> color_cells;
	color_cells.reserve(mask.size() / 32);
	for (int i = 0; i < static_cast<int>(mask.size()); ++i) {
		if (mask[static_cast<size_t>(i)])
			color_cells.push_back(i);
	}

	if (color_cells.empty())
		return false;

	const int min_radius = std::max(2, static_cast<int>(std::floor(min_radius_px / step)));
	const int max_radius = std::max(min_radius, static_cast<int>(std::ceil(max_radius_px / step)));
	const int stride = color_cells.size() > 6000 ? 2 : 1;

	for (int i = 0; i < static_cast<int>(color_cells.size()); i += stride) {
		const int index = color_cells[static_cast<size_t>(i)];
		const int cx = index % grid_w;
		const int cy = index / grid_w;

		for (int radius = min_radius; radius <= max_radius; ++radius) {
			const double score = score_circle_candidate(mask, grid_w, grid_h, cx, cy, radius);
			if (score <= best.score)
				continue;

			best.score = score;
			best.x = std::clamp((cx * step + step * 0.5) / static_cast<double>(width), 0.0, 1.0);
			best.y = std::clamp((cy * step + step * 0.5) / static_cast<double>(height), 0.0, 1.0);
			best.radius =
				std::clamp((radius * step) / static_cast<double>(std::min(width, height)), 0.002, 0.2);
		}
	}

	return best.score > 0.0;
}

static bool auto_detect_target_circle(CameraTallyFilter *filter, const obs_source_frame *frame, Rgb target,
				      double &out_x, double &out_y, double &out_radius, AutoDetectStats &stats)
{
	if (!filter || !frame || frame->width == 0 || frame->height == 0)
		return false;

	const uint32_t width = frame->width;
	const uint32_t height = frame->height;
	const uint32_t min_dim = std::min(width, height);
	const int step = min_dim >= 1600 ? 3 : 2;
	const int grid_w = static_cast<int>((width + step - 1) / step);
	const int grid_h = static_cast<int>((height + step - 1) / step);
	std::vector<uint8_t> mask(static_cast<size_t>(grid_w) * grid_h, 0);
	std::vector<uint8_t> visited(mask.size(), 0);

	for (int gy = 0; gy < grid_h; ++gy) {
		if (!is_auto_detect_search_row(gy, grid_h))
			continue;
		const uint32_t y = std::min<uint32_t>(static_cast<uint32_t>(gy * step), height - 1);
		for (int gx = 0; gx < grid_w; ++gx) {
			const uint32_t x = std::min<uint32_t>(static_cast<uint32_t>(gx * step), width - 1);
			Rgb rgb{};
			if (!read_pixel(frame, x, y, rgb))
				continue;

			stats.readable_samples++;
			if (is_auto_detect_indicator_sample(filter, rgb, target)) {
				mask[static_cast<size_t>(gy) * grid_w + gx] = 1;
				stats.color_hits++;
			}
		}
	}

	ColorComponent best;
	const int min_count = std::max(2, static_cast<int>((min_dim * 0.002 / step) * (min_dim * 0.002 / step)));
	const double min_radius_px = std::max(2.0, min_dim * 0.0025);
	const double max_radius_px = std::max(12.0, min_dim * 0.08);
	CircleCandidate local_best;
	find_circle_by_local_density(mask, grid_w, grid_h, step, width, height, min_radius_px, max_radius_px,
				     local_best);
	stats.best_density_score = local_best.score;
	std::vector<int> stack;
	stack.reserve(1024);

	for (int gy = 0; gy < grid_h; ++gy) {
		if (!is_auto_detect_search_row(gy, grid_h))
			continue;
		for (int gx = 0; gx < grid_w; ++gx) {
			const int start = gy * grid_w + gx;
			if (!mask[start] || visited[start])
				continue;

			ColorComponent component;
			stats.components++;
			component.min_x = component.max_x = gx;
			component.min_y = component.max_y = gy;
			stack.clear();
			stack.push_back(start);
			visited[start] = 1;

			while (!stack.empty()) {
				const int index = stack.back();
				stack.pop_back();
				const int x = index % grid_w;
				const int y = index / grid_w;
				component.count++;
				component.sum_x += x;
				component.sum_y += y;
				component.min_x = std::min(component.min_x, x);
				component.max_x = std::max(component.max_x, x);
				component.min_y = std::min(component.min_y, y);
				component.max_y = std::max(component.max_y, y);

				const int neighbors[8][2] = {
					{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
				};
				for (const auto &neighbor : neighbors) {
					const int nx = x + neighbor[0];
					const int ny = y + neighbor[1];
					if (nx < 0 || nx >= grid_w || ny < 0 || ny >= grid_h)
						continue;
					const int neighbor_index = ny * grid_w + nx;
					if (!mask[neighbor_index] || visited[neighbor_index])
						continue;
					visited[neighbor_index] = 1;
					stack.push_back(neighbor_index);
				}
			}

			if (component.count < min_count)
				continue;

			const double box_w = (component.max_x - component.min_x + 1) * step;
			const double box_h = (component.max_y - component.min_y + 1) * step;
			const double aspect = box_w > box_h ? box_w / std::max(1.0, box_h)
							    : box_h / std::max(1.0, box_w);
			const double box_area = static_cast<double>((component.max_x - component.min_x + 1) *
								    (component.max_y - component.min_y + 1));
			const double fill = static_cast<double>(component.count) / std::max(1.0, box_area);
			const double radius_px = std::max(box_w, box_h) * 0.55;
			if (aspect > 2.2 || fill < 0.22 || radius_px < min_radius_px || radius_px > max_radius_px)
				continue;

			const double circularity_penalty = std::abs(1.0 - aspect) * 0.35 + std::abs(0.65 - fill) * 0.2;
			component.score = component.count * (1.0 - std::min(0.8, circularity_penalty));
			if (component.score > best.score) {
				best = component;
				stats.best_component_score = component.score;
			}
		}
	}

	if (best.score <= 0.0 && local_best.score <= 0.0)
		return false;

	if (local_best.score > best.score * 0.35) {
		out_x = local_best.x;
		out_y = local_best.y;
		out_radius = local_best.radius;
		return true;
	}

	out_x = std::clamp(((best.sum_x / best.count) * step + step * 0.5) / width, 0.0, 1.0);
	out_y = std::clamp(((best.sum_y / best.count) * step + step * 0.5) / height, 0.0, 1.0);
	const double radius_px = std::max(best.max_x - best.min_x + 1, best.max_y - best.min_y + 1) * step * 0.58;
	out_radius = std::clamp(radius_px / static_cast<double>(min_dim), 0.002, 0.2);
	return true;
}

static void apply_auto_detect_center(CameraTallyFilter *filter, const obs_source_frame *frame)
{
	if (!filter || !filter->auto_detect_center_pending)
		return;

	double x = 0.0;
	double y = 0.0;
	double radius = 0.0;
	AutoDetectStats stats;
	const Rgb chosen{static_cast<uint8_t>(clamp_int(filter->color_r, 0, 255)),
			 static_cast<uint8_t>(clamp_int(filter->color_g, 0, 255)),
			 static_cast<uint8_t>(clamp_int(filter->color_b, 0, 255))};
	const struct {
		const char *name;
		Rgb color;
	} targets[] = {
		{"red", {255, 0, 0}},
		{"green", {0, 255, 0}},
		{"selected", chosen},
	};

	const char *found_name = nullptr;
	bool found = false;
	for (const auto &target : targets) {
		AutoDetectStats pass_stats;
		if (auto_detect_target_circle(filter, frame, target.color, x, y, radius, pass_stats)) {
			stats = pass_stats;
			found_name = target.name;
			found = true;
			break;
		}

		stats.readable_samples += pass_stats.readable_samples;
		stats.color_hits += pass_stats.color_hits;
		stats.components += pass_stats.components;
		stats.best_component_score = std::max(stats.best_component_score, pass_stats.best_component_score);
		stats.best_density_score = std::max(stats.best_density_score, pass_stats.best_density_score);
	}

	obs_data_t *settings = obs_source_get_settings(filter->context);
	if (!settings)
		return;

	char detail[256];
	std::snprintf(
		detail, sizeof(detail),
		"%s%s%d/%d color pixels, %d candidates, tried red/green/selected, top/bottom 15%%, format %s, %ux%u",
		found_name ? found_name : "", found_name ? ": " : "", stats.color_hits, stats.readable_samples,
		stats.components, video_format_name(frame->format), frame->width, frame->height);

	if (found) {
		obs_data_set_double(settings, "center_x", x);
		obs_data_set_double(settings, "center_y", y);
		obs_data_set_double(settings, "radius", radius);
		obs_data_set_bool(settings, "show_overlay", true);
		obs_data_set_string(settings, "auto_detect_center_result", "found");
		obs_data_set_string(settings, "auto_detect_center_detail", detail);
		obs_log(LOG_INFO, "RecPilot auto-detected target circle: %.3f %.3f radius %.3f (%s)", x, y, radius,
			detail);
	} else {
		obs_data_set_string(settings, "auto_detect_center_result", "not_found");
		obs_data_set_string(settings, "auto_detect_center_detail", detail);
		obs_log(LOG_WARNING, "RecPilot auto-detect could not find a matching target circle (%s)", detail);
	}

	filter->auto_detect_center_pending = false;
	filter->auto_detect_band_overlay_frames = 0;
	obs_data_set_bool(settings, "auto_detect_center_pending", false);
	obs_source_update(filter->context, settings);
	obs_data_release(settings);
}

static bool copy_frame_rgba(const uint8_t *data, uint32_t linesize, uint32_t width, uint32_t height,
			    std::vector<uint8_t> &rgba)
{
	if (!data || width == 0 || height == 0)
		return false;

	rgba.resize(static_cast<size_t>(width) * height * 4);
	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t *src = data + y * linesize;
		uint8_t *dst = rgba.data() + static_cast<size_t>(y) * width * 4;
		std::memcpy(dst, src, static_cast<size_t>(width) * 4);
	}
	return true;
}

static std::string json_escape(const std::string &value)
{
	std::string out;
	out.reserve(value.size() + 8);
	for (unsigned char ch : value) {
		switch (ch) {
		case '\\':
			out += "\\\\";
			break;
		case '"':
			out += "\\\"";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (ch < 0x20) {
				char buffer[7];
				std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
				out += buffer;
			} else {
				out.push_back(static_cast<char>(ch));
			}
		}
	}
	return out;
}

static std::vector<MetadataField> parse_metadata_fields_json(const std::string &json)
{
	std::vector<MetadataField> fields;
	if (json.empty())
		return fields;

	std::string wrapped = "{\"fields\":";
	wrapped += json;
	wrapped += "}";
	obs_data_t *root = obs_data_create_from_json(wrapped.c_str());
	if (!root)
		return fields;

	obs_data_array_t *array = obs_data_get_array(root, "fields");
	const size_t count = array ? obs_data_array_count(array) : 0;
	fields.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *item = obs_data_array_item(array, i);
		if (!item)
			continue;

		MetadataField field;
		const char *name = obs_data_get_string(item, "name");
		const char *value = obs_data_get_string(item, "value");
		field.name = name ? name : "";
		field.value = value ? value : "";
		field.x = obs_data_get_double(item, "x");
		field.y = obs_data_get_double(item, "y");
		field.width = obs_data_get_double(item, "width");
		field.height = obs_data_get_double(item, "height");
		if (field.width <= 0.0)
			field.width = 0.12;
		if (field.height <= 0.0)
			field.height = 0.05;
		fields.push_back(field);
		obs_data_release(item);
	}

	if (array)
		obs_data_array_release(array);
	obs_data_release(root);
	return fields;
}

static std::string metadata_fields_to_json(const std::vector<MetadataField> &fields)
{
	std::string json = "[";
	bool first = true;
	for (const MetadataField &field : fields) {
		if (!first)
			json += ",";
		first = false;
		char buffer[256];
		std::snprintf(buffer, sizeof(buffer), "\"x\":%.6f,\"y\":%.6f,\"width\":%.6f,\"height\":%.6f", field.x,
			      field.y, field.width, field.height);
		json += "{\"name\":\"" + json_escape(field.name) + "\",\"value\":\"" + json_escape(field.value) + "\",";
		json += buffer;
		json += "}";
	}
	json += "]";
	return json;
}

static bool has_clapperboard_identity_fields(const std::string &json)
{
	for (const MetadataField &field : parse_metadata_fields_json(json)) {
		const std::string name = uppercase_ascii(field.name);
		if (!field.value.empty() &&
		    (name == "SCENE" || name == "SHOT" || name == "TAKE" || name == "SEQUENCE" || name == "SLATE"))
			return true;
	}
	return false;
}

static void add_clapperboard_field(std::map<std::string, std::string> &fields, const std::string &name,
				   std::string value)
{
	value = compact_spaces(std::move(value));
	while (!value.empty() && (value.front() == ':' || value.front() == '-' || value.front() == '#' ||
				  value.front() == '=' || value.front() == '|'))
		value = trim(value.substr(1));
	while (!value.empty() &&
	       (value.back() == ':' || value.back() == '-' || value.back() == '|' || value.back() == '.'))
		value.pop_back();
	value = compact_spaces(value);
	if (value.empty())
		return;
	if (value.size() > 64)
		value.resize(64);
	if (fields.find(name) == fields.end())
		fields[name] = value;
}

static bool clapperboard_word_at(const std::string &upper, size_t pos, const std::string &label)
{
	if (pos == std::string::npos || pos + label.size() > upper.size())
		return false;
	const bool left_ok = pos == 0 || !std::isalnum(static_cast<unsigned char>(upper[pos - 1]));
	const size_t right = pos + label.size();
	const bool right_ok = right >= upper.size() || !std::isalnum(static_cast<unsigned char>(upper[right]));
	return left_ok && right_ok;
}

static size_t clapperboard_find_label(const std::string &upper, const std::string &label, size_t start = 0)
{
	size_t pos = upper.find(label, start);
	while (pos != std::string::npos) {
		if (clapperboard_word_at(upper, pos, label))
			return pos;
		pos = upper.find(label, pos + 1);
	}
	return std::string::npos;
}

static std::string clapperboard_value_after_label(const std::string &line, const std::string &upper,
						  const std::string &label)
{
	const size_t pos = clapperboard_find_label(upper, label);
	if (pos == std::string::npos)
		return {};
	size_t start = pos + label.size();
	while (start < line.size() && (std::isspace(static_cast<unsigned char>(line[start])) || line[start] == ':' ||
				       line[start] == '-' || line[start] == '#' || line[start] == '='))
		start++;

	const std::vector<std::string> stop_labels = {"SLATE", "SCENE", "SHOT",   "TAKE", "SEQUENCE", "SEQ",
						      "ROLL",  "REEL",  "CAMERA", "CAM",  "FPS",      "SHUTTER",
						      "ISO",   "EI",    "WB",     "LENS", "FILTER",   "FILTERS",
						      "ND",    "STOP",  "TILT"};
	size_t end = line.size();
	for (const std::string &stop : stop_labels) {
		size_t next = clapperboard_find_label(upper, stop, start + 1);
		if (next != std::string::npos && next < end)
			end = next;
	}
	return trim(line.substr(start, end - start));
}

static void clapperboard_extract_roll_clip(std::map<std::string, std::string> &fields, const std::string &text)
{
	const std::string upper = uppercase_ascii(text);
	for (size_t i = 0; i + 6 < upper.size(); ++i) {
		if (!std::isalpha(static_cast<unsigned char>(upper[i])))
			continue;
		size_t j = i + 1;
		while (j < upper.size() && std::isdigit(static_cast<unsigned char>(upper[j])) && j - i <= 5)
			j++;
		if (j - i < 4 || j >= upper.size() || upper[j] != 'C')
			continue;
		size_t k = j + 1;
		while (k < upper.size() && std::isdigit(static_cast<unsigned char>(upper[k])) && k - j <= 5)
			k++;
		if (k == j + 1)
			continue;
		add_clapperboard_field(fields, "Camera", upper.substr(i, 1));
		add_clapperboard_field(fields, "Roll", upper.substr(i, j - i));
		add_clapperboard_field(fields, "Clip", upper.substr(j + 1, k - j - 1));
		return;
	}
}

static void clapperboard_extract_inline_numbers(std::map<std::string, std::string> &fields, const std::string &text)
{
	const std::string upper = uppercase_ascii(text);
	auto extract_before = [&](const std::string &suffix) -> std::string {
		const size_t pos = upper.find(suffix);
		if (pos == std::string::npos || pos == 0)
			return {};
		size_t start = pos;
		while (start > 0) {
			const char ch = upper[start - 1];
			if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != '.')
				break;
			start--;
		}
		return trim(text.substr(start, pos - start));
	};
	auto extract_after = [&](const std::string &prefix) -> std::string {
		size_t pos = clapperboard_find_label(upper, prefix);
		if (pos == std::string::npos)
			return {};
		pos += prefix.size();
		while (pos < text.size() && !std::isdigit(static_cast<unsigned char>(text[pos])))
			pos++;
		size_t end = pos;
		while (end < text.size() &&
		       (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '.' || text[end] == '/'))
			end++;
		return trim(text.substr(pos, end - pos));
	};

	add_clapperboard_field(fields, "Color Temp", extract_before("K"));
	add_clapperboard_field(fields, "ISO", extract_before("EI"));
	add_clapperboard_field(fields, "ISO", extract_after("ISO"));
	add_clapperboard_field(fields, "Filters", extract_after("ND"));
	add_clapperboard_field(fields, "FPS", extract_after("FPS"));
	add_clapperboard_field(fields, "Shutter", extract_after("SHUTTER"));
}

static std::string clapperboard_fields_json_from_text(const std::string &ocr_text)
{
	std::map<std::string, std::string> fields;
	const std::string text = compact_spaces(ocr_text);
	if (text.empty())
		return "[]";

	clapperboard_extract_roll_clip(fields, text);
	clapperboard_extract_inline_numbers(fields, text);

	size_t start = 0;
	while (start < ocr_text.size()) {
		size_t end = ocr_text.find('\n', start);
		if (end == std::string::npos)
			end = ocr_text.size();
		const std::string line = compact_spaces(ocr_text.substr(start, end - start));
		const std::string upper = uppercase_ascii(line);
		if (!line.empty()) {
			add_clapperboard_field(fields, "Slate", clapperboard_value_after_label(line, upper, "SLATE"));
			add_clapperboard_field(fields, "Scene", clapperboard_value_after_label(line, upper, "SCENE"));
			add_clapperboard_field(fields, "Scene", clapperboard_value_after_label(line, upper, "SC"));
			add_clapperboard_field(fields, "Shot", clapperboard_value_after_label(line, upper, "SHOT"));
			add_clapperboard_field(fields, "Take", clapperboard_value_after_label(line, upper, "TAKE"));
			add_clapperboard_field(fields, "Sequence",
					       clapperboard_value_after_label(line, upper, "SEQUENCE"));
			add_clapperboard_field(fields, "Sequence", clapperboard_value_after_label(line, upper, "SEQ"));
			add_clapperboard_field(fields, "Roll", clapperboard_value_after_label(line, upper, "ROLL"));
			add_clapperboard_field(fields, "Roll", clapperboard_value_after_label(line, upper, "REEL"));
			add_clapperboard_field(fields, "Camera", clapperboard_value_after_label(line, upper, "CAMERA"));
			add_clapperboard_field(fields, "Camera", clapperboard_value_after_label(line, upper, "CAM"));
			add_clapperboard_field(fields, "Lens", clapperboard_value_after_label(line, upper, "LENS"));
			add_clapperboard_field(fields, "Filters",
					       clapperboard_value_after_label(line, upper, "FILTERS"));
			add_clapperboard_field(fields, "Filters",
					       clapperboard_value_after_label(line, upper, "FILTER"));
			add_clapperboard_field(fields, "Stop", clapperboard_value_after_label(line, upper, "STOP"));
			add_clapperboard_field(fields, "FPS", clapperboard_value_after_label(line, upper, "FPS"));
			add_clapperboard_field(fields, "Shutter",
					       clapperboard_value_after_label(line, upper, "SHUTTER"));
			add_clapperboard_field(fields, "ISO", clapperboard_value_after_label(line, upper, "ISO"));
			add_clapperboard_field(fields, "ISO", clapperboard_value_after_label(line, upper, "EI"));
			add_clapperboard_field(fields, "Color Temp", clapperboard_value_after_label(line, upper, "WB"));
		}
		start = end + 1;
	}

	std::string json = "[";
	bool first = true;
	for (const auto &field : fields) {
		if (!first)
			json += ",";
		first = false;
		json += "{\"name\":\"" + json_escape(field.first) + "\",\"value\":\"" + json_escape(field.second) +
			"\"}";
	}
	json += "]";
	return json;
}

static std::string clapperboard_fields_json_from_observations(const std::string &observations_json, bool border_only)
{
	std::string wrapped = "{\"observations\":";
	wrapped += observations_json.empty() ? "[]" : observations_json;
	wrapped += "}";
	obs_data_t *root = obs_data_create_from_json(wrapped.c_str());
	if (!root)
		return "[]";

	obs_data_array_t *array = obs_data_get_array(root, "observations");
	std::vector<MetadataField> fields;
	std::set<std::string> seen;
	const size_t count = array ? obs_data_array_count(array) : 0;
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *item = obs_data_array_item(array, i);
		if (!item)
			continue;

		const char *raw_text = obs_data_get_string(item, "text");
		const std::string text = raw_text ? compact_spaces(raw_text) : "";
		const double x = obs_data_get_double(item, "x");
		const double y = obs_data_get_double(item, "y");
		const double w = obs_data_get_double(item, "width");
		const double h = obs_data_get_double(item, "height");
		const bool in_border = x < 0.15 || y < 0.15 || x + w > 0.85 || y + h > 0.85;
		if (text.empty() || (border_only && !in_border)) {
			obs_data_release(item);
			continue;
		}

		const std::string parsed_json = clapperboard_fields_json_from_text(text);
		for (MetadataField field : parse_metadata_fields_json(parsed_json)) {
			if (field.name.empty() || field.value.empty() || seen.find(field.name) != seen.end())
				continue;
			field.x = std::clamp(x - 0.01, 0.0, 0.98);
			field.y = std::clamp(y - 0.006, 0.0, 0.98);
			field.width = std::clamp(w + 0.02, 0.02, 1.0 - field.x);
			field.height = std::clamp(h + 0.012, 0.02, 1.0 - field.y);
			fields.push_back(field);
			seen.insert(field.name);
		}
		obs_data_release(item);
	}

	if (array)
		obs_data_array_release(array);
	obs_data_release(root);

	if (!fields.empty()) {
		const std::string candidate = metadata_fields_to_json(fields);
		if (border_only || has_clapperboard_identity_fields(candidate))
			return candidate;
	}

	std::string text;
	root = obs_data_create_from_json(wrapped.c_str());
	if (!root)
		return "[]";
	array = obs_data_get_array(root, "observations");
	const size_t fallback_count = array ? obs_data_array_count(array) : 0;
	for (size_t i = 0; i < fallback_count; ++i) {
		obs_data_t *item = obs_data_array_item(array, i);
		if (!item)
			continue;
		const double x = obs_data_get_double(item, "x");
		const double y = obs_data_get_double(item, "y");
		const double w = obs_data_get_double(item, "width");
		const double h = obs_data_get_double(item, "height");
		const bool in_border = x < 0.15 || y < 0.15 || x + w > 0.85 || y + h > 0.85;
		if (!border_only || in_border) {
			if (!text.empty())
				text += "\n";
			const char *raw_text = obs_data_get_string(item, "text");
			text += raw_text ? raw_text : "";
		}
		obs_data_release(item);
	}
	if (array)
		obs_data_array_release(array);
	obs_data_release(root);
	return clapperboard_fields_json_from_text(text);
}

static std::string clapperboard_crop_json_from_observations(const std::string &observations_json)
{
	std::string wrapped = "{\"observations\":";
	wrapped += observations_json.empty() ? "[]" : observations_json;
	wrapped += "}";
	obs_data_t *root = obs_data_create_from_json(wrapped.c_str());
	if (!root)
		return {};

	obs_data_array_t *array = obs_data_get_array(root, "observations");
	const size_t count = array ? obs_data_array_count(array) : 0;
	double left = 1.0;
	double top = 1.0;
	double right = 0.0;
	double bottom = 0.0;
	size_t used = 0;
	auto clap_score = [](const std::string &text) {
		const std::string upper = uppercase_ascii(text);
		int score = 0;
		const std::array<std::string, 12> words = {"SLATE", "SCENE", "SHOT",   "TAKE", "SEQUENCE", "SEQ",
							   "ROLL",  "REEL",  "CAMERA", "CAM",  "PROD",     "DIRECTOR"};
		for (const std::string &word : words) {
			if (upper.find(word) != std::string::npos)
				score += 3;
		}
		for (char ch : upper) {
			if (std::isdigit(static_cast<unsigned char>(ch)))
				score += 1;
		}
		return score;
	};
	struct ObservationBox {
		std::string text;
		double x = 0.0;
		double y = 0.0;
		double w = 0.0;
		double h = 0.0;
		int score = 0;
	};
	std::vector<ObservationBox> boxes;
	boxes.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *item = obs_data_array_item(array, i);
		if (!item)
			continue;
		const char *raw_text = obs_data_get_string(item, "text");
		const std::string text = raw_text ? compact_spaces(raw_text) : "";
		const double x = obs_data_get_double(item, "x");
		const double y = obs_data_get_double(item, "y");
		const double w = obs_data_get_double(item, "width");
		const double h = obs_data_get_double(item, "height");
		if (!text.empty() && w > 0.002 && h > 0.002)
			boxes.push_back({text, x, y, w, h, clap_score(text)});
		obs_data_release(item);
	}
	if (array)
		obs_data_array_release(array);
	obs_data_release(root);

	int best_score = 0;
	for (const ObservationBox &box : boxes)
		best_score = std::max(best_score, box.score);

	for (const ObservationBox &box : boxes) {
		const bool in_hud_border = box.x < 0.15 || box.y < 0.15 || box.x + box.w > 0.85 || box.y + box.h > 0.85;
		const bool use = best_score > 0 ? box.score > 0 : !in_hud_border;
		if (!use)
			continue;
		left = std::min(left, box.x);
		top = std::min(top, box.y);
		right = std::max(right, box.x + box.w);
		bottom = std::max(bottom, box.y + box.h);
		used++;
	}

	if (used == 0 || right <= left || bottom <= top)
		return {};

	const double pad_x = std::max(0.06, (right - left) * 0.65);
	const double pad_top = std::max(0.16, (bottom - top) * 1.25);
	const double pad_bottom = std::max(0.08, (bottom - top) * 0.70);
	left = std::clamp(left - pad_x, 0.0, 1.0);
	top = std::clamp(top - pad_top, 0.0, 1.0);
	right = std::clamp(right + pad_x, 0.0, 1.0);
	bottom = std::clamp(bottom + pad_bottom, 0.0, 1.0);
	if (right - left < 0.20) {
		const double center = (left + right) * 0.5;
		left = std::clamp(center - 0.10, 0.0, 0.80);
		right = std::clamp(left + 0.20, 0.20, 1.0);
	}
	if (bottom - top < 0.18) {
		const double center = (top + bottom) * 0.5;
		top = std::clamp(center - 0.09, 0.0, 0.82);
		bottom = std::clamp(top + 0.18, 0.18, 1.0);
	}

	std::ostringstream json;
	json << "{\"x\":" << left << ",\"y\":" << top << ",\"width\":" << (right - left)
	     << ",\"height\":" << (bottom - top) << "}";
	return json.str();
}

static NormalizedRect parse_normalized_rect_json(const std::string &json)
{
	NormalizedRect rect;
	if (json.empty())
		return rect;
	obs_data_t *data = obs_data_create_from_json(json.c_str());
	if (!data)
		return rect;
	rect.x = obs_data_get_double(data, "x");
	rect.y = obs_data_get_double(data, "y");
	rect.width = obs_data_get_double(data, "width");
	rect.height = obs_data_get_double(data, "height");
	rect.valid = rect.width > 0.01 && rect.height > 0.01;
	obs_data_release(data);
	return rect;
}

static std::string normalized_rect_json(const NormalizedRect &rect)
{
	if (!rect.valid)
		return {};
	std::ostringstream json;
	json << "{\"x\":" << rect.x << ",\"y\":" << rect.y << ",\"width\":" << rect.width
	     << ",\"height\":" << rect.height << "}";
	return json.str();
}

static NormalizedRect detect_clapperboard_crop_from_rgba(const std::vector<uint8_t> &pixels, uint32_t width,
							 uint32_t height)
{
	NormalizedRect result;
	if (pixels.empty() || width < 64 || height < 64)
		return result;

	const int grid_w = 120;
	const int grid_h = std::max(64, static_cast<int>(std::round(static_cast<double>(height) * grid_w / width)));
	std::vector<double> gray(static_cast<size_t>(grid_w) * grid_h, 0.0);
	std::vector<double> saturation(static_cast<size_t>(grid_w) * grid_h, 0.0);

	for (int gy = 0; gy < grid_h; ++gy) {
		const uint32_t sy = std::min<uint32_t>(
			height - 1, static_cast<uint32_t>((static_cast<double>(gy) + 0.5) * height / grid_h));
		for (int gx = 0; gx < grid_w; ++gx) {
			const uint32_t sx = std::min<uint32_t>(
				width - 1, static_cast<uint32_t>((static_cast<double>(gx) + 0.5) * width / grid_w));
			const size_t offset = (static_cast<size_t>(sy) * width + sx) * 4;
			const double r = pixels[offset + 0];
			const double g = pixels[offset + 1];
			const double b = pixels[offset + 2];
			gray[static_cast<size_t>(gy) * grid_w + gx] = 0.299 * r + 0.587 * g + 0.114 * b;
			saturation[static_cast<size_t>(gy) * grid_w + gx] = std::max({r, g, b}) - std::min({r, g, b});
		}
	}

	std::vector<int> edge_cell(static_cast<size_t>(grid_w) * grid_h, 0);
	for (int y = 1; y < grid_h - 1; ++y) {
		for (int x = 1; x < grid_w - 1; ++x) {
			const double edge = std::abs(gray[static_cast<size_t>(y) * grid_w + x + 1] -
						     gray[static_cast<size_t>(y) * grid_w + x - 1]) +
					    std::abs(gray[static_cast<size_t>(y + 1) * grid_w + x] -
						     gray[static_cast<size_t>(y - 1) * grid_w + x]);
			edge_cell[static_cast<size_t>(y) * grid_w + x] = edge > 35.0 ? 1 : 0;
		}
	}

	const int integral_w = grid_w + 1;
	std::vector<double> sum(static_cast<size_t>(integral_w) * (grid_h + 1), 0.0);
	std::vector<double> sum_sq(static_cast<size_t>(integral_w) * (grid_h + 1), 0.0);
	std::vector<double> sat_sum(static_cast<size_t>(integral_w) * (grid_h + 1), 0.0);
	std::vector<int> neutral(static_cast<size_t>(integral_w) * (grid_h + 1), 0);
	std::vector<int> dark(static_cast<size_t>(integral_w) * (grid_h + 1), 0);
	std::vector<int> very_dark(static_cast<size_t>(integral_w) * (grid_h + 1), 0);
	std::vector<int> edge_sum(static_cast<size_t>(integral_w) * (grid_h + 1), 0);
	for (int y = 0; y < grid_h; ++y) {
		double row_sum = 0.0;
		double row_sum_sq = 0.0;
		double row_sat = 0.0;
		int row_neutral = 0;
		int row_dark = 0;
		int row_very_dark = 0;
		int row_edge = 0;
		for (int x = 0; x < grid_w; ++x) {
			const size_t cell = static_cast<size_t>(y) * grid_w + x;
			const double value = gray[cell];
			const double sat = saturation[cell];
			row_sum += value;
			row_sum_sq += value * value;
			row_sat += sat;
			row_neutral += value > 70.0 && sat < 65.0 ? 1 : 0;
			row_dark += value < 75.0 ? 1 : 0;
			row_very_dark += value < 45.0 ? 1 : 0;
			row_edge += edge_cell[cell];
			const size_t idx = static_cast<size_t>(y + 1) * integral_w + (x + 1);
			const size_t above = static_cast<size_t>(y) * integral_w + (x + 1);
			sum[idx] = sum[above] + row_sum;
			sum_sq[idx] = sum_sq[above] + row_sum_sq;
			sat_sum[idx] = sat_sum[above] + row_sat;
			neutral[idx] = neutral[above] + row_neutral;
			dark[idx] = dark[above] + row_dark;
			very_dark[idx] = very_dark[above] + row_very_dark;
			edge_sum[idx] = edge_sum[above] + row_edge;
		}
	}

	auto rect_stats = [&](int x, int y, int w, int h, double &mean, double &variance, double &neutral_frac,
			      double &dark_frac, double &very_dark_frac, double &edge_frac, double &sat_mean) {
		x = std::clamp(x, 0, grid_w - 1);
		y = std::clamp(y, 0, grid_h - 1);
		w = std::clamp(w, 1, grid_w - x);
		h = std::clamp(h, 1, grid_h - y);
		const int x2 = x + w;
		const int y2 = y + h;
		const size_t a = static_cast<size_t>(y) * integral_w + x;
		const size_t b = static_cast<size_t>(y) * integral_w + x2;
		const size_t c = static_cast<size_t>(y2) * integral_w + x;
		const size_t d = static_cast<size_t>(y2) * integral_w + x2;
		const double area = static_cast<double>(w * h);
		const double total = sum[d] - sum[b] - sum[c] + sum[a];
		const double total_sq = sum_sq[d] - sum_sq[b] - sum_sq[c] + sum_sq[a];
		const double sat_total = sat_sum[d] - sat_sum[b] - sat_sum[c] + sat_sum[a];
		const int neutral_total = neutral[d] - neutral[b] - neutral[c] + neutral[a];
		const int dark_total = dark[d] - dark[b] - dark[c] + dark[a];
		const int very_dark_total = very_dark[d] - very_dark[b] - very_dark[c] + very_dark[a];
		const int edge_total = edge_sum[d] - edge_sum[b] - edge_sum[c] + edge_sum[a];
		mean = total / area;
		variance = std::max(0.0, total_sq / area - mean * mean);
		neutral_frac = neutral_total / area;
		dark_frac = dark_total / area;
		very_dark_frac = very_dark_total / area;
		edge_frac = edge_total / area;
		sat_mean = sat_total / area;
	};

	double best_score = 0.0;
	int best_x = 0;
	int best_y = 0;
	int best_w = 0;
	int best_h = 0;
	const int min_x = static_cast<int>(grid_w * 0.08);
	const int max_x = static_cast<int>(grid_w * 0.92);
	const int min_y = static_cast<int>(grid_h * 0.18);
	const int max_y = static_cast<int>(grid_h * 0.86);

	for (int h = std::max(12, grid_h / 7); h <= std::max(22, grid_h / 2); h += 2) {
		for (int w = std::max(18, grid_w / 8); w <= std::max(62, grid_w * 2 / 3); w += 3) {
			const double aspect = static_cast<double>(w) / h;
			if (aspect < 0.9 || aspect > 2.7)
				continue;
			for (int y = min_y; y + h < max_y; y += 2) {
				for (int x = min_x; x + w < max_x; x += 2) {
					double mean = 0.0;
					double variance = 0.0;
					double neutral_frac = 0.0;
					double dark_frac = 0.0;
					double very_dark_frac = 0.0;
					double edge_frac = 0.0;
					double sat_mean = 0.0;
					rect_stats(x, y, w, h, mean, variance, neutral_frac, dark_frac, very_dark_frac,
						   edge_frac, sat_mean);

					if (neutral_frac < 0.35 || edge_frac < 0.14 || mean < 65.0 || sat_mean > 50.0)
						continue;
					const double center_x = (x + w * 0.5) / grid_w;
					const double center_y = (y + h * 0.5) / grid_h;
					const double area_frac = static_cast<double>(w * h) / (grid_w * grid_h);
					const double center_penalty =
						(std::abs(center_x - 0.5) + std::abs(center_y - 0.55)) * 30.0;
					const double top_penalty = std::max(0.0, 0.28 - center_y) * 80.0;
					const double aspect_penalty = std::abs(aspect - 1.45) * 6.0;
					const double score = neutral_frac * 55.0 + edge_frac * 170.0 +
							     dark_frac * 10.0 + very_dark_frac * 20.0 +
							     area_frac * 50.0 - sat_mean * 0.5 - center_penalty -
							     top_penalty - aspect_penalty;
					if (score > best_score) {
						best_score = score;
						best_x = x;
						best_y = y;
						best_w = w;
						best_h = h;
					}
				}
			}
		}
	}

	if (best_score <= 0.0)
		return result;

	const double anchor_center_x = (best_x + best_w * 0.5) / grid_w;
	const double anchor_center_y = (best_y + best_h * 0.5) / grid_h;
	const double crop_width = std::clamp(std::max(0.36, static_cast<double>(best_w) / grid_w * 2.2), 0.32, 0.52);
	const double crop_height = std::clamp(std::max(0.34, static_cast<double>(best_h) / grid_h * 2.8), 0.30, 0.50);
	const double left = std::clamp(anchor_center_x - crop_width * 0.5, 0.0, 1.0 - crop_width);
	const double top = std::clamp(anchor_center_y - crop_height * 0.60, 0.0, 1.0 - crop_height);
	const double right = left + crop_width;
	const double bottom = top + crop_height;
	result.x = left;
	result.y = top;
	result.width = right - left;
	result.height = bottom - top;
	result.valid = result.width >= 0.18 && result.height >= 0.14;
	return result;
}

static std::filesystem::path clapperboard_snapshot_folder(CameraTallyFilter *filter)
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return {};

	std::filesystem::path folder = current_recording_root_folder(config);
	if (folder.empty())
		return {};

	const std::string clip_name = latest_clip_name(filter);
	if (filter) {
		if (filter->auto_folder_enabled && filter->auto_folder_by_date)
			folder /= path_from_utf8(recording_date_folder_name());
		if (filter->auto_folder_enabled && filter->auto_folder_by_camera) {
			const std::string camera_folder = recording_camera_folder_name(clip_name);
			if (!camera_folder.empty())
				folder /= path_from_utf8(camera_folder);
		}
		if (filter->auto_folder_enabled && filter->auto_folder_by_card) {
			const std::string card_folder = recording_card_folder_name(clip_name);
			if (!card_folder.empty())
				folder /= path_from_utf8(card_folder);
		}
	}

	folder /= "Clapperboard";
	return folder;
}

static std::string clapperboard_snapshot_base_name(CameraTallyFilter *filter)
{
	std::string clip_name = latest_clip_name(filter);
	if (clip_name.empty()) {
		std::ostringstream fallback;
		fallback << "RecPilot_" << static_cast<long long>(std::time(nullptr));
		clip_name = fallback.str();
	}
	return sanitize_path_component(clip_name);
}

static std::vector<std::filesystem::path> clapperboard_snapshot_paths(CameraTallyFilter *filter, size_t count)
{
	std::vector<std::filesystem::path> paths;
	paths.reserve(count);

	std::filesystem::path folder;
	if (filter && filter->clapperboard_save_snapshots) {
		folder = clapperboard_snapshot_folder(filter);
		try {
			if (!folder.empty())
				std::filesystem::create_directories(folder);
		} catch (const std::exception &ex) {
			obs_log(LOG_WARNING, "RecPilot could not create Clapperboard snapshot folder: %s", ex.what());
			folder.clear();
		}
	}

	const std::string base = clapperboard_snapshot_base_name(filter);
	for (size_t i = 0; i < count; ++i) {
		std::ostringstream name;
		name << base << "_Clapperboard_" << std::setw(3) << std::setfill('0') << (i + 1) << ".jpg";
		paths.push_back(folder.empty() ? (std::filesystem::temp_directory_path() / name.str())
					       : (folder / name.str()));
	}
	return paths;
}

static std::string json_string_array(const std::vector<std::string> &values)
{
	std::string json = "[";
	for (size_t i = 0; i < values.size(); ++i) {
		if (i > 0)
			json += ",";
		json += "\"" + json_escape(values[i]) + "\"";
	}
	json += "]";
	return json;
}

static bool copy_normalized_rgba_region(const uint8_t *data, uint32_t linesize, uint32_t width, uint32_t height,
					double norm_x, double norm_y, double norm_w, double norm_h,
					std::vector<uint8_t> &crop, uint32_t &crop_width, uint32_t &crop_height)
{
	if (!data || width == 0 || height == 0)
		return false;

	const int x = clamp_int(static_cast<int>(norm_x * width), 0, static_cast<int>(width - 1));
	const int y = clamp_int(static_cast<int>(norm_y * height), 0, static_cast<int>(height - 1));
	const int right = clamp_int(static_cast<int>((norm_x + norm_w) * width), x + 1, static_cast<int>(width));
	const int bottom = clamp_int(static_cast<int>((norm_y + norm_h) * height), y + 1, static_cast<int>(height));

	crop_width = static_cast<uint32_t>(right - x);
	crop_height = static_cast<uint32_t>(bottom - y);
	if (crop_width < 4 || crop_height < 4)
		return false;

	crop.resize(static_cast<size_t>(crop_width) * crop_height * 4);
	for (uint32_t row = 0; row < crop_height; ++row) {
		const uint8_t *src = data + (static_cast<uint32_t>(y) + row) * linesize + static_cast<uint32_t>(x) * 4;
		uint8_t *dst = crop.data() + row * crop_width * 4;
		std::memcpy(dst, src, static_cast<size_t>(crop_width) * 4);
	}
	return true;
}

static void refresh_metadata_ocr_once(CameraTallyFilter *filter, const uint8_t *data, uint32_t linesize, uint32_t width,
				      uint32_t height)
{
	if (!filter || !data || width == 0 || height == 0)
		return;

	std::vector<MetadataField> fields = parse_metadata_fields_json(filter->metadata_fields_json);
	if (fields.empty())
		return;

	bool changed = false;
	for (MetadataField &field : fields) {
		if (field.name.empty())
			continue;

		std::vector<uint8_t> crop;
		uint32_t crop_width = 0;
		uint32_t crop_height = 0;
		if (!copy_normalized_rgba_region(data, linesize, width, height, field.x, field.y, field.width,
						 field.height, crop, crop_width, crop_height))
			continue;

		std::string text;
		try {
			text = compact_spaces(recognize_text_rgba_macos(crop, crop_width, crop_height));
		} catch (const std::exception &ex) {
			obs_log(LOG_WARNING, "RecPilot metadata OCR refresh failed: %s", ex.what());
		} catch (...) {
			obs_log(LOG_WARNING, "RecPilot metadata OCR refresh failed with an unknown error");
		}

		if (!text.empty() && field.value != text) {
			field.value = std::move(text);
			changed = true;
		}
	}

	if (!changed)
		return;

	const std::string json = metadata_fields_to_json(fields);
	filter->metadata_fields_json = json;
	if (!filter->context)
		return;

	obs_data_t *settings = obs_source_get_settings(filter->context);
	if (!settings)
		return;
	obs_data_set_string(settings, "metadata_fields_json", json.c_str());
	obs_source_update(filter->context, settings);
	obs_data_release(settings);
	obs_log(LOG_INFO, "RecPilot refreshed metadata OCR once before auto recording");
}

static void apply_auto_detect_clip_name(CameraTallyFilter *filter, const uint8_t *data, uint32_t linesize,
					uint32_t width, uint32_t height)
{
	if (!filter || !filter->auto_detect_clip_name_pending)
		return;

	std::vector<uint8_t> rgba;
	double x = 0.0;
	double y = 0.0;
	double box_width = 0.0;
	double box_height = 0.0;
	std::string text;
	const bool found = copy_frame_rgba(data, linesize, width, height, rgba) &&
			   recognize_clip_name_box_rgba_macos(rgba, width, height, x, y, box_width, box_height, text);

	obs_data_t *settings = obs_source_get_settings(filter->context);
	if (!settings)
		return;

	char detail[256];
	std::snprintf(detail, sizeof(detail), "%s%sformat %ux%u", found && !text.empty() ? text.c_str() : "",
		      found && !text.empty() ? ", " : "", width, height);

	if (found) {
		const double center_y = y + box_height * 0.5;
		const double horizontal_padding = std::max(3.0 / static_cast<double>(width), box_width * 0.04);
		const double vertical_padding = std::max(3.0 / static_cast<double>(height), box_height * 0.45);
		const double target_width =
			std::clamp(box_width + horizontal_padding * 2.0, 0.02, REC_PILOT_CLIP_NAME_WIDTH_MAX);
		const double target_height =
			std::clamp(box_height + vertical_padding * 2.0, 0.025, REC_PILOT_CLIP_NAME_HEIGHT_MAX);
		const double target_x = std::clamp(x - horizontal_padding, 0.0, 1.0 - target_width);
		const double target_y = std::clamp(center_y - target_height * 0.5, 0.0, 1.0 - target_height);

		obs_data_set_double(settings, "ocr_x", target_x);
		obs_data_set_double(settings, "ocr_y", target_y);
		obs_data_set_double(settings, "ocr_width", target_width);
		obs_data_set_double(settings, "ocr_height", target_height);
		obs_data_set_bool(settings, "show_overlay", true);
		obs_data_set_string(settings, "auto_detect_clip_name_result", "found");
		obs_data_set_string(settings, "auto_detect_clip_name_detail", detail);
		obs_log(LOG_INFO, "RecPilot auto-detected clip name box: %.3f %.3f %.3f %.3f (%s)", target_x, target_y,
			target_width, target_height, detail);
	} else {
		obs_data_set_string(settings, "auto_detect_clip_name_result", "not_found");
		obs_data_set_string(settings, "auto_detect_clip_name_detail", detail);
		obs_log(LOG_WARNING, "RecPilot auto-detect could not find a clip name (%s)", detail);
	}

	filter->auto_detect_clip_name_pending = false;
	filter->auto_detect_band_overlay_frames = 0;
	obs_data_set_bool(settings, "auto_detect_clip_name_pending", false);
	obs_source_update(filter->context, settings);
	obs_data_release(settings);
}

static bool analyse_rgba_region(CameraTallyFilter *filter, const uint8_t *data, uint32_t linesize, uint32_t width,
				uint32_t height)
{
	if (!data || width == 0 || height == 0)
		return false;

	const int cx = clamp_int(static_cast<int>(filter->center_x * width), 0, static_cast<int>(width - 1));
	const int cy = clamp_int(static_cast<int>(filter->center_y * height), 0, static_cast<int>(height - 1));
	const int radius = std::max(2, static_cast<int>(filter->radius * std::min(width, height)));
	const int radius_sq = radius * radius;

	int samples = 0;
	int red_hits = 0;

	for (int y = cy - radius; y <= cy + radius; y += 2) {
		if (y < 0 || y >= static_cast<int>(height))
			continue;

		for (int x = cx - radius; x <= cx + radius; x += 2) {
			if (x < 0 || x >= static_cast<int>(width))
				continue;
			const int dx = x - cx;
			const int dy = y - cy;
			if (dx * dx + dy * dy > radius_sq)
				continue;

			const uint8_t *p = data + y * linesize + x * 4;
			samples++;
			if (is_color_sample(filter, {p[0], p[1], p[2]}))
				red_hits++;
		}
	}

	if (samples == 0)
		return false;

	const double red_ratio = static_cast<double>(red_hits) / samples;
	return red_ratio >= filter->red_coverage;
}

static void set_detection_color(CameraTallyFilter *filter, Rgb rgb)
{
	if (!filter || !filter->context)
		return;

	filter->color_r = rgb.r;
	filter->color_g = rgb.g;
	filter->color_b = rgb.b;
	filter->color_pick_pending = false;

	obs_data_t *settings = obs_source_get_settings(filter->context);
	if (!settings)
		return;

	obs_data_set_int(settings, "color_r", rgb.r);
	obs_data_set_int(settings, "color_g", rgb.g);
	obs_data_set_int(settings, "color_b", rgb.b);
	obs_data_set_bool(settings, "color_pick_pending", false);
	obs_source_update(filter->context, settings);
	obs_data_release(settings);
}

static void apply_rgba_color_pick(CameraTallyFilter *filter, const uint8_t *data, uint32_t linesize, uint32_t width,
				  uint32_t height)
{
	if (!filter || !filter->color_pick_pending || !data || width == 0 || height == 0)
		return;

	const int x = clamp_int(static_cast<int>(filter->color_pick_x * width), 0, static_cast<int>(width - 1));
	const int y = clamp_int(static_cast<int>(filter->color_pick_y * height), 0, static_cast<int>(height - 1));
	const uint8_t *p = data + static_cast<uint32_t>(y) * linesize + static_cast<uint32_t>(x) * 4;
	set_detection_color(filter, {p[0], p[1], p[2]});
}

static bool copy_ocr_region(CameraTallyFilter *filter, const uint8_t *data, uint32_t linesize, uint32_t width,
			    uint32_t height, std::vector<uint8_t> &crop, uint32_t &crop_width, uint32_t &crop_height)
{
	if (!filter || !data || width == 0 || height == 0)
		return false;

	const int x = clamp_int(static_cast<int>(filter->ocr_x * width), 0, static_cast<int>(width - 1));
	const int y = clamp_int(static_cast<int>(filter->ocr_y * height), 0, static_cast<int>(height - 1));
	const int right = clamp_int(static_cast<int>((filter->ocr_x + filter->ocr_width) * width), x + 1,
				    static_cast<int>(width));
	const int bottom = clamp_int(static_cast<int>((filter->ocr_y + filter->ocr_height) * height), y + 1,
				     static_cast<int>(height));

	crop_width = static_cast<uint32_t>(right - x);
	crop_height = static_cast<uint32_t>(bottom - y);
	if (crop_width < 4 || crop_height < 4)
		return false;

	crop.resize(static_cast<size_t>(crop_width) * crop_height * 4);
	for (uint32_t row = 0; row < crop_height; ++row) {
		const uint8_t *src = data + (static_cast<uint32_t>(y) + row) * linesize + static_cast<uint32_t>(x) * 4;
		uint8_t *dst = crop.data() + row * crop_width * 4;
		memcpy(dst, src, static_cast<size_t>(crop_width) * 4);
	}

	return true;
}

static void schedule_ocr(CameraTallyFilter *filter, const uint8_t *data, uint32_t linesize, uint32_t width,
			 uint32_t height)
{
	if (!filter->ocr_enabled || !filter->ocr_state)
		return;

	filter->ocr_frame_count++;
	if (filter->ocr_frame_count < 45)
		return;
	filter->ocr_frame_count = 0;

	{
		std::lock_guard<std::mutex> lock(filter->ocr_state->mutex);
		if (filter->ocr_state->inflight)
			return;
		if (filter->ocr_state->worker.valid() &&
		    filter->ocr_state->worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			filter->ocr_state->worker.get();
		filter->ocr_state->inflight = true;
	}

	std::vector<uint8_t> crop;
	uint32_t crop_width = 0;
	uint32_t crop_height = 0;
	if (!copy_ocr_region(filter, data, linesize, width, height, crop, crop_width, crop_height)) {
		std::lock_guard<std::mutex> lock(filter->ocr_state->mutex);
		filter->ocr_state->inflight = false;
		return;
	}

	auto state = filter->ocr_state;
	const bool o_to_zero = filter->ocr_o_to_zero;
	const bool spaces_to_underscores = filter->ocr_spaces_to_underscores;
	const bool remove_spaces = filter->ocr_remove_spaces;
	const bool add_one = filter->ocr_add_one;
	filter->ocr_state->worker =
		std::async(std::launch::async, [state, crop = std::move(crop), crop_width, crop_height, o_to_zero,
						spaces_to_underscores, remove_spaces, add_one]() mutable {
			std::string text;
			try {
				text = normalize_clip_name(recognize_text_rgba_macos(crop, crop_width, crop_height),
							   o_to_zero, spaces_to_underscores, remove_spaces, add_one);
			} catch (const std::exception &ex) {
				obs_log(LOG_WARNING, "RecPilot OCR failed: %s", ex.what());
			} catch (...) {
				obs_log(LOG_WARNING, "RecPilot OCR failed with an unknown error");
			}
			std::lock_guard<std::mutex> lock(state->mutex);
			if (state->alive && !text.empty())
				state->latest_clip_name = std::move(text);
			state->inflight = false;
		});
}

static void schedule_metadata_ocr(CameraTallyFilter *filter, const uint8_t *data, uint32_t linesize, uint32_t width,
				  uint32_t height)
{
	if (!filter || !filter->metadata_ocr_state)
		return;

	bool has_ready_json = false;
	std::string ready_json;
	{
		std::lock_guard<std::mutex> lock(filter->metadata_ocr_state->mutex);
		if (filter->metadata_ocr_state->worker.valid() &&
		    filter->metadata_ocr_state->worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			filter->metadata_ocr_state->worker.get();
		if (filter->metadata_ocr_state->ready) {
			has_ready_json = true;
			ready_json = std::move(filter->metadata_ocr_state->pending_json);
			filter->metadata_ocr_state->pending_json.clear();
			filter->metadata_ocr_state->ready = false;
		}
	}

	if (has_ready_json && !ready_json.empty() && ready_json != filter->metadata_fields_json && filter->context) {
		obs_data_t *settings = obs_source_get_settings(filter->context);
		if (settings) {
			obs_data_set_string(settings, "metadata_fields_json", ready_json.c_str());
			obs_source_update(filter->context, settings);
			obs_data_release(settings);
		}
		filter->metadata_fields_json = ready_json;
	}

	const bool metadata_active = filter->metadata_overlay_visible;
	if (!metadata_active || !data || width == 0 || height == 0)
		return;

	filter->metadata_ocr_frame_count++;
	if (filter->metadata_ocr_frame_count < 45)
		return;
	filter->metadata_ocr_frame_count = 0;

	{
		std::lock_guard<std::mutex> lock(filter->metadata_ocr_state->mutex);
		if (filter->metadata_ocr_state->inflight)
			return;
		filter->metadata_ocr_state->inflight = true;
	}

	std::vector<MetadataField> fields = parse_metadata_fields_json(filter->metadata_fields_json);
	if (fields.empty()) {
		std::lock_guard<std::mutex> lock(filter->metadata_ocr_state->mutex);
		filter->metadata_ocr_state->inflight = false;
		return;
	}

	struct MetadataCrop {
		size_t index = 0;
		std::vector<uint8_t> pixels;
		uint32_t width = 0;
		uint32_t height = 0;
	};

	std::vector<MetadataCrop> crops;
	crops.reserve(fields.size());
	for (size_t i = 0; i < fields.size(); ++i) {
		MetadataCrop crop;
		crop.index = i;
		if (copy_normalized_rgba_region(data, linesize, width, height, fields[i].x, fields[i].y,
						fields[i].width, fields[i].height, crop.pixels, crop.width,
						crop.height)) {
			crops.push_back(std::move(crop));
		}
	}

	if (crops.empty()) {
		std::lock_guard<std::mutex> lock(filter->metadata_ocr_state->mutex);
		filter->metadata_ocr_state->inflight = false;
		return;
	}

	auto state = filter->metadata_ocr_state;
	const std::string previous_json = filter->metadata_fields_json;
	filter->metadata_ocr_state->worker = std::async(std::launch::async, [state, fields = std::move(fields),
									     crops = std::move(crops),
									     previous_json]() mutable {
		bool changed = false;
		for (const MetadataCrop &crop : crops) {
			std::string text;
			try {
				text = compact_spaces(recognize_text_rgba_macos(crop.pixels, crop.width, crop.height));
			} catch (const std::exception &ex) {
				obs_log(LOG_WARNING, "RecPilot metadata OCR failed: %s", ex.what());
			} catch (...) {
				obs_log(LOG_WARNING, "RecPilot metadata OCR failed with an unknown error");
			}

			if (!text.empty() && crop.index < fields.size() && fields[crop.index].value != text) {
				fields[crop.index].value = std::move(text);
				changed = true;
			}
		}

		std::lock_guard<std::mutex> lock(state->mutex);
		if (state->alive && changed) {
			const std::string json = metadata_fields_to_json(fields);
			if (json != previous_json) {
				state->pending_json = json;
				state->ready = true;
			}
		}
		state->inflight = false;
	});
}

static void schedule_clapperboard_ocr(CameraTallyFilter *filter, const uint8_t *data, uint32_t linesize, uint32_t width,
				      uint32_t height)
{
	if (!filter || !filter->clapperboard_state)
		return;

	bool has_ready_json = false;
	std::string ready_json;
	std::string ready_image_path;
	std::string ready_image_paths_json;
	std::string ready_crop_json;
	{
		std::lock_guard<std::mutex> lock(filter->clapperboard_state->mutex);
		if (filter->clapperboard_state->worker.valid() &&
		    filter->clapperboard_state->worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			filter->clapperboard_state->worker.get();
		if (filter->clapperboard_state->ready) {
			has_ready_json = true;
			ready_json = std::move(filter->clapperboard_state->pending_json);
			ready_image_path = std::move(filter->clapperboard_state->pending_image_path);
			ready_image_paths_json = std::move(filter->clapperboard_state->pending_image_paths_json);
			ready_crop_json = std::move(filter->clapperboard_state->pending_crop_json);
			filter->clapperboard_state->pending_json.clear();
			filter->clapperboard_state->pending_image_path.clear();
			filter->clapperboard_state->pending_image_paths_json.clear();
			filter->clapperboard_state->pending_crop_json.clear();
			filter->clapperboard_state->ready = false;
		}
	}

	if (has_ready_json && filter->context) {
		obs_data_t *settings = obs_source_get_settings(filter->context);
		if (settings) {
			obs_data_set_string(settings, "clapperboard_fields_json", ready_json.c_str());
			obs_data_set_string(settings, "clapperboard_image_path", ready_image_path.c_str());
			obs_data_set_string(settings, "clapperboard_image_paths_json", ready_image_paths_json.c_str());
			obs_data_set_string(settings, "clapperboard_crop_json", ready_crop_json.c_str());
			obs_data_set_string(settings, "clapperboard_result",
					    ready_json.empty() || ready_json == "[]" ? "not_found" : "found");
			obs_data_set_bool(settings, "clapperboard_pending", false);
			obs_source_update(filter->context, settings);
			obs_data_release(settings);
		}
		filter->clapperboard_pending = false;
	}

	if (!filter->clapperboard_pending || !data || width == 0 || height == 0)
		return;

	{
		std::lock_guard<std::mutex> lock(filter->clapperboard_state->mutex);
		if (filter->clapperboard_state->inflight)
			return;
	}

	const bool save_snapshot = filter->clapperboard_target == "clapperboard";
	if (save_snapshot) {
		if (filter->clapperboard_capture_tick == 0 || filter->clapperboard_capture_tick == 5 ||
		    filter->clapperboard_capture_tick == 10) {
			FrameSnapshot snapshot;
			snapshot.width = width;
			snapshot.height = height;
			if (copy_frame_rgba(data, linesize, width, height, snapshot.pixels))
				filter->clapperboard_snapshots.push_back(std::move(snapshot));
		}
		filter->clapperboard_capture_tick++;
		if (filter->clapperboard_snapshots.size() < 3 && filter->clapperboard_capture_tick <= 10)
			return;
	} else {
		FrameSnapshot snapshot;
		snapshot.width = width;
		snapshot.height = height;
		if (copy_frame_rgba(data, linesize, width, height, snapshot.pixels))
			filter->clapperboard_snapshots.push_back(std::move(snapshot));
	}

	if (filter->clapperboard_snapshots.empty())
		return;

	{
		std::lock_guard<std::mutex> lock(filter->clapperboard_state->mutex);
		if (filter->clapperboard_state->inflight)
			return;
		filter->clapperboard_state->inflight = true;
	}

	auto state = filter->clapperboard_state;
	const bool border_only = filter->clapperboard_border_only;
	std::vector<FrameSnapshot> snapshots = std::move(filter->clapperboard_snapshots);
	filter->clapperboard_snapshots.clear();
	filter->clapperboard_capture_tick = 0;
	const std::vector<std::filesystem::path> snapshot_paths =
		save_snapshot ? clapperboard_snapshot_paths(filter, snapshots.size())
			      : std::vector<std::filesystem::path>();
	filter->clapperboard_state->worker = std::async(std::launch::async, [state, snapshots = std::move(snapshots),
									     snapshot_paths, border_only,
									     save_snapshot]() mutable {
		std::string fields_json = "[]";
		std::string image_path;
		std::vector<std::string> image_paths;
		std::string crop_json;
		try {
			if (save_snapshot) {
				for (size_t i = 0; i < snapshots.size() && i < snapshot_paths.size(); ++i) {
					const std::string snapshot_path = path_to_utf8(snapshot_paths[i]);
					if (save_rgba_jpeg_macos(snapshots[i].pixels, snapshots[i].width,
								 snapshots[i].height, snapshot_path)) {
						image_paths.push_back(snapshot_path);
					}
				}
				if (!image_paths.empty())
					image_path = image_paths.front();
			}

			std::string combined_text;
			for (size_t i = 0; i < snapshots.size(); ++i) {
				if (save_snapshot && crop_json.empty()) {
					const NormalizedRect visual_crop = detect_clapperboard_crop_from_rgba(
						snapshots[i].pixels, snapshots[i].width, snapshots[i].height);
					crop_json = normalized_rect_json(visual_crop);
				}

				const std::string observations_json = recognize_text_observations_json_rgba_macos(
					snapshots[i].pixels, snapshots[i].width, snapshots[i].height);
				if (crop_json.empty())
					crop_json = clapperboard_crop_json_from_observations(observations_json);
				if (save_snapshot && crop_json.empty())
					crop_json = "{\"x\":0.15,\"y\":0.15,\"width\":0.70,\"height\":0.70}";
				std::string ocr_observations_json = observations_json;
				if (save_snapshot) {
					const NormalizedRect crop_rect = parse_normalized_rect_json(crop_json);
					if (crop_rect.valid) {
						std::vector<uint8_t> crop;
						uint32_t crop_width = 0;
						uint32_t crop_height = 0;
						if (copy_normalized_rgba_region(
							    snapshots[i].pixels.data(), snapshots[i].width * 4,
							    snapshots[i].width, snapshots[i].height, crop_rect.x,
							    crop_rect.y, crop_rect.width, crop_rect.height, crop,
							    crop_width, crop_height)) {
							ocr_observations_json =
								recognize_text_observations_json_rgba_macos(
									crop, crop_width, crop_height);
						}
					}
				}
				const std::string parsed =
					clapperboard_fields_json_from_observations(ocr_observations_json, border_only);
				if (!parsed.empty() && parsed != "[]" &&
				    (!save_snapshot || has_clapperboard_identity_fields(parsed))) {
					fields_json = parsed;
					break;
				}
				if (fields_json == "[]" && !parsed.empty() && parsed != "[]")
					fields_json = parsed;
			}
		} catch (const std::exception &ex) {
			obs_log(LOG_WARNING, "RecPilot CLAP OCR failed: %s", ex.what());
		} catch (...) {
			obs_log(LOG_WARNING, "RecPilot CLAP OCR failed with an unknown error");
		}

		std::lock_guard<std::mutex> lock(state->mutex);
		if (state->alive) {
			state->pending_json = std::move(fields_json);
			state->pending_image_path = std::move(image_path);
			state->pending_image_paths_json = json_string_array(image_paths);
			state->pending_crop_json = std::move(crop_json);
			state->ready = true;
		}
		state->inflight = false;
	});
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *filter = new CameraTallyFilter;
	filter->context = source;
	filter->registry_key = source;
	obs_data_set_bool(settings, "armed", obs_data_get_bool(settings, "arm_on_launch"));

	obs_enter_graphics();
	filter->render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	filter->circle = create_circle_vertex_buffer();
	filter->crosshair = create_crosshair_vertex_buffer();
	filter->arrows = create_arrow_vertex_buffer();
	filter->rectangle = create_rectangle_vertex_buffer();
	obs_leave_graphics();

	filter_update(filter, settings);
	{
		std::lock_guard<std::mutex> lock(g_filters_mutex);
		g_filters[filter->registry_key] = filter;
	}
	return filter;
}

static void filter_destroy(void *data)
{
	auto *filter = static_cast<CameraTallyFilter *>(data);
	{
		std::lock_guard<std::mutex> lock(g_filters_mutex);
		g_filters.erase(filter->registry_key);
	}
	if (filter->ocr_state) {
		{
			std::lock_guard<std::mutex> lock(filter->ocr_state->mutex);
			filter->ocr_state->alive = false;
		}
		if (filter->ocr_state->worker.valid())
			filter->ocr_state->worker.wait();
	}
	if (filter->metadata_ocr_state) {
		{
			std::lock_guard<std::mutex> lock(filter->metadata_ocr_state->mutex);
			filter->metadata_ocr_state->alive = false;
		}
		if (filter->metadata_ocr_state->worker.valid())
			filter->metadata_ocr_state->worker.wait();
	}
	if (filter->clapperboard_state) {
		{
			std::lock_guard<std::mutex> lock(filter->clapperboard_state->mutex);
			filter->clapperboard_state->alive = false;
		}
		if (filter->clapperboard_state->worker.valid())
			filter->clapperboard_state->worker.wait();
	}

	obs_enter_graphics();
	if (filter->stage)
		gs_stagesurface_destroy(filter->stage);
	if (filter->render)
		gs_texrender_destroy(filter->render);
	if (filter->circle)
		gs_vertexbuffer_destroy(filter->circle);
	if (filter->crosshair)
		gs_vertexbuffer_destroy(filter->crosshair);
	if (filter->arrows)
		gs_vertexbuffer_destroy(filter->arrows);
	if (filter->rectangle)
		gs_vertexbuffer_destroy(filter->rectangle);
	obs_leave_graphics();

	delete filter;
}

static void filter_update(void *data, obs_data_t *settings)
{
	auto *filter = static_cast<CameraTallyFilter *>(data);
	filter->armed = obs_data_get_bool(settings, "armed");
	filter->show_overlay = obs_data_get_bool(settings, "show_overlay");
	filter->start_on_red = true;
	obs_data_set_bool(settings, "start_on_red", true);
	filter->ocr_enabled = true;
	filter->ocr_o_to_zero = obs_data_get_bool(settings, "ocr_o_to_zero");
	filter->ocr_spaces_to_underscores = obs_data_get_bool(settings, "ocr_spaces_to_underscores");
	filter->ocr_remove_spaces = obs_data_get_bool(settings, "ocr_remove_spaces");
	filter->ocr_add_one = obs_data_get_bool(settings, "ocr_add_one");
	filter->auto_folder_enabled = obs_data_get_bool(settings, "auto_folder_enabled");
	filter->auto_folder_by_date = obs_data_get_bool(settings, "auto_folder_by_date");
	filter->auto_folder_by_camera = obs_data_get_bool(settings, "auto_folder_by_camera");
	filter->auto_folder_by_card = obs_data_get_bool(settings, "auto_folder_by_card");
	filter->metadata_overlay_visible = obs_data_get_bool(settings, "metadata_overlay_visible");
	filter->metadata_csv_per_card = obs_data_get_bool(settings, "metadata_csv_per_card");
	filter->clapperboard_save_snapshots = obs_data_get_bool(settings, "clapperboard_save_snapshots");
	const char *metadata_fields = obs_data_get_string(settings, "metadata_fields_json");
	filter->metadata_fields_json = metadata_fields ? metadata_fields : "[]";
	const char *clapperboard_target = obs_data_get_string(settings, "clapperboard_target");
	filter->clapperboard_target = clapperboard_target && *clapperboard_target ? clapperboard_target
										  : "clapperboard";
	if (filter->ocr_spaces_to_underscores && filter->ocr_remove_spaces)
		filter->ocr_remove_spaces = false;
	filter->color_r = static_cast<int>(obs_data_get_int(settings, "color_r"));
	filter->color_g = static_cast<int>(obs_data_get_int(settings, "color_g"));
	filter->color_b = static_cast<int>(obs_data_get_int(settings, "color_b"));
	if (filter->color_r == 0 && filter->color_g == 0 && filter->color_b == 0)
		filter->color_r = 255;
	filter->color_pick_pending = obs_data_get_bool(settings, "color_pick_pending");
	filter->auto_detect_center_pending = obs_data_get_bool(settings, "auto_detect_center_pending");
	if (filter->auto_detect_center_pending && !filter->last_auto_detect_center_pending)
		filter->auto_detect_band_overlay_frames = 150;
	filter->last_auto_detect_center_pending = filter->auto_detect_center_pending;
	filter->auto_detect_clip_name_pending = obs_data_get_bool(settings, "auto_detect_clip_name_pending");
	if (filter->auto_detect_clip_name_pending && !filter->last_auto_detect_clip_name_pending)
		filter->auto_detect_band_overlay_frames = 150;
	filter->last_auto_detect_clip_name_pending = filter->auto_detect_clip_name_pending;
	filter->clapperboard_pending = obs_data_get_bool(settings, "clapperboard_pending");
	filter->clapperboard_border_only = obs_data_get_bool(settings, "clapperboard_border_only");
	filter->color_pick_x = obs_data_get_double(settings, "color_pick_x");
	filter->color_pick_y = obs_data_get_double(settings, "color_pick_y");
	filter->center_x = obs_data_get_double(settings, "center_x");
	filter->center_y = obs_data_get_double(settings, "center_y");
	filter->radius = obs_data_get_double(settings, "radius");
	filter->ocr_x = obs_data_get_double(settings, "ocr_x");
	filter->ocr_y = obs_data_get_double(settings, "ocr_y");
	filter->ocr_width = std::clamp(obs_data_get_double(settings, "ocr_width"), 0.0, REC_PILOT_CLIP_NAME_WIDTH_MAX);
	filter->ocr_height =
		std::clamp(obs_data_get_double(settings, "ocr_height"), 0.0, REC_PILOT_CLIP_NAME_HEIGHT_MAX);
	filter->red_threshold = obs_data_get_double(settings, "red_threshold");
	filter->red_coverage = obs_data_get_double(settings, "red_coverage");
	filter->start_frames = static_cast<int>(obs_data_get_int(settings, "start_frames"));
	filter->stop_frames = static_cast<int>(obs_data_get_int(settings, "stop_frames"));
}

static void set_detection_center(CameraTallyFilter *filter, double x, double y)
{
	if (!filter || !filter->context)
		return;

	x = std::clamp(x, 0.0, 1.0);
	y = std::clamp(y, 0.0, 1.0);

	obs_data_t *settings = obs_source_get_settings(filter->context);
	if (!settings)
		return;

	obs_data_set_double(settings, "center_x", x);
	obs_data_set_double(settings, "center_y", y);
	obs_source_update(filter->context, settings);
	obs_data_release(settings);
}

static void filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "armed", false);
	obs_data_set_default_bool(settings, "arm_on_launch", false);
	obs_data_set_default_bool(settings, "show_overlay", true);
	obs_data_set_default_bool(settings, "start_on_red", true);
	obs_data_set_default_bool(settings, "ocr_enabled", true);
	obs_data_set_default_bool(settings, "ocr_o_to_zero", true);
	obs_data_set_default_bool(settings, "ocr_spaces_to_underscores", true);
	obs_data_set_default_bool(settings, "ocr_remove_spaces", false);
	obs_data_set_default_bool(settings, "ocr_add_one", false);
	obs_data_set_default_bool(settings, "auto_folder_enabled", false);
	obs_data_set_default_bool(settings, "auto_folder_by_date", true);
	obs_data_set_default_bool(settings, "auto_folder_by_camera", true);
	obs_data_set_default_bool(settings, "auto_folder_by_card", true);
	obs_data_set_default_bool(settings, "metadata_overlay_visible", false);
	obs_data_set_default_bool(settings, "metadata_csv_per_card", false);
	obs_data_set_default_bool(settings, "clapperboard_save_snapshots", true);
	obs_data_set_default_string(settings, "metadata_fields_json", "[]");
	obs_data_set_default_bool(settings, "coffee_thanks_confirmed", false);
	obs_data_set_default_string(settings, "coffee_reminder_day", "");
	obs_data_set_default_int(settings, "coffee_reminder_count", 0);
	obs_data_set_default_int(settings, "color_r", 255);
	obs_data_set_default_int(settings, "color_g", 0);
	obs_data_set_default_int(settings, "color_b", 0);
	obs_data_set_default_bool(settings, "color_pick_pending", false);
	obs_data_set_default_bool(settings, "auto_detect_center_pending", false);
	obs_data_set_default_string(settings, "auto_detect_center_result", "");
	obs_data_set_default_string(settings, "auto_detect_center_detail", "");
	obs_data_set_default_bool(settings, "auto_detect_clip_name_pending", false);
	obs_data_set_default_string(settings, "auto_detect_clip_name_result", "");
	obs_data_set_default_string(settings, "auto_detect_clip_name_detail", "");
	obs_data_set_default_bool(settings, "clapperboard_pending", false);
	obs_data_set_default_bool(settings, "clapperboard_border_only", false);
	obs_data_set_default_string(settings, "clapperboard_target", "clapperboard");
	obs_data_set_default_string(settings, "clapperboard_result", "");
	obs_data_set_default_string(settings, "clapperboard_fields_json", "[]");
	obs_data_set_default_string(settings, "clapperboard_image_path", "");
	obs_data_set_default_string(settings, "clapperboard_image_paths_json", "[]");
	obs_data_set_default_string(settings, "clapperboard_crop_json", "");
	obs_data_set_default_double(settings, "color_pick_x", 0.5);
	obs_data_set_default_double(settings, "color_pick_y", 0.5);
	obs_data_set_default_double(settings, "center_x", 0.08);
	obs_data_set_default_double(settings, "center_y", 0.08);
	obs_data_set_default_double(settings, "radius", 0.025);
	obs_data_set_default_double(settings, "ocr_x", 0.20);
	obs_data_set_default_double(settings, "ocr_y", 0.06);
	obs_data_set_default_double(settings, "ocr_width", 0.14);
	obs_data_set_default_double(settings, "ocr_height", 0.08);
	obs_data_set_default_double(settings, "red_threshold", 0.48);
	obs_data_set_default_double(settings, "red_coverage", 0.02);
	obs_data_set_default_int(settings, "start_frames", 1);
	obs_data_set_default_int(settings, "stop_frames", 12);
}

static obs_properties_t *filter_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	return props;
}

static obs_source_frame *filter_video(void *data, obs_source_frame *frame)
{
	auto *filter = static_cast<CameraTallyFilter *>(data);
	apply_auto_detect_center(filter, frame);
	if (filter->start_on_red)
		apply_color_state(filter, analyse_frame_red(filter, frame));
	return frame;
}

static void filter_mouse_click(void *data, const obs_mouse_event *event, int32_t type, bool mouse_up,
			       uint32_t click_count)
{
	auto *filter = static_cast<CameraTallyFilter *>(data);
	if (!filter || !event || mouse_up || type != MOUSE_LEFT || !filter->picking_center)
		return;

	obs_source_t *target = obs_filter_get_target(filter->context);
	const uint32_t width = target ? obs_source_get_base_width(target) : 0;
	const uint32_t height = target ? obs_source_get_base_height(target) : 0;
	if (width == 0 || height == 0)
		return;

	filter->picking_center = false;
	set_detection_center(filter, static_cast<double>(event->x) / static_cast<double>(width),
			     static_cast<double>(event->y) / static_cast<double>(height));
	obs_log(LOG_INFO, "RecPilot detection center selected with cursor: %.3f %.3f", filter->center_x,
		filter->center_y);

	UNUSED_PARAMETER(click_count);
}

static void draw_overlay(CameraTallyFilter *filter, uint32_t width, uint32_t height)
{
	const bool draw_metadata = filter->metadata_overlay_visible;
	const bool draw_main_overlay = filter->show_overlay;
	if ((!draw_main_overlay && !draw_metadata) || !filter->rectangle || width == 0 || height == 0)
		return;
	if (draw_main_overlay && (!filter->circle || !filter->crosshair || !filter->arrows))
		return;
	if (obs_frontend_recording_active())
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	constexpr float center_mark = 5.0f;
	auto draw_center_mark = [&](float x, float y, uint32_t overlay_color, float grow) {
		gs_render_start(true);
		gs_vertex2f(x - center_mark - grow, y);
		gs_vertex2f(x + center_mark + grow, y);
		gs_vertex2f(x, y - center_mark - grow);
		gs_vertex2f(x, y + center_mark + grow);
		gs_vertbuffer_t *mark = gs_render_save();
		if (!mark)
			return;

		gs_effect_set_color(color, overlay_color);
		gs_load_vertexbuffer(mark);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw(GS_LINES, 0, 0);
		gs_vertexbuffer_destroy(mark);
	};

	auto draw_dashed_rect = [&](float x, float y, float w, float h, uint32_t overlay_color, float grow) {
		x -= grow;
		y -= grow;
		w += grow * 2.0f;
		h += grow * 2.0f;
		const float dash = std::max(8.0f, std::min(w, h) * 0.18f);
		const float gap = dash * 0.65f;
		const float step = dash + gap;

		auto add_dash_line = [&](float x1, float y1, float x2, float y2) {
			const float dx = x2 - x1;
			const float dy = y2 - y1;
			const float len = std::sqrt(dx * dx + dy * dy);
			if (len <= 0.0f)
				return;
			const float ux = dx / len;
			const float uy = dy / len;
			for (float start = 0.0f; start < len; start += step) {
				const float end = std::min(start + dash, len);
				gs_vertex2f(x1 + ux * start, y1 + uy * start);
				gs_vertex2f(x1 + ux * end, y1 + uy * end);
			}
		};

		gs_render_start(true);
		add_dash_line(x, y, x + w, y);
		add_dash_line(x + w, y, x + w, y + h);
		add_dash_line(x + w, y + h, x, y + h);
		add_dash_line(x, y + h, x, y);
		gs_vertbuffer_t *rect = gs_render_save();
		if (!rect)
			return;

		gs_effect_set_color(color, overlay_color);
		gs_load_vertexbuffer(rect);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw(GS_LINES, 0, 0);
		gs_vertexbuffer_destroy(rect);
	};

	if (draw_main_overlay) {
		const float cx = static_cast<float>(filter->center_x * width);
		const float cy = static_cast<float>(filter->center_y * height);
		const float radius = std::max(2.0f, static_cast<float>(filter->radius * std::min(width, height)));

		auto draw_search_band = [&](float y, float h, uint32_t overlay_color, float grow) {
			matrix4 transform;
			matrix4_identity(&transform);
			transform.x.x = static_cast<float>(width) + grow * 2.0f;
			transform.y.y = h + grow * 2.0f;
			transform.t.x = -grow;
			transform.t.y = y - grow;

			gs_matrix_push();
			gs_matrix_mul(&transform);
			gs_effect_set_color(color, overlay_color);
			gs_load_vertexbuffer(filter->rectangle);
			while (gs_effect_loop(solid, "Solid"))
				gs_draw(GS_LINESTRIP, 0, 0);
			gs_matrix_pop();
		};

		if (filter->auto_detect_center_pending || filter->auto_detect_clip_name_pending ||
		    filter->auto_detect_band_overlay_frames > 0) {
			const float band_h = static_cast<float>(height) * 0.15f;
			for (float grow : {5.0f, 4.0f, 3.0f}) {
				draw_search_band(0.0f, band_h, 0xFF000000, grow);
				draw_search_band(static_cast<float>(height) - band_h, band_h, 0xFF000000, grow);
			}
			for (float grow : {2.0f, 1.0f, 0.0f}) {
				draw_search_band(0.0f, band_h, 0x8038A7FF, grow);
				draw_search_band(static_cast<float>(height) - band_h, band_h, 0x8038A7FF, grow);
			}
			if (!filter->auto_detect_center_pending && !filter->auto_detect_clip_name_pending)
				filter->auto_detect_band_overlay_frames--;
		}

		auto draw_buffer = [&](gs_vertbuffer_t *buffer, gs_draw_mode draw_mode, uint32_t overlay_color,
				       float scale) {
			matrix4 transform;
			matrix4_identity(&transform);
			transform.x.x = radius * scale;
			transform.y.y = radius * scale;
			transform.t.x = cx;
			transform.t.y = cy;

			gs_matrix_push();
			gs_matrix_mul(&transform);
			gs_effect_set_color(color, overlay_color);
			gs_load_vertexbuffer(buffer);
			while (gs_effect_loop(solid, "Solid"))
				gs_draw(draw_mode, 0, 0);
			gs_matrix_pop();
		};

		for (float scale : {1.10f, 1.07f, 1.04f, 1.01f})
			draw_buffer(filter->circle, GS_LINESTRIP, 0xFF000000, scale);
		for (float scale : {1.00f, 0.97f, 0.94f, 0.91f})
			draw_buffer(filter->circle, GS_LINESTRIP, 0xFF00FFFF, scale);
		for (float scale : {0.84f, 0.81f})
			draw_buffer(filter->circle, GS_LINESTRIP, 0xFFFFFFFF, scale);

		for (float scale : {1.03f, 1.00f, 0.97f})
			draw_buffer(filter->crosshair, GS_LINES, 0xFFFF2020, scale);
		for (float scale : {1.85f, 1.78f, 1.71f})
			draw_buffer(filter->arrows, GS_LINES, 0xFF000000, scale);
		for (float scale : {1.72f, 1.65f, 1.58f})
			draw_buffer(filter->arrows, GS_LINES, 0xFFFFFFFF, scale);

		for (float grow : {3.0f, 2.0f, 1.0f})
			draw_center_mark(cx, cy, 0xFF000000, grow);
		draw_center_mark(cx, cy, 0xFFFFFFFF, 0.0f);

		const float rect_x = static_cast<float>(filter->ocr_x * width);
		const float rect_y = static_cast<float>(filter->ocr_y * height);
		const float rect_w = static_cast<float>(filter->ocr_width * width);
		const float rect_h = static_cast<float>(filter->ocr_height * height);
		const float rect_cx = rect_x + rect_w * 0.5f;
		const float rect_cy = rect_y + rect_h * 0.5f;

		auto draw_rectangle = [&](uint32_t overlay_color, float grow) {
			matrix4 transform;
			matrix4_identity(&transform);
			transform.x.x = rect_w + grow * 2.0f;
			transform.y.y = rect_h + grow * 2.0f;
			transform.t.x = rect_x - grow;
			transform.t.y = rect_y - grow;

			gs_matrix_push();
			gs_matrix_mul(&transform);
			gs_effect_set_color(color, overlay_color);
			gs_load_vertexbuffer(filter->rectangle);
			while (gs_effect_loop(solid, "Solid"))
				gs_draw(GS_LINESTRIP, 0, 0);
			gs_matrix_pop();
		};

		for (float grow : {4.0f, 3.0f, 2.0f})
			draw_rectangle(0xFF000000, grow);
		for (float grow : {1.0f, 0.0f})
			draw_rectangle(0xFF18D4FF, grow);
		for (float grow : {3.0f, 2.0f, 1.0f})
			draw_center_mark(rect_cx, rect_cy, 0xFF000000, grow);
		draw_center_mark(rect_cx, rect_cy, 0xFFFFFFFF, 0.0f);
	}

	if (draw_metadata) {
		const std::vector<MetadataField> fields = parse_metadata_fields_json(filter->metadata_fields_json);
		for (const MetadataField &field : fields) {
			const float x = static_cast<float>(field.x * width);
			const float y = static_cast<float>(field.y * height);
			const float w = static_cast<float>(field.width * width);
			const float h = static_cast<float>(field.height * height);
			if (w <= 1.0f || h <= 1.0f)
				continue;
			for (float grow : {3.0f, 2.0f})
				draw_dashed_rect(x, y, w, h, 0xD0000000, grow);
			for (float grow : {1.0f, 0.0f})
				draw_dashed_rect(x, y, w, h, 0xFF32E875, grow);
		}
	}

	gs_blend_state_pop();
}

static void filter_render(void *data, gs_effect_t *effect)
{
	auto *filter = static_cast<CameraTallyFilter *>(data);
	obs_source_t *target = obs_filter_get_target(filter->context);
	obs_source_t *parent = obs_filter_get_parent(filter->context);

	if (!filter->render || !target) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	const uint32_t width = target ? obs_source_get_base_width(target) : 0;
	const uint32_t height = target ? obs_source_get_base_height(target) : 0;
	if (width == 0 || height == 0) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	gs_texrender_reset(filter->render);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(filter->render, width, height)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);

		const uint32_t parent_flags = obs_source_get_output_flags(target);
		const bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		const bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		if (parent && target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_texrender_end(filter->render);
	}

	gs_blend_state_pop();

	gs_texture_t *texture = gs_texrender_get_texture(filter->render);
	if (!texture) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	if (!filter->stage || filter->stage_width != width || filter->stage_height != height) {
		if (filter->stage)
			gs_stagesurface_destroy(filter->stage);
		filter->stage = gs_stagesurface_create(width, height, GS_RGBA);
		filter->stage_width = width;
		filter->stage_height = height;
	}

	if (filter->stage) {
		gs_stage_texture(filter->stage, texture);

		uint8_t *pixels = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(filter->stage, &pixels, &linesize)) {
			if (filter->start_on_red)
				apply_color_state(filter, analyse_rgba_region(filter, pixels, linesize, width, height),
						  pixels, linesize, width, height);
			apply_rgba_color_pick(filter, pixels, linesize, width, height);
			apply_auto_detect_clip_name(filter, pixels, linesize, width, height);
			schedule_ocr(filter, pixels, linesize, width, height);
			schedule_metadata_ocr(filter, pixels, linesize, width, height);
			schedule_clapperboard_ocr(filter, pixels, linesize, width, height);
			gs_stagesurface_unmap(filter->stage);
		}
	}

	gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(default_effect, "image"), texture);
	while (gs_effect_loop(default_effect, "Draw"))
		gs_draw_sprite(texture, 0, width, height);

	draw_overlay(filter, width, height);

	UNUSED_PARAMETER(effect);
}

} // namespace

std::string camera_tally_filter_get_clip_name(obs_source_t *source)
{
	if (!source)
		return {};

	std::lock_guard<std::mutex> lock(g_filters_mutex);
	auto it = g_filters.find(source);
	if (it == g_filters.end())
		return {};
	return latest_clip_name(it->second);
}

void camera_tally_start_recording_with_clip_name(obs_source_t *source)
{
	std::string clip_name;
	CameraTallyFilter *filter = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_filters_mutex);
		auto it = g_filters.find(source);
		if (it != g_filters.end()) {
			filter = it->second;
			clip_name = latest_clip_name(it->second);
		}
	}

	request_recording_start(filter, clip_name);
}

void camera_tally_stop_recording(obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	request_recording_stop();
}

bool camera_tally_filter_begin_center_pick(obs_source_t *source)
{
	if (!source)
		return false;

	std::lock_guard<std::mutex> lock(g_filters_mutex);
	auto it = g_filters.find(source);
	if (it == g_filters.end())
		return false;

	it->second->picking_center = true;
	return true;
}

static void camera_tally_request_clap_ocr(obs_source_t *source, const char *target, bool border_only)
{
	if (!source)
		return;

	std::lock_guard<std::mutex> lock(g_filters_mutex);
	auto it = g_filters.find(source);
	if (it == g_filters.end() || !it->second || !it->second->context)
		return;

	auto *filter = it->second;
	filter->clapperboard_pending = true;
	filter->clapperboard_border_only = border_only;
	filter->clapperboard_target = target ? target : "clapperboard";
	filter->clapperboard_capture_tick = 0;
	filter->clapperboard_snapshots.clear();
	obs_data_t *settings = obs_source_get_settings(filter->context);
	if (!settings)
		return;
	obs_data_set_bool(settings, "clapperboard_pending", true);
	obs_data_set_bool(settings, "clapperboard_border_only", border_only);
	obs_data_set_string(settings, "clapperboard_target", filter->clapperboard_target.c_str());
	obs_data_set_string(settings, "clapperboard_result", "waiting");
	obs_data_set_string(settings, "clapperboard_fields_json", "[]");
	obs_data_set_string(settings, "clapperboard_image_path", "");
	obs_data_set_string(settings, "clapperboard_image_paths_json", "[]");
	obs_data_set_string(settings, "clapperboard_crop_json", "");
	obs_source_update(filter->context, settings);
	obs_data_release(settings);
}

void camera_tally_request_clapperboard(obs_source_t *source)
{
	camera_tally_request_clap_ocr(source, "clapperboard", false);
}

void camera_tally_request_metadata_auto_detect(obs_source_t *source)
{
	camera_tally_request_clap_ocr(source, "metadata", true);
}

void register_camera_tally_filter()
{
	obs_source_info info = {};
	info.id = "camera_tally_rec_trigger_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB | OBS_SOURCE_INTERACTION;
	info.get_name = filter_name;
	info.create = filter_create;
	info.destroy = filter_destroy;
	info.update = filter_update;
	info.get_defaults = filter_defaults;
	info.get_properties = filter_properties;
	info.filter_video = filter_video;
	info.mouse_click = filter_mouse_click;
	info.video_render = filter_render;

	obs_register_source(&info);
}

void camera_tally_register_frontend_events()
{
	obs_frontend_add_event_callback(frontend_event, nullptr);
}

void camera_tally_unregister_frontend_events()
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
}

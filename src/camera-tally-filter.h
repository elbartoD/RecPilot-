#pragma once

#include <string>

struct obs_source;
typedef struct obs_source obs_source_t;

inline constexpr double REC_PILOT_CLIP_NAME_WIDTH_MAX = 0.25;
inline constexpr double REC_PILOT_CLIP_NAME_HEIGHT_MAX = 0.15;

void register_camera_tally_filter();
void camera_tally_register_frontend_events();
void camera_tally_unregister_frontend_events();
std::string camera_tally_filter_get_clip_name(obs_source_t *source);
void camera_tally_start_recording_with_clip_name(obs_source_t *source);
void camera_tally_stop_recording(obs_source_t *source);
bool camera_tally_filter_begin_center_pick(obs_source_t *source);
void camera_tally_request_clapperboard(obs_source_t *source);
void camera_tally_request_metadata_auto_detect(obs_source_t *source);

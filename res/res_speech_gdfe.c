/*
 * res_speech_gdfe -- an Asterisk speech driver for Google DialogFlow for Enterprise
 * 
 * Copyright (C) 2018, USAN, Inc.
 * 
 * Daniel Collins <daniel.collins@usan.com>
 * 
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source asterisk tree.
 *
 */

/*** MODULEINFO
    <depend>res_speech</depend>
	<depend>dfegrpc</depend>
 ***/

#include <asterisk.h>
#include <asterisk/module.h>
#include <asterisk/lock.h>
#include <asterisk/linkedlists.h>
#include <asterisk/cli.h>
#include <asterisk/term.h>
#include <asterisk/speech.h>

#ifdef RAII_VAR
#define ASTERISK_13_OR_LATER
#endif

#ifdef ASTERISK_13_OR_LATER
#include <asterisk/format.h>
#include <asterisk/format_cache.h>
#include <asterisk/codec.h>
#include <asterisk/format_cap.h>
#else
#include <asterisk/frame.h>
#include <asterisk/astobj2.h>
#endif

#include <asterisk/chanvars.h>
#include <asterisk/pbx.h>
#include <asterisk/config.h>
#include <asterisk/ulaw.h>

#include <libdfegrpc.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef ASTERISK_13_OR_LATER
#include <jansson.h>
#endif

#define GDF_PROP_SESSION_ID_NAME	"session_id"
#define GDF_PROP_ALTERNATE_SESSION_NAME "name"
#define GDF_PROP_PROJECT_ID_NAME	"project_id"
#define GDF_PROP_LANGUAGE_NAME		"language"
#define GDF_PROP_LOG_CONTEXT		"log_context"
#define GDF_PROP_ALTERNATE_LOG_CONTEXT	"logContext"
#define GDF_PROP_APPLICATION_CONTEXT	"application"
#define VAD_PROP_VOICE_THRESHOLD	"voice_threshold"
#define VAD_PROP_VOICE_DURATION		"voice_duration"
#define VAD_PROP_SILENCE_DURATION	"silence_duration"

enum VAD_STATE {
	VAD_STATE_START,
	VAD_STATE_SPEAK,
	VAD_STATE_SILENT
};

struct gdf_pvt {
	ast_mutex_t lock;
	struct dialogflow_session *session;
	
	enum VAD_STATE vad_state;
	int vad_state_duration; /* ms */
	int vad_change_duration; /* ms -- cumulative time of "not current state" audio */

	int voice_threshold; /* 0 - (2^16 - 1) */
	int voice_minimum_duration; /* ms */
	int silence_minimum_duration; /* ms */

	int call_log_open_already_attempted;
	FILE *call_log_file_handle;

	int utterance_counter;

	int utterance_preendpointer_recording_open_already_attempted;
	FILE *utterance_preendpointer_recording_file_handle;
	int utterance_postendpointer_recording_open_already_attempted;
	FILE *utterance_postendpointer_recording_file_handle;
	
	AST_DECLARE_STRING_FIELDS(
		AST_STRING_FIELD(logical_agent_name);
		AST_STRING_FIELD(project_id);
		AST_STRING_FIELD(session_id);
		AST_STRING_FIELD(service_key);
		AST_STRING_FIELD(endpoint);
		AST_STRING_FIELD(event);
		AST_STRING_FIELD(language);
		AST_STRING_FIELD(lastAudioResponse);

		AST_STRING_FIELD(call_log_path);
		AST_STRING_FIELD(call_log_file_basename);
		AST_STRING_FIELD(call_logging_application_name);
		AST_STRING_FIELD(call_logging_context);
	);
};

struct ao2_container *config;

struct gdf_logical_agent {
	const char *name;
	const char *project_id;
	const char *service_key;
	char endpoint[0];
};

struct gdf_config {
	int vad_voice_threshold;
	int vad_voice_minimum_duration;
	int vad_silence_minimum_duration;

	int enable_call_logs;
	int enable_preendpointer_recordings;
	int enable_postendpointer_recordings;

	struct ao2_container *logical_agents;

	AST_DECLARE_STRING_FIELDS(
		AST_STRING_FIELD(service_key);
		AST_STRING_FIELD(endpoint);
		AST_STRING_FIELD(call_log_location);
	);
};

enum gdf_call_log_type {
	CALL_LOG_TYPE_SESSION,
	CALL_LOG_TYPE_ENDPOINTER,
	CALL_LOG_TYPE_DIALOGFLOW
};

static struct gdf_config *gdf_get_config(void);
static struct gdf_logical_agent *get_logical_agent_by_name(struct gdf_config *config, const char *name);
static void gdf_log_call_event(struct gdf_pvt *pvt, enum gdf_call_log_type type, const char *event, size_t log_data_size, const struct dialogflow_log_data *log_data);
#define gdf_log_call_event_only(pvt, type, event)       gdf_log_call_event(pvt, type, event, 0, NULL)

static struct ast_str *build_log_related_filename_to_thread_local_str(struct gdf_pvt *pvt, int include_utterance_counter, const char *type, const char *extension);

#ifdef ASTERISK_13_OR_LATER
typedef struct ast_format *local_ast_format_t;
#else
typedef int local_ast_format_t;
#endif

static int gdf_create(struct ast_speech *speech, local_ast_format_t format)
{
	struct gdf_pvt *pvt;
	struct gdf_config *cfg;
	char session_id[32];
	size_t sidlen = sizeof(session_id);
	char *sid = session_id;

	pvt = ast_calloc_with_stringfields(1, struct gdf_pvt, 252);
	if (!pvt) {
		ast_log(LOG_WARNING, "Error allocating memory for GDF private structure\n");
		return -1;
	}

	ast_mutex_init(&pvt->lock);

	ast_build_string(&sid, &sidlen, "%p", pvt);

	cfg = gdf_get_config();

	pvt->session = df_create_session(pvt);
	df_set_auth_key(pvt->session, cfg->service_key);
	df_set_endpoint(pvt->session, cfg->endpoint);

	if (!pvt->session) {
		ast_log(LOG_WARNING, "Error creating session for GDF\n");
		ao2_t_ref(cfg, -1, "done with creating session");
		ast_free(pvt);
		return -1;
	}

	/* temporarily set _something_ */
	df_set_session_id(pvt->session, session_id);
	ast_string_field_set(pvt, session_id, session_id);
	pvt->voice_threshold = cfg->vad_voice_threshold;
	pvt->voice_minimum_duration = cfg->vad_voice_minimum_duration;
	pvt->silence_minimum_duration = cfg->vad_silence_minimum_duration;
	ast_string_field_set(pvt, call_logging_application_name, "unknown");

	ast_mutex_lock(&speech->lock);
	speech->state = AST_SPEECH_STATE_NOT_READY;
	speech->data = pvt;
	ast_mutex_unlock(&speech->lock);

	ao2_t_ref(cfg, -1, "done with creating session");

	return 0;
}

static int gdf_destroy(struct ast_speech *speech)
{
	struct gdf_pvt *pvt = speech->data;

	if (speech->state == AST_SPEECH_STATE_READY) {
		df_stop_recognition(pvt->session);
	}

	if (!ast_strlen_zero(pvt->lastAudioResponse)) {
		unlink(pvt->lastAudioResponse);
	}

	df_close_session(pvt->session);

	if (pvt->call_log_file_handle != NULL) {
		fclose(pvt->call_log_file_handle);
	}

	ast_string_field_free_memory(pvt);
	ast_mutex_destroy(&pvt->lock);
	return 0;
}

static int gdf_load(struct ast_speech *speech, const char *grammar_name, const char *grammar)
{
	return 0;
}

static int gdf_unload(struct ast_speech *speech, const char *grammar_name)
{
	return 0;
}

#define EVENT_COLON_LEN	6
#define EVENT_COLON		"event:"
static int is_grammar_old_style_event(const char *grammar_name)
{
	return !strncasecmp(grammar_name, EVENT_COLON, EVENT_COLON_LEN);
}

static void activate_old_style_event(struct gdf_pvt *pvt, const char *grammar_name)
{
	const char *name = grammar_name + EVENT_COLON_LEN;
	ast_log(LOG_DEBUG, "Activating event %s on %s\n", name, pvt->session_id);
	ast_mutex_lock(&pvt->lock);
	ast_string_field_set(pvt, event, name);
	ast_mutex_unlock(&pvt->lock);
}

#define BUILTIN_COLON_GRAMMAR_SLASH_LEN	16
#define BUILTIN_COLON_GRAMMAR_SLASH		"builtin:grammar/"
static int is_grammar_new_style_format(const char *grammar_name)
{
	return !strncasecmp(grammar_name, BUILTIN_COLON_GRAMMAR_SLASH, BUILTIN_COLON_GRAMMAR_SLASH_LEN);
}

static void activate_agent_for_name(struct gdf_pvt *pvt, const char *name, size_t name_len, const char *event)
{
	struct gdf_config *config;

	ast_mutex_lock(&pvt->lock);
	ast_string_field_build(pvt, logical_agent_name, "%.*s", (int) name_len, name);
	ast_mutex_unlock(&pvt->lock);

	config = gdf_get_config();
	if (config) {
		struct gdf_logical_agent *logical_agent_map = get_logical_agent_by_name(config, pvt->logical_agent_name);
		ast_mutex_lock(&pvt->lock);
		ast_string_field_set(pvt, project_id, S_OR(logical_agent_map ? logical_agent_map->project_id : NULL, pvt->logical_agent_name));
		ast_string_field_set(pvt, service_key, S_OR(logical_agent_map ? logical_agent_map->service_key : NULL, config->service_key));
		ast_string_field_set(pvt, endpoint, S_OR(logical_agent_map ? logical_agent_map->endpoint : NULL, config->endpoint));
		ast_string_field_set(pvt, event, event);
		ast_mutex_unlock(&pvt->lock);
		if (logical_agent_map) {
			ao2_ref(logical_agent_map, -1);
		}
		ao2_ref(config, -1);
	} else {
		ast_mutex_lock(&pvt->lock);
		ast_string_field_set(pvt, project_id, pvt->logical_agent_name);
		ast_string_field_set(pvt, event, event);
		ast_mutex_unlock(&pvt->lock);
	}
	df_set_project_id(pvt->session, pvt->project_id);
	df_set_endpoint(pvt->session, pvt->endpoint);
	df_set_auth_key(pvt->session, pvt->service_key);

	if (!ast_strlen_zero(event)) {
		ast_log(LOG_DEBUG, "Activating project %s ('%s'), event %s on %s\n", 
			pvt->project_id, pvt->logical_agent_name, pvt->event, pvt->session_id);
	} else {
		ast_log(LOG_DEBUG, "Activating project %s ('%s') on %s\n", pvt->project_id, pvt->logical_agent_name,
			pvt->session_id);
	}
}

static void activate_new_style_grammar(struct gdf_pvt *pvt, const char *grammar_name)
{
	const char *name_part = grammar_name + BUILTIN_COLON_GRAMMAR_SLASH_LEN;
	const char *event_part = "";
	size_t name_len;
	const char *question_mark;

	if ((question_mark = strchr(name_part, '?'))) {
		name_len = question_mark - name_part;
		event_part = question_mark + 1;
	} else {
		name_len = strlen(name_part);
	}

	activate_agent_for_name(pvt, name_part, name_len, event_part);
}

/** activate is used in this context to prime DFE with an event for 'detection'
 * 	this is typically used when starting up (e.g. event:welcome)
 */
static int gdf_activate(struct ast_speech *speech, const char *grammar_name)
{
	struct gdf_pvt *pvt = speech->data;
	if (is_grammar_old_style_event(grammar_name)) {
		activate_old_style_event(pvt, grammar_name);
	} else if (is_grammar_new_style_format(grammar_name)) {
		activate_new_style_grammar(pvt, grammar_name);
	} else {
		ast_log(LOG_WARNING, "Do not understand grammar name %s on %s\n", grammar_name, pvt->session_id);
		return -1;
	}
	return 0;
}

static int gdf_deactivate(struct ast_speech *speech, const char *grammar_name)
{
	return 0;
}

static int calculate_audio_level(const short *slin, int len)
{
	int i;
	long long sum = 0;
	for (i = 0; i < len; i++) {
		short sample = slin[i];
		sum += abs(sample);
	}
#ifdef RES_SPEECH_GDFE_DEBUG_VAD
	ast_log(LOG_DEBUG, "packet sum = %lld, average = %d\n", sum, (int)(sum / len));
#endif
	return sum / len;
}

static void write_end_of_recognition_call_event(struct gdf_pvt *pvt)
{
	gdf_log_call_event_only(pvt, CALL_LOG_TYPE_SESSION, "end");
}

static int are_currently_recording_pre_endpointed_audio(struct gdf_pvt *pvt)
{
	int are_recording;
	ast_mutex_lock(&pvt->lock);
	are_recording = (pvt->utterance_preendpointer_recording_file_handle != NULL);
	ast_mutex_unlock(&pvt->lock);
	return are_recording;
}

static int open_preendpointed_recording_file(struct gdf_pvt *pvt)
{
	struct ast_str *path = build_log_related_filename_to_thread_local_str(pvt, 1, "pre", "ul");
	FILE *record_file;

	ast_mutex_lock(&pvt->lock);
	pvt->utterance_preendpointer_recording_open_already_attempted = 1;
	ast_mutex_unlock(&pvt->lock);

	record_file = fopen(ast_str_buffer(path), "w");
	if (record_file) {
		struct dialogflow_log_data log_data[] = {
			{ "filename", ast_str_buffer(path) }
		};
		gdf_log_call_event(pvt, CALL_LOG_TYPE_ENDPOINTER, "pre_recording_start", ARRAY_LEN(log_data), log_data);
		ast_log(LOG_DEBUG, "Opened %s for preendpointer recording for %s\n", ast_str_buffer(path), pvt->session_id);
		ast_mutex_lock(&pvt->lock);
		pvt->utterance_preendpointer_recording_file_handle = record_file;
		ast_mutex_unlock(&pvt->lock);
	} else {
		ast_log(LOG_WARNING, "Unable to open %s for preendpointer recording for %s -- %d: %s\n", ast_str_buffer(path), pvt->session_id, errno, strerror(errno));
	}

	return (record_file == NULL ? -1 : 0);
}

static int open_postendpointed_recording_file(struct gdf_pvt *pvt)
{
	struct ast_str *path = build_log_related_filename_to_thread_local_str(pvt, 1, "post", "ul");
	FILE *record_file;

	ast_mutex_lock(&pvt->lock);
	pvt->utterance_postendpointer_recording_open_already_attempted = 1;
	ast_mutex_unlock(&pvt->lock);

	record_file = fopen(ast_str_buffer(path), "w");
	if (record_file) {
		struct dialogflow_log_data log_data[] = {
			{ "filename", ast_str_buffer(path) }
		};
		gdf_log_call_event(pvt, CALL_LOG_TYPE_ENDPOINTER, "post_recording_start", ARRAY_LEN(log_data), log_data);
		ast_log(LOG_DEBUG, "Opened %s for postendpointer recording for %s\n", ast_str_buffer(path), pvt->session_id);
		ast_mutex_lock(&pvt->lock);
		pvt->utterance_postendpointer_recording_file_handle = record_file;
		ast_mutex_unlock(&pvt->lock);
	} else {
		ast_log(LOG_WARNING, "Unable to open %s for postendpointer recording for %s -- %d: %s\n", ast_str_buffer(path), pvt->session_id, errno, strerror(errno));
	}

	return (record_file == NULL ? -1 : 0);
}

static void maybe_record_audio(struct gdf_pvt *pvt, const char *mulaw, size_t mulaw_len, enum VAD_STATE current_vad_state)
{
	struct gdf_config *config = gdf_get_config();
	int enable_preendpointer_recordings = 0;
	int enable_postendpointer_recordings = 0;
	int currently_recording_preendpointed_audio = 0;
	int currently_recording_postendpointed_audio = 0;
	int already_attempted_open_for_preendpointed_audio = 0;
	int already_attempted_open_for_postendpointed_audio = 0;

	if (config) {
		enable_preendpointer_recordings = config->enable_preendpointer_recordings;
		enable_postendpointer_recordings = config->enable_postendpointer_recordings;		
		ao2_t_ref(config, -1, "done with config checking for recording");
	}

	if (enable_postendpointer_recordings || enable_preendpointer_recordings) {
		int have_call_log_path;
		ast_mutex_lock(&pvt->lock);
		have_call_log_path = !ast_strlen_zero(pvt->call_log_path);
		if (have_call_log_path) {
			currently_recording_preendpointed_audio = (pvt->utterance_preendpointer_recording_file_handle != NULL);
			already_attempted_open_for_preendpointed_audio = pvt->utterance_preendpointer_recording_open_already_attempted;
			currently_recording_postendpointed_audio = (pvt->utterance_postendpointer_recording_file_handle != NULL);
			already_attempted_open_for_postendpointed_audio = pvt->utterance_postendpointer_recording_open_already_attempted;
		}
		ast_mutex_unlock(&pvt->lock);
	}

	if (enable_preendpointer_recordings) {
		if (!currently_recording_preendpointed_audio && !already_attempted_open_for_preendpointed_audio) {
			if (!open_preendpointed_recording_file(pvt)) {
				currently_recording_preendpointed_audio = 1;
			}
		}
		if (currently_recording_preendpointed_audio) {
			size_t written = fwrite(mulaw, sizeof(char), mulaw_len, pvt->utterance_preendpointer_recording_file_handle);
			if (written < mulaw_len) {
				ast_log(LOG_WARNING, "Only wrote %d of %d bytes for pre-endpointed recording for %s\n",
					(int) written, (int) mulaw_len, pvt->session_id);
			}
		}
	}

	if (enable_postendpointer_recordings && current_vad_state == VAD_STATE_SPEAK) {
		if (!currently_recording_postendpointed_audio && !already_attempted_open_for_postendpointed_audio) {
			if (!open_postendpointed_recording_file(pvt)) {
				currently_recording_postendpointed_audio = 1;
			}
		}
		if (currently_recording_postendpointed_audio) {
			size_t written = fwrite(mulaw, sizeof(char), mulaw_len, pvt->utterance_postendpointer_recording_file_handle);
			if (written < mulaw_len) {
				ast_log(LOG_WARNING, "Only wrote %d of %d bytes for post-endpointed recording for %s\n",
					(int) written, (int) mulaw_len, pvt->session_id);
			}
		}
	}
}

static void close_preendpointed_audio_recording(struct gdf_pvt *pvt)
{
	ast_mutex_lock(&pvt->lock);
	if (pvt->utterance_preendpointer_recording_file_handle) {
		fclose(pvt->utterance_preendpointer_recording_file_handle);
		pvt->utterance_preendpointer_recording_file_handle = NULL;
	}
	ast_mutex_unlock(&pvt->lock);
	gdf_log_call_event_only(pvt, CALL_LOG_TYPE_ENDPOINTER, "pre_recording_stop");
}

static void close_postendpointed_audio_recording(struct gdf_pvt *pvt)
{
	ast_mutex_lock(&pvt->lock);
	if (pvt->utterance_postendpointer_recording_file_handle) {
		fclose(pvt->utterance_postendpointer_recording_file_handle);
		pvt->utterance_postendpointer_recording_file_handle = NULL;
	}
	ast_mutex_unlock(&pvt->lock);
	gdf_log_call_event_only(pvt, CALL_LOG_TYPE_ENDPOINTER, "post_recording_stop");
}

static int gdf_stop_recognition(struct ast_speech *speech, struct gdf_pvt *pvt)
{
	close_preendpointed_audio_recording(pvt);
	close_postendpointed_audio_recording(pvt);
	ast_speech_change_state(speech, AST_SPEECH_STATE_DONE);
	write_end_of_recognition_call_event(pvt);
	return 0;
}

/* speech structure is locked */
static int gdf_write(struct ast_speech *speech, void *data, int len)
{
	struct gdf_pvt *pvt = speech->data;
	enum dialogflow_session_state state;
	enum VAD_STATE vad_state;
	enum VAD_STATE orig_vad_state;
	int threshold;
	int cur_duration;
	int change_duration;
	int avg_level;
	int voice_duration;
	int silence_duration;
	int datams;
	int datasamples;

	ast_mutex_lock(&pvt->lock);
	orig_vad_state = vad_state = pvt->vad_state;
	threshold = pvt->voice_threshold;
	cur_duration = pvt->vad_state_duration;
	change_duration = pvt->vad_change_duration;
	voice_duration = pvt->voice_minimum_duration;
	silence_duration = pvt->silence_minimum_duration;
	ast_mutex_unlock(&pvt->lock);

	datasamples = len / sizeof(short); /* 2 bytes per sample for slin */
	datams = datasamples / 8; /* 8 samples per millisecond */

	cur_duration += datams;

	avg_level = calculate_audio_level((short *)data, len);
	if (avg_level >= threshold) {
		if (vad_state != VAD_STATE_SPEAK) {
			change_duration += datams;
		} else {
			change_duration = 0;
		}
	} else {
		if (vad_state != VAD_STATE_SPEAK) {
			change_duration = 0;
		} else {
			change_duration += datams;
		}
	}

	if (vad_state == VAD_STATE_START) {
		if (change_duration >= voice_duration) {
			/* speaking */
			vad_state = VAD_STATE_SPEAK;
			change_duration = 0;
			cur_duration = 0;
			gdf_log_call_event_only(pvt, CALL_LOG_TYPE_ENDPOINTER, "start_of_speech");
		}
	} else if (vad_state == VAD_STATE_SPEAK) {
		if (change_duration >= silence_duration) {
			/* stopped speaking */
			/* noop at this time */
			vad_state = VAD_STATE_SILENT;
			change_duration = 0;
			cur_duration = 0;
			gdf_log_call_event_only(pvt, CALL_LOG_TYPE_ENDPOINTER, "end_of_speech");
		}
	}

	ast_mutex_lock(&pvt->lock);
	pvt->vad_state = vad_state;
	pvt->vad_state_duration = cur_duration;
	pvt->vad_change_duration = change_duration;
	ast_mutex_unlock(&pvt->lock);

#ifdef RES_SPEECH_GDFE_DEBUG_VAD
	ast_log(LOG_DEBUG, "avg: %d thr: %d dur: %d chg: %d vce: %d sil: %d old: %d new: %d\n",
		avg_level, threshold, cur_duration, change_duration, voice_duration, silence_duration, 
		orig_vad_state, vad_state);
#endif

	if (vad_state == VAD_STATE_SPEAK && orig_vad_state == VAD_STATE_START) {
		if (df_start_recognition(pvt->session, pvt->language, 0)) {
			ast_log(LOG_WARNING, "Error starting recognition on %s\n", pvt->session_id);
			gdf_stop_recognition(speech, pvt);
		}
	}

	if (vad_state != VAD_STATE_START) {
		int mulaw_len = datasamples * sizeof(char);
		char *mulaw = alloca(mulaw_len);
		int i;

		for (i = 0; i < datasamples; i++) {
			mulaw[i] = AST_LIN2MU(((short *)data)[i]);
		}

		maybe_record_audio(pvt, mulaw, mulaw_len, vad_state);

		state = df_write_audio(pvt->session, mulaw, mulaw_len);

		if (!ast_test_flag(speech, AST_SPEECH_SPOKE) && df_get_response_count(pvt->session) > 0) {
			ast_set_flag(speech, AST_SPEECH_QUIET);
			ast_set_flag(speech, AST_SPEECH_SPOKE);
		}

		if (state == DF_STATE_FINISHED || state == DF_STATE_ERROR) {
			df_stop_recognition(pvt->session);
			gdf_stop_recognition(speech, pvt);
		}
	} else if (are_currently_recording_pre_endpointed_audio(pvt)) {
		int mulaw_len = datasamples * sizeof(char);
		char *mulaw = alloca(mulaw_len);
		int i;

		for (i = 0; i < datasamples; i++) {
			mulaw[i] = AST_LIN2MU(((short *)data)[i]);
		}

		maybe_record_audio(pvt, mulaw, mulaw_len, vad_state);
	}

	return 0;
}

static int gdf_dtmf(struct ast_speech *speech, const char *dtmf)
{
	return -1;
}

static int should_start_call_log(struct gdf_pvt *pvt)
{
	int should_start;
	ast_mutex_lock(&pvt->lock);
	should_start = !pvt->call_log_open_already_attempted;
	ast_mutex_unlock(&pvt->lock);
	if (should_start) {
		struct gdf_config *cfg;
		cfg = gdf_get_config();
		if (cfg) {
			should_start &= cfg->enable_call_logs;
			ao2_t_ref(cfg, -1, "done checking for starting call log");
		}
	}
	return should_start;
}

AST_THREADSTORAGE(call_log_path);
static void calculate_log_path(struct gdf_pvt *pvt)
{
	struct varshead var_head = { .first = NULL, .last = NULL };
	struct ast_var_t *var;
	struct gdf_config *cfg;

	ast_mutex_lock(&pvt->lock);
	var = ast_var_assign("APPLICATION", pvt->call_logging_application_name);
	ast_mutex_unlock(&pvt->lock);

	AST_LIST_INSERT_HEAD(&var_head, var, entries);

	cfg = gdf_get_config();
	if (cfg) {
		struct ast_str *path = ast_str_thread_get(&call_log_path, 256);

		ast_str_substitute_variables_varshead(&path, 0, &var_head, cfg->call_log_location);

		ast_mutex_lock(&pvt->lock);
		ast_string_field_set(pvt, call_log_path, ast_str_buffer(path));
		ast_mutex_unlock(&pvt->lock);

		ao2_t_ref(cfg, -1, "done with config in calculating call log path");
	}
	
	ast_var_delete(var);
}

static void calculate_log_file_basename(struct gdf_pvt *pvt)
{
	struct timeval t;
	struct ast_tm now;
	
	t = ast_tvnow();
	ast_localtime(&t, &now, NULL);
	ast_string_field_build(pvt, call_log_file_basename, "%02d%02d_%s", now.tm_min, now.tm_sec, pvt->session_id);
}

static void mkdir_log_path(struct gdf_pvt *pvt)
{
	ast_mkdir(pvt->call_log_path, 0644);
}

static struct ast_str *build_log_related_filename_to_thread_local_str(struct gdf_pvt *pvt, int include_utterance_counter, const char *type, const char *extension)
{
	struct ast_str *path;
	path = ast_str_thread_get(&call_log_path, 256);
	ast_mutex_lock(&pvt->lock);
	ast_str_set(&path, 0, pvt->call_log_path);
	ast_str_append(&path, 0, pvt->call_log_file_basename);
	ast_str_append(&path, 0, "_%s", type);
	if (include_utterance_counter) {
		ast_str_append(&path, 0, "_%d", pvt->utterance_counter);
	}
	ast_str_append(&path, 0, ".%s" , extension);
	ast_mutex_unlock(&pvt->lock);
	return path;
}

static void start_call_log(struct gdf_pvt *pvt)
{
	ast_mutex_lock(&pvt->lock);
	pvt->call_log_open_already_attempted = 1;
	ast_mutex_unlock(&pvt->lock);

	calculate_log_path(pvt);
	calculate_log_file_basename(pvt);

	if (!ast_strlen_zero(pvt->call_log_path)) {
		struct ast_str *path;
		FILE *log_file;

		mkdir_log_path(pvt);

		path = build_log_related_filename_to_thread_local_str(pvt, 0, "log", "jsonl");

		log_file = fopen(ast_str_buffer(path), "w");
		if (log_file) {
			ast_log(LOG_DEBUG, "Opened %s for call log for %s\n", ast_str_buffer(path), pvt->session_id);
			ast_mutex_lock(&pvt->lock);
			pvt->call_log_file_handle = log_file;
			ast_mutex_unlock(&pvt->lock);
		} else {
			ast_log(LOG_WARNING, "Unable to open %s for writing call log for %s -- %d: %s\n", ast_str_buffer(path), pvt->session_id, errno, strerror(errno));
		}
	} else {
		ast_log(LOG_WARNING, "Not starting call log, path is empty\n");
	}
}

static void log_endpointer_start_event(struct gdf_pvt *pvt)
{
	int pvt_threshold;
	char threshold[11];
	int pvt_voice_duration;
	char voice_duration[11];
	int pvt_silence_duration;
	char silence_duration[11];
	struct dialogflow_log_data log_data[] = {
		{ VAD_PROP_VOICE_THRESHOLD, threshold },
		{ VAD_PROP_VOICE_DURATION, voice_duration },
		{ VAD_PROP_SILENCE_DURATION, silence_duration },
	};

	ast_mutex_lock(&pvt->lock);
	pvt_threshold = pvt->voice_threshold;
	pvt_voice_duration = pvt->voice_minimum_duration;
	pvt_silence_duration = pvt->silence_minimum_duration;
	ast_mutex_unlock(&pvt->lock);

	sprintf(threshold, "%d", pvt_threshold);
	sprintf(voice_duration, "%d", pvt_voice_duration);
	sprintf(silence_duration, "%d", pvt_silence_duration);

	gdf_log_call_event(pvt, CALL_LOG_TYPE_ENDPOINTER, "start", ARRAY_LEN(log_data), log_data);
}

static int gdf_start(struct ast_speech *speech)
{
	struct gdf_pvt *pvt = speech->data;
	char *event = NULL;
	char *language = NULL;
	char *project_id = NULL;

	ast_mutex_lock(&pvt->lock);
	event = ast_strdupa(pvt->event);
	language = ast_strdupa(pvt->language);
	project_id = ast_strdupa(pvt->project_id);
	ast_string_field_set(pvt, event, "");
	pvt->vad_state = VAD_STATE_START;
	pvt->vad_state_duration = 0;
	pvt->vad_change_duration = 0;
	pvt->utterance_counter++;
	ast_mutex_unlock(&pvt->lock);

	if (should_start_call_log(pvt)) {
		start_call_log(pvt);
	}

	{
		char utterance_number[11];
		struct dialogflow_log_data log_data[] = {
			{ "event", event },
			{ "language", language },
			{ "project_id", project_id },
			{ "logical_agent_name", pvt->logical_agent_name },
			{ "utterance", utterance_number },
			{ "context", pvt->call_logging_context },
			{ "application", pvt->call_logging_application_name }
		};
		sprintf(utterance_number, "%d", pvt->utterance_counter);
		gdf_log_call_event(pvt, CALL_LOG_TYPE_SESSION, "start", ARRAY_LEN(log_data), log_data);
	}
	log_endpointer_start_event(pvt);
	
	if (!ast_strlen_zero(event)) {
		if (df_recognize_event(pvt->session, event, language, 0)) {
			ast_log(LOG_WARNING, "Error recognizing event on %s\n", pvt->session_id);
			ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);
		} else {
			gdf_stop_recognition(speech, pvt);
		}
	} else {
		ast_speech_change_state(speech, AST_SPEECH_STATE_READY);
	}

	return 0;
}

static int gdf_change(struct ast_speech *speech, const char *name, const char *value)
{
	struct gdf_pvt *pvt = speech->data;

	if (!strcasecmp(name, GDF_PROP_SESSION_ID_NAME) || !strcasecmp(name, GDF_PROP_ALTERNATE_SESSION_NAME)) {
		if (ast_strlen_zero(value)) {
			ast_log(LOG_WARNING, "Session ID must have a value, refusing to set to nothing (remains %s)\n", df_get_session_id(pvt->session));
			return -1;
		}
		df_set_session_id(pvt->session, value);
		ast_mutex_lock(&pvt->lock);
		ast_string_field_set(pvt, session_id, value);
		ast_mutex_unlock(&pvt->lock);
	} else if (!strcasecmp(name, GDF_PROP_PROJECT_ID_NAME)) {
		if (ast_strlen_zero(value)) {
			ast_log(LOG_WARNING, "Project ID must have a value, refusing to set to nothing (remains %s)\n", df_get_project_id(pvt->session));
			return -1;
		}
		ast_mutex_lock(&pvt->lock);
		ast_string_field_set(pvt, project_id, value);
		ast_mutex_unlock(&pvt->lock);
		df_set_project_id(pvt->session, value);
	} else if (!strcasecmp(name, GDF_PROP_LANGUAGE_NAME)) {
		ast_mutex_lock(&pvt->lock);
		ast_string_field_set(pvt, language, value);
		ast_mutex_unlock(&pvt->lock);
	} else if (!strcasecmp(name, GDF_PROP_LOG_CONTEXT) || !strcasecmp(name, GDF_PROP_ALTERNATE_LOG_CONTEXT)) {
		ast_mutex_lock(&pvt->lock);
		ast_string_field_set(pvt, call_logging_context, value);
		ast_mutex_unlock(&pvt->lock);
	} else if (!strcasecmp(name, GDF_PROP_APPLICATION_CONTEXT)) {
		ast_mutex_lock(&pvt->lock);
		ast_string_field_set(pvt, call_logging_application_name, value);
		ast_mutex_unlock(&pvt->lock);
	} else if (!strcasecmp(name, VAD_PROP_VOICE_THRESHOLD)) {
		int i;
		if (ast_strlen_zero(value)) {
			ast_log(LOG_WARNING, "Cannot set " VAD_PROP_VOICE_THRESHOLD " to an empty value\n");
			return -1;
		} else if (sscanf(value, "%d", &i) == 1) {
			ast_mutex_lock(&pvt->lock);
			pvt->voice_threshold = i;
			ast_mutex_unlock(&pvt->lock);
		} else {
			ast_log(LOG_WARNING, "Invalid value for " VAD_PROP_VOICE_THRESHOLD " -- '%s'\n", value);
			return -1;
		}
	} else if (!strcasecmp(name, VAD_PROP_VOICE_DURATION)) {
		int i;
		if (ast_strlen_zero(value)) {
			ast_log(LOG_WARNING, "Cannot set " VAD_PROP_VOICE_DURATION " to an empty value\n");
			return -1;
		} else if (sscanf(value, "%d", &i) == 1) {
			ast_mutex_lock(&pvt->lock);
			pvt->voice_minimum_duration = i;
			ast_mutex_unlock(&pvt->lock);
		} else {
			ast_log(LOG_WARNING, "Invalid value for " VAD_PROP_VOICE_DURATION " -- '%s'\n", value);
			return -1;
		}
	} else if (!strcasecmp(name, VAD_PROP_SILENCE_DURATION)) {
		int i;
		if (ast_strlen_zero(value)) {
			ast_log(LOG_WARNING, "Cannot set " VAD_PROP_SILENCE_DURATION " to an empty value\n");
			return -1;
		} else if (sscanf(value, "%d", &i) == 1) {
			ast_mutex_lock(&pvt->lock);
			pvt->silence_minimum_duration = i;
			ast_mutex_unlock(&pvt->lock);
		} else {
			ast_log(LOG_WARNING, "Invalid value for " VAD_PROP_SILENCE_DURATION " -- '%s'\n", value);
			return -1;
		}
	} else {
		ast_log(LOG_WARNING, "Unknown property '%s'\n", name);
		return -1;
	}

	return 0;
}

#ifdef ASTERISK_13_OR_LATER
static int gdf_get_setting(struct ast_speech *speech, const char *name, char *buf, size_t len)
{
	struct gdf_pvt *pvt = speech->data;

	if (!strcasecmp(name, GDF_PROP_SESSION_ID_NAME)) {
		ast_copy_string(buf, df_get_session_id(pvt->session), len);
	} else if (!strcasecmp(name, GDF_PROP_PROJECT_ID_NAME)) {
		ast_copy_string(buf, df_get_project_id(pvt->session), len);
	} else if (!strcasecmp(name, GDF_PROP_LANGUAGE_NAME)) {
		ast_mutex_lock(&pvt->lock);
		ast_copy_string(buf, pvt->language, len);
		ast_mutex_unlock(&pvt->lock);
	} else if (!strcasecmp(name, VAD_PROP_VOICE_THRESHOLD)) {
		ast_mutex_lock(&pvt->lock);
		ast_build_string(&buf, &len, "%d", pvt->voice_threshold);
		ast_mutex_unlock(&pvt->lock);
	} else if (!strcasecmp(name, VAD_PROP_VOICE_DURATION)) {
		ast_mutex_lock(&pvt->lock);
		ast_build_string(&buf, &len, "%d", pvt->voice_minimum_duration);
		ast_mutex_unlock(&pvt->lock);
	} else if (!strcasecmp(name, VAD_PROP_SILENCE_DURATION)) {
		ast_mutex_lock(&pvt->lock);
		ast_build_string(&buf, &len, "%d", pvt->silence_minimum_duration);
		ast_mutex_unlock(&pvt->lock);
	} else {
		ast_log(LOG_WARNING, "Unknown property '%s'\n", name);
		return -1;
	}

	return 0;
}
#endif

static int gdf_change_results_type(struct ast_speech *speech, enum ast_speech_results_type results_type)
{
	return 0;
}

static struct ast_speech_result *gdf_get_results(struct ast_speech *speech)
{
	/* speech is not locked */
	struct gdf_pvt *pvt = speech->data;
	int results = df_get_result_count(pvt->session);
	int i;
	struct ast_speech_result *start = NULL;
	struct ast_speech_result *end = NULL;
	static int last_resort = 0;

	struct dialogflow_result *fulfillment_text = NULL;
	struct dialogflow_result *output_audio = NULL;

	const char *audioFile = NULL;

	for (i = 0; i < results; i++) {
		struct dialogflow_result *df_result = df_get_result(pvt->session, i); /* this is a borrowed reference */
		if (df_result) {
			if (!strcasecmp(df_result->slot, "output_audio")) {
				/* this is fine for now, but we really need a flag on the structure that says it's binary vs. text */
				output_audio = df_result;
			} else {
				struct ast_speech_result *new = ast_calloc(1, sizeof(*new));
				if (new) {
					new->text = ast_strdup(df_result->value);
					new->score = df_result->score;
					new->grammar = ast_strdup(df_result->slot);

					if (!strcasecmp(df_result->slot, "fulfillment_text")) {
						fulfillment_text = df_result;
					}
				}

				if (end) {
					AST_LIST_NEXT(end, list) = new;
					end = new;
				} else {
					start = end = new;
				}
			}
		}
	}

	if (output_audio) { 
		struct ast_speech_result *new;
		char tmpFilename[128];
		int fd;
		ssize_t written;

		ast_copy_string(tmpFilename, "/tmp/res_speech_gdfe_fulfillment_XXXXXX.wav", sizeof(tmpFilename));
		fd = mkstemps(tmpFilename, 4);

		if (fd < 0) {
			ast_log(LOG_WARNING, "Unable to create temporary file for fulfillment message\n");
			sprintf(tmpFilename, "/tmp/res_speech_gdfe_fulfillment_%d.wav", ast_atomic_fetchadd_int(&last_resort, 1));
			fd = open(tmpFilename, O_WRONLY | O_CREAT, 0600);
		}
		written = write(fd, output_audio->value, output_audio->valueLen);
		if (written < output_audio->valueLen) {
			ast_log(LOG_WARNING, "Short write to temporary file for fulfillment message\n");
		}
		close(fd);

		audioFile = tmpFilename;

		new = ast_calloc(1, sizeof(*new));
		if (new) {
			new->text = ast_strdup(tmpFilename);
			new->score = 100;
			new->grammar = ast_strdup("fulfillment_audio");

			if (end) {
				AST_LIST_NEXT(end, list) = new;
				end = new;
			} else {
				start = end = new;
			}
		} else {
			ast_log(LOG_WARNING, "Unable to allocate speech result slot for synthesized fulfillment text\n");
		}
	} else if (fulfillment_text && !ast_strlen_zero(fulfillment_text->value)) {
		char tmpFilename[128];
		int fd;
		struct gdf_config *cfg;
		char *key;
		char *language;

		cfg = gdf_get_config();
		key = ast_strdupa(cfg->service_key);
		ao2_t_ref(cfg, -1, "done with creating session");

		ast_mutex_lock(&pvt->lock);
		language = ast_strdupa(pvt->language);
		ast_mutex_unlock(&pvt->lock);

		ast_copy_string(tmpFilename, "/tmp/res_speech_gdfe_fulfillment_XXXXXX.wav", sizeof(tmpFilename));
		fd = mkstemps(tmpFilename, 4);

		if (fd >= 0) {
			close(fd);
		} else {
			ast_log(LOG_WARNING, "Unable to create temporary file for fulfillment message\n");
			sprintf(tmpFilename, "/tmp/res_speech_gdfe_fulfillment_%d.wav", ast_atomic_fetchadd_int(&last_resort, 1));
		}

		audioFile = tmpFilename;

		if (google_synth_speech(NULL, key, fulfillment_text->value, language, NULL, tmpFilename)) {
			ast_log(LOG_WARNING, "Failed to synthesize fulfillment text to %s\n", tmpFilename);
		} else {
			struct ast_speech_result *new = ast_calloc(1, sizeof(*new));
			if (new) {
				new->text = ast_strdup(tmpFilename);
				new->score = 100;
				new->grammar = ast_strdup("fulfillment_audio");

				if (end) {
					AST_LIST_NEXT(end, list) = new;
					end = new;
				} else {
					start = end = new;
				}
			} else {
				ast_log(LOG_WARNING, "Unable to allocate speech result slot for synthesized fulfillment text\n");
			}
		}
	}

	if (!ast_strlen_zero(audioFile)) {
		if (!ast_strlen_zero(pvt->lastAudioResponse)) {
			unlink(pvt->lastAudioResponse);
		}
		ast_string_field_set(pvt, lastAudioResponse, audioFile);
	}

	return start;
}

static void gdf_config_destroy(void *o)
{
	struct gdf_config *conf = o;

	ast_string_field_free_memory(conf);

	if (conf->logical_agents) {
		ao2_ref(conf->logical_agents, -1);
	}
}

static struct gdf_config *gdf_get_config(void)
{
	struct gdf_config *cfg;
#ifdef ASTERISK_13_OR_LATER
	ao2_rdlock(config);
#else
	ao2_lock(config);
#endif
	cfg = ao2_find(config, NULL, 0);
	ao2_unlock(config);
	return cfg;
}

static void logical_agent_destructor(void *obj)
{
	/* noop */
}

static struct gdf_logical_agent *logical_agent_alloc(const char *name, const char *project_id, const char *service_key, const char *endpoint)
{
	size_t name_len = strlen(name);
	size_t project_id_len = strlen(project_id);
	size_t service_key_len = strlen(service_key);
	size_t endpoint_len = strlen(endpoint);
	size_t space_needed = name_len + 1 +
							project_id_len + 1 +
							service_key_len + 1 +
							endpoint_len + 1;
	struct gdf_logical_agent *agent;
	
	agent = ao2_alloc(space_needed + sizeof(struct gdf_logical_agent), logical_agent_destructor);
	if (agent) {
		ast_copy_string(agent->endpoint, endpoint, endpoint_len + 1);
		agent->service_key = agent->endpoint + endpoint_len + 1;
		ast_copy_string((char *)agent->service_key, service_key, service_key_len + 1);
		agent->project_id = agent->service_key + service_key_len + 1;
		ast_copy_string((char *)agent->project_id, project_id, project_id_len + 1);
		agent->name = agent->project_id + project_id_len + 1;
		ast_copy_string((char *)agent->name, name, name_len + 1);
	}

	return agent;
}

static int logical_agent_hash_callback(const void *obj, const int flags)
{
	const struct gdf_logical_agent *agent = obj;
	return ast_str_case_hash(agent->name);
}

static int logical_agent_compare_callback(void *obj, void *other, int flags)
{
	const struct gdf_logical_agent *agentA = obj;
	const struct gdf_logical_agent *agentB = other;
	return (!strcasecmp(agentA->name, agentB->name) ? CMP_MATCH | CMP_STOP : 0);
}

static struct gdf_logical_agent *get_logical_agent_by_name(struct gdf_config *config, const char *name)
{
	struct gdf_logical_agent tmpAgent = { .name = name };
	return ao2_find(config->logical_agents, &tmpAgent, OBJ_POINTER);
}

static struct ast_str *load_service_key(const char *val)
{
	struct ast_str *buffer = ast_str_create(3 * 1024); /* big enough for the typical key size */
	if (!buffer) {
		ast_log(LOG_WARNING, "Memory allocation failure allocating ast_str for loading service key\n");
		return NULL;
	}

	if (strchr(val, '{')) {
		ast_str_set(&buffer, 0, val);
	} else {
		FILE *f;
		ast_log(LOG_DEBUG, "Loading service key data from %s\n", val);
		f = fopen(val, "r");
		if (f) {
			char readbuffer[512];
			size_t read = fread(readbuffer, sizeof(char), sizeof(readbuffer), f);
			while (read > 0) {
				ast_str_append_substr(&buffer, 0, readbuffer, read);
				read = fread(readbuffer, sizeof(char), sizeof(readbuffer), f);
			}
			if (ferror(f)) {
				ast_log(LOG_WARNING, "Error reading %s -- %d\n", val, errno);
			}
			fclose(f);
		} else {
			ast_log(LOG_ERROR, "Unable to open service key file %s -- %d\n", val, errno);
		}
	}

	return buffer;
}

#define CONFIGURATION_FILENAME		"res_speech_gdfe.conf"
static int load_config(int reload)
{
	struct ast_config *cfg = NULL;
	struct ast_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	cfg = ast_config_load(CONFIGURATION_FILENAME, config_flags);
	if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		ast_log(LOG_DEBUG, "Configuration unchanged.\n");
	} else {
		struct gdf_config *conf;
		const char *val;
		const char *category;

		if (cfg == CONFIG_STATUS_FILEINVALID) {
			ast_log(LOG_WARNING, "Configuration file invalid\n");
			cfg = ast_config_new();
		} else if (cfg == CONFIG_STATUS_FILEMISSING) {
			ast_log(LOG_WARNING, "Configuration not found, using defaults\n");
			cfg = ast_config_new();
		}
		
		conf = ao2_alloc(sizeof(*conf), gdf_config_destroy);
		if (!conf) {
			ast_log(LOG_WARNING, "Failed to allocate config record for speech gdf\n");
			ast_config_destroy(cfg);
			return AST_MODULE_LOAD_FAILURE;
		}

		if (ast_string_field_init(conf, 3 * 1024)) {
			ast_log(LOG_WARNING, "Failed to allocate string fields for config for speech gdf\n");
			ao2_ref(conf, -1);
			ast_config_destroy(cfg);
			return AST_MODULE_LOAD_FAILURE;
		}

		conf->logical_agents = ao2_container_alloc(32, logical_agent_hash_callback, logical_agent_compare_callback);
		if (!conf->logical_agents) {
			ast_log(LOG_WARNING, "Failed to allocate logical agent container for speech gdf\n");
			ao2_ref(conf, -1);
			ast_config_destroy(cfg);
		}

		val = ast_variable_retrieve(cfg, "general", "service_key");
		if (ast_strlen_zero(val)) {
			ast_log(LOG_VERBOSE, "Service key not provided -- will use default credentials.\n");
		} else {
			struct ast_str *buffer = load_service_key(val);
			ast_string_field_set(conf, service_key, ast_str_buffer(buffer));
			ast_free(buffer);
		}

		val = ast_variable_retrieve(cfg, "general", "endpoint");
		if (!ast_strlen_zero(val)) {
			ast_string_field_set(conf, endpoint, val);
		}

		conf->vad_voice_threshold = 512;
		val = ast_variable_retrieve(cfg, "general", "vad_voice_threshold");
		if (!ast_strlen_zero(val)) {
			int i;
			if (sscanf(val, "%d", &i) == 1) {
				conf->vad_voice_threshold = i;
			} else {
				ast_log(LOG_WARNING, "Invalid value for vad_voice_threshold\n");
			}
		}

		conf->vad_voice_minimum_duration = 40; /* ms */
		val = ast_variable_retrieve(cfg, "general", "vad_voice_minimum_duration");
		if (!ast_strlen_zero(val)) {
			int i;
			if (sscanf(val, "%d", &i) == 1) {
				conf->vad_voice_minimum_duration = i;
			} else {
				ast_log(LOG_WARNING, "Invalid value for vad_voice_minimum_duration\n");
			}
		}

		conf->vad_silence_minimum_duration = 500; /* ms */
		val = ast_variable_retrieve(cfg, "general", "vad_silence_minimum_duration");
		if (!ast_strlen_zero(val)) {
			int i;
			if (sscanf(val, "%d", &i) == 1) {
				conf->vad_silence_minimum_duration = i;
			} else {
				ast_log(LOG_WARNING, "Invalid value for vad_silence_minimum_duration\n");
			}
		}

		ast_string_field_set(conf, call_log_location, "/var/log/dialogflow/${APPLICATION}/${STRFTIME(,,%Y/%m/%d/%H)}/");
		val = ast_variable_retrieve(cfg, "general", "call_log_location");
		if (!ast_strlen_zero(val)) {
			ast_string_field_set(conf, call_log_location, val);
		}

		conf->enable_call_logs = 1;
		val = ast_variable_retrieve(cfg, "general", "enable_call_logs");
		if (!ast_strlen_zero(val)) {
			conf->enable_call_logs = ast_true(val);
		}

		conf->enable_preendpointer_recordings = 0;
		val = ast_variable_retrieve(cfg, "general", "enable_preendpointer_recordings");
		if (!ast_strlen_zero(val)) {
			conf->enable_preendpointer_recordings = ast_true(val);
		}

		conf->enable_postendpointer_recordings = 0;
		val = ast_variable_retrieve(cfg, "general", "enable_postendpointer_recordings");
		if (!ast_strlen_zero(val)) {
			conf->enable_postendpointer_recordings = ast_true(val);
		}

		category = NULL;
		while ((category = ast_category_browse(cfg, category))) {
			if (strcasecmp("general", category)) {
				const char *name = category;
				const char *project_id = ast_variable_retrieve(cfg, category, "project_id");
				const char *endpoint = ast_variable_retrieve(cfg, category, "endpoint");
				const char *service_key = ast_variable_retrieve(cfg, category, "service_key");

				if (!ast_strlen_zero(service_key)) {
					struct ast_str *buffer = load_service_key(service_key);
					if (buffer) {
						service_key = ast_strdupa(ast_str_buffer(buffer));
						ast_free(buffer);
					}
				}

				if (!ast_strlen_zero(project_id)) {
					struct gdf_logical_agent *agent;
					
					agent = logical_agent_alloc(name, project_id, S_OR(service_key, ""), S_OR(endpoint, ""));
					if (agent) {
						ao2_link(conf->logical_agents, agent);
						ao2_ref(agent, -1);
					} else {
						ast_log(LOG_WARNING, "Memory allocation failed creating logical agent %s\n", name);
					}
				} else {
					ast_log(LOG_WARNING, "Mapped project_id is required for %s\n", name);
				}
			}
		}

		/* swap out the configs */
#ifdef ASTERISK_13_OR_LATER
		ao2_wrlock(config);
#else
		ao2_lock(config);
#endif
		{
			struct gdf_config *old_config = gdf_get_config();
			ao2_unlink(config, old_config);
			ao2_ref(old_config, -1);
		}
		ao2_link(config, conf);
		ao2_unlock(config);
		ao2_ref(conf, -1);
	}

	if (cfg) {
		ast_config_destroy(cfg);
	}
	
	return AST_MODULE_LOAD_SUCCESS;
}

static char *gdfe_reload(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "gdfe reload";
		e->usage = 
			"Usage: gdfe reload\n"
			"       Reload res_speech_gdfe configuration.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		ast_cli(a->fd, "Reloading res_speech_gdfe config from " CONFIGURATION_FILENAME "\n");
		load_config(1);
		ast_cli(a->fd, "Reload complete\n");
		ast_cli(a->fd, "\n\n");
		return CLI_SUCCESS;
	}
}

static char *gdfe_show_config(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct gdf_config *config;
	
	switch (cmd) {
	case CLI_INIT:
		e->command = "gdfe show config";
		e->usage = 
			"Usage: gdfe show config\n"
			"       Show current gdfe configuration.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	default:
		config = gdf_get_config();
		if (config) {
			struct ao2_iterator i;
			struct gdf_logical_agent *agent;

			ast_cli(a->fd, "[general]\n");
			ast_cli(a->fd, "service_key = %s\n", config->service_key);
			ast_cli(a->fd, "endpoint = %s\n", config->endpoint);
			ast_cli(a->fd, "vad_voice_threshold = %d\n", config->vad_voice_threshold);
			ast_cli(a->fd, "vad_voice_minimum_duration = %d\n", config->vad_voice_minimum_duration);
			ast_cli(a->fd, "vad_silence_minimum_duration = %d\n", config->vad_silence_minimum_duration);
			ast_cli(a->fd, "call_log_location = %s\n", config->call_log_location);
			ast_cli(a->fd, "enable_call_logs = %s\n", AST_CLI_YESNO(config->enable_call_logs));
			ast_cli(a->fd, "enable_preendpointer_recordings = %s\n", AST_CLI_YESNO(config->enable_preendpointer_recordings));
			ast_cli(a->fd, "enable_postendpointer_recordings = %s\n", AST_CLI_YESNO(config->enable_postendpointer_recordings));
			i = ao2_iterator_init(config->logical_agents, 0);
			while ((agent = ao2_iterator_next(&i))) {
				ast_cli(a->fd, "\n[%s]\n", agent->name);
				ast_cli(a->fd, "project_id = %s\n", agent->project_id);
				ast_cli(a->fd, "endpoint = %s\n", agent->endpoint);
				ast_cli(a->fd, "service_key = %s\n", agent->service_key);
				ao2_ref(agent, -1);
			}
			ao2_iterator_destroy(&i);
			ao2_ref(config, -1);
		} else {
			ast_cli(a->fd, "Unable to retrieve configuration\n");
		}
		ast_cli(a->fd, "\n");
		return CLI_SUCCESS;
	}
}

static struct ast_cli_entry gdfe_cli[] = {
	AST_CLI_DEFINE(gdfe_reload, "Reload gdfe configuration"),
	AST_CLI_DEFINE(gdfe_show_config, "Show current gdfe configuration"),
};

static int call_log_enabled_for_pvt(struct gdf_pvt *pvt)
{
	struct gdf_config *config;
	int log_enabled = 0;
	
	config = gdf_get_config();
	if (config) {
		log_enabled = config->enable_call_logs;
		if (log_enabled) {
			ast_mutex_lock(&pvt->lock);
			log_enabled = (pvt->call_log_file_handle != NULL);
			ast_mutex_unlock(&pvt->lock);
		}

		ao2_t_ref(config, -1, "done with config in log check");
	}
	return log_enabled;
}

#ifndef ASTERISK_13_OR_LATER
#define AST_ISO8601_LEN	29
#endif

static void gdf_log_call_event(struct gdf_pvt *pvt, enum gdf_call_log_type type, const char *event, size_t log_data_size, const struct dialogflow_log_data *log_data)
{
	struct timeval timeval_now;
	struct ast_tm tm_now = {};
	char char_now[AST_ISO8601_LEN];
	const char *char_type;
	char *log_line;
	size_t i;
#ifdef ASTERISK_13_OR_LATER
	RAII_VAR(struct ast_json *, log_message, ast_json_object_create(), ast_json_unref);
#else
	json_t *log_message;
#endif

	if (!call_log_enabled_for_pvt(pvt)) {
		return;
	}
    
	timeval_now = ast_tvnow();
	ast_localtime(&timeval_now, &tm_now, NULL);

	ast_strftime(char_now, sizeof(char_now), "%FT%T.%q%z", &tm_now);

	if (type == CALL_LOG_TYPE_SESSION) {
		char_type = "SESSION";
	} else if (type == CALL_LOG_TYPE_ENDPOINTER) {
		char_type = "ENDPOINTER";
	} else if (type == CALL_LOG_TYPE_DIALOGFLOW) {
		char_type = "DIALOGFLOW";
	} else {
		char_type = "UNKNOWN";
	}

#ifdef ASTERISK_13_OR_LATER
	ast_json_object_set(log_message, "log_timestamp", ast_json_string_create(char_now));
	ast_json_object_set(log_message, "log_type", ast_json_string_create(char_type));
	ast_json_object_set(log_message, "log_event", ast_json_string_create(event));
	for (i = 0; i < log_data_size; i++) {
		ast_json_object_set(log_message, log_data[i].name, ast_json_string_create(log_data[i].value));
	}
	log_line = ast_json_dump_string(log_message);
#else
	log_message = json_object();
	json_object_set_new(log_message, "log_timestamp", json_string(char_now));
	json_object_set_new(log_message, "log_type", json_string(char_type));
	json_object_set_new(log_message, "log_event", json_string(event));
	for (i = 0; i < log_data_size; i++) {
		json_object_set_new(log_message, log_data[i].name, json_string(log_data[i].value));
	}
	log_line = json_dumps(log_message, JSON_COMPACT);
#endif

	ast_mutex_lock(&pvt->lock);
	fprintf(pvt->call_log_file_handle, "%s\n", log_line);
	ast_mutex_unlock(&pvt->lock);

#ifdef ASTERISK_13_OR_LATER
	ast_json_free(log_line);
#else
	json_decref(log_message);
	ast_free(log_line);
#endif
}

static void libdialogflow_general_logging_callback(enum dialogflow_log_level level, const char *file, int line, const char *function, const char *fmt, va_list args)
{
	char *buff;
	va_list args2;
	va_copy(args2, args);
    size_t len = vsnprintf(NULL, 0, fmt, args2);
    va_end(args2);
    buff = alloca(len + 1);
    vsnprintf(buff, len + 1, fmt, args);

	ast_log((int) level, file, line, function, "%s", buff);
}

static void libdialogflow_call_logging_callback(void *user_data, const char *event, size_t log_data_size, const struct dialogflow_log_data *data)
{
	struct gdf_pvt *pvt = (struct gdf_pvt *) user_data;
	gdf_log_call_event(pvt, CALL_LOG_TYPE_DIALOGFLOW, event, log_data_size, data);
}

static char gdf_engine_name[] = "GoogleDFE";

static struct ast_speech_engine gdf_engine = {
	.name = gdf_engine_name,
	.create = gdf_create,
	.destroy = gdf_destroy,
	.load = gdf_load,
	.unload = gdf_unload,
	.activate = gdf_activate,
	.deactivate = gdf_deactivate,
	.write = gdf_write,
	.dtmf = gdf_dtmf,
	.start = gdf_start,
	.change = gdf_change,
#ifdef ASTERISK_13_OR_LATER
	.get_setting = gdf_get_setting,
#endif
	.change_results_type = gdf_change_results_type,
	.get = gdf_get_results
};

static enum ast_module_load_result load_module(void)
{
	struct gdf_config *cfg;

	config = ao2_container_alloc(1, NULL, NULL);
	if (!config) {
		ast_log(LOG_ERROR, "Failed to allocate configuration container\n");
		return AST_MODULE_LOAD_FAILURE;
	}

	cfg = ao2_alloc(sizeof(*cfg), gdf_config_destroy);
	if (!cfg) {
		ast_log(LOG_ERROR, "Failed to allocate blank configuration\n");
		ao2_ref(config, -1);
		return AST_MODULE_LOAD_FAILURE;
	}

	ao2_link(config, cfg);
	ao2_ref(cfg, -1);

	if (load_config(0)) {
		ast_log(LOG_WARNING, "Failed to load configuration\n");
	}

#ifdef ASTERISK_13_OR_LATER
	gdf_engine.formats = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);

	if (!gdf_engine.formats) {
		ast_log(LOG_ERROR, "DFE speech could not create format caps\n");
		ao2_ref(config, -1);
		return AST_MODULE_LOAD_FAILURE;
	}

	ast_format_cap_append(gdf_engine.formats, ast_format_ulaw, 20);
#else
	gdf_engine.formats = AST_FORMAT_SLINEAR;
#endif

	if (ast_speech_register(&gdf_engine)) {
		ast_log(LOG_WARNING, "DFE speech failed to register with speech subsystem\n");
		ao2_ref(config, -1);
		return AST_MODULE_LOAD_FAILURE;
	}

	if (df_init(libdialogflow_general_logging_callback, libdialogflow_call_logging_callback)) {
		ast_log(LOG_WARNING, "Failed to initialize dialogflow library\n");
		ao2_ref(config, -1);
		return AST_MODULE_LOAD_FAILURE;
	}

	ast_cli_register_multiple(gdfe_cli, ARRAY_LEN(gdfe_cli));

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	if (ast_speech_unregister(gdf_engine.name)) {
		ast_log(LOG_WARNING, "Failed to unregister GDF speech engine\n");
		return -1;
	}

	ast_cli_unregister_multiple(gdfe_cli, ARRAY_LEN(gdfe_cli));

#ifdef ASTERISK_13_OR_LATER
	ao2_t_ref(gdf_engine.formats, -1, "unloading module");
#endif

	return 0;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Google DialogFlow for Enterprise (DFE) Speech Engine");
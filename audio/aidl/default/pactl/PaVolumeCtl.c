#define LOG_TAG "audio_pa_volume"
//#define LOG_NDEBUG 0

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>

#include <log/log.h>
#include <pulse/pulseaudio.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>

#include "PaVolumeCtl.h"

struct pa_callback_args {
	pa_mainloop_api *mainloop_api;
	pa_context *context;
	enum action m_action;
	pa_cvolume m_volume;
	bool muted;
	bool need_info;
	pthread_mutex_t lock;
	int quit_count;
	int ret;
	char *sink_name;
	char *source_name;
	char *port_name;
	char *info;
};

static void quit(int ret, void *userdata)
{
    pa_assert(((struct pa_callback_args *)userdata)->mainloop_api);
    ((struct pa_callback_args *)userdata)->mainloop_api->quit(((struct pa_callback_args *)userdata)->mainloop_api, ret);
}

static void context_drain_complete(pa_context *c, void *userdata)
{
    pa_context_disconnect(c);
}

static void drain(void *userdata)
{
    pa_operation *o;
    if (!(o = pa_context_drain(((struct pa_callback_args *)userdata)->context, context_drain_complete, NULL))) {
        pa_context_disconnect(((struct pa_callback_args *)userdata)->context);
    } else {
        pa_operation_unref(o);
    }
}

static void fill_volume(pa_cvolume *cv, pa_cvolume *m_volume, unsigned supported)
{
    pa_cvolume_set(m_volume, supported, m_volume->values[0]);
    *cv = *m_volume;
}

static void simple_callback_lock(pa_context *c, int success, void *userdata)
{
    if (!success) {
        ALOGE("(%d)Failure: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }
	pthread_mutex_lock(&((struct pa_callback_args *)userdata)->lock);
	int quit_count = --((struct pa_callback_args *)userdata)->quit_count;
	pthread_mutex_unlock(&((struct pa_callback_args *)userdata)->lock);
	if (quit_count == 0) {
		drain(userdata);
	}
}

static void simple_callback(pa_context *c, int success, void *userdata)
{
    if (!success) {
        ALOGE("(%d)Failure: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }
    drain(userdata);
}

static void get_sink_info_callback(pa_context *c, const pa_sink_info *i, int is_last, void *userdata)
{
    pa_cvolume cv;
    if (is_last < 0) {
        ALOGE("(%d)Failed to get sink information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        return;
    }

    pa_assert(i);

    cv = i->volume;
    fill_volume(&cv, &((struct pa_callback_args *)userdata)->m_volume, i->channel_map.channels);

    pa_operation_unref(pa_context_set_sink_volume_by_name(c, ((struct pa_callback_args *)userdata)->sink_name, 
		&cv, simple_callback_lock, userdata));
}

static void get_source_volume_callback(pa_context *c, const pa_source_info *i, int is_last, void *userdata)
{
    pa_cvolume cv;
    if (is_last < 0) {
        ALOGE("(%d)Failed to get source information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        return;
    }

    pa_assert(i);

    cv = i->volume;
    fill_volume(&cv, &((struct pa_callback_args *)userdata)->m_volume, i->channel_map.channels);

    pa_operation_unref(pa_context_set_source_volume_by_name(c, ((struct pa_callback_args *)userdata)->source_name, &cv, 
		simple_callback_lock, userdata));
}

static void set_master_volume_callback(pa_context *c, const pa_server_info *i, void *userdata)
{
    if (!i) {
        ALOGE("(%d)Failed to get server information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    ALOGV(("Default Sink: %s\nDefault Source: %s\n"), i->default_sink_name, i->default_source_name);

    ((struct pa_callback_args *)userdata)->sink_name = pa_xstrdup(i->default_sink_name);
    ((struct pa_callback_args *)userdata)->source_name = pa_xstrdup(i->default_source_name);

    pa_operation_unref(pa_context_get_sink_info_by_name(c, ((struct pa_callback_args *)userdata)->sink_name, 
        get_sink_info_callback, userdata));
    pa_operation_unref(pa_context_get_source_info_by_name(c, ((struct pa_callback_args *)userdata)->source_name, 
        get_source_volume_callback, userdata));
}

static void get_mute_callback(pa_context *c, const pa_sink_info *i, int is_last, void *userdata)
{
    if (is_last < 0) {
        ALOGE("(%d)Failed to get sink information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        return;
    }

    pa_assert(i);

	ALOGV("Mute: %s, Mute: %d", pa_yes_no_localised(i->mute), i->mute);
	((struct pa_callback_args *)userdata)->ret = i->mute;
	drain(userdata);
}

static void get_sink_volume_callback(pa_context *c, const pa_sink_info *i, int is_last, void *userdata)
{
    if (is_last < 0) {
        ALOGE("(%d)Failed to get sink information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        return;
    }

    pa_assert(i);
	pa_volume_t volume = i->volume.values[0];

	ALOGV("Mute: %s ,Volume: %d",
		pa_yes_no_localised(i->mute),
		(unsigned)(((uint64_t)volume * 100 + (uint64_t)PA_VOLUME_NORM / 2) / (uint64_t)PA_VOLUME_NORM));
	((struct pa_callback_args *)userdata)->ret = (int)(((uint64_t)volume * 100 + (uint64_t)PA_VOLUME_NORM / 2) / (uint64_t)PA_VOLUME_NORM);
	drain(userdata);
}

static void get_master_volume_callback(pa_context *c, const pa_server_info *i, void *userdata)
{
    if (!i) {
        ALOGE("(%d)Failed to get server information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }
    ALOGV("Default Sink: %s", i->default_sink_name);
    ((struct pa_callback_args *)userdata)->sink_name = pa_xstrdup(i->default_sink_name);

    pa_operation_unref(pa_context_get_sink_info_by_name(c, ((struct pa_callback_args *)userdata)->sink_name, 
        get_sink_volume_callback, userdata));
}

static void set_master_mute_callback(pa_context *c, const pa_server_info *i, void *userdata)
{
    if (!i) {
        ALOGE("(%d)Failed to get server information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }
    ALOGV("Default Sink: %s ,Default Source: %s", i->default_sink_name, i->default_source_name);
    ((struct pa_callback_args *)userdata)->sink_name = pa_xstrdup(i->default_sink_name);
    ((struct pa_callback_args *)userdata)->source_name = pa_xstrdup(i->default_source_name);

    pa_operation_unref(pa_context_set_sink_mute_by_name(c, ((struct pa_callback_args *)userdata)->sink_name, 
        ((struct pa_callback_args *)userdata)->muted, simple_callback_lock, userdata));
    pa_operation_unref(pa_context_set_source_mute_by_name(c, ((struct pa_callback_args *)userdata)->source_name, 
        ((struct pa_callback_args *)userdata)->muted, simple_callback_lock, userdata));
}

static void get_master_mute_callback(pa_context *c, const pa_server_info *i, void *userdata)
{
    if (!i) {
        ALOGE("(%d)Failed to get server information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }
    ALOGV("Default Sink: %s", i->default_sink_name);
    ((struct pa_callback_args *)userdata)->sink_name = pa_xstrdup(i->default_sink_name);

    pa_operation_unref(pa_context_get_sink_info_by_name(c, ((struct pa_callback_args *)userdata)->sink_name, 
        get_mute_callback, userdata));
}

static void get_output_devs_get_sink_info_callback(pa_context *c, const pa_sink_info *i, 
int is_last, void *userdata)
{
    if (is_last < 0) {
        ALOGE("(%d)Failed to get source information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        drain(userdata);
        return;
    }

    pa_assert(i);

    if (i->ports == NULL) {
        return;
    }

    pa_sink_port_info **p;
    char result[1024] = {0};
    int count = 0;
    for (p = i->ports; *p; p++) {
        if ((*p)->available != PA_PORT_AVAILABLE_NO) {
            if (i->active_port && strcmp(i->active_port->name, (*p)->name) == 0) {
                pa_volume_t volume = i->volume.values[0];
                int vol = (int)(((uint64_t)volume * 100 + (uint64_t)PA_VOLUME_NORM / 2) / (uint64_t)PA_VOLUME_NORM);
                if (strlen(result) == 0) {
                    if (i->description) {
                        sprintf(result, "%s %s=%s(%s)=%f=%d", i->name, (*p)->name, (*p)->description, i->description, vol / 100.0, i->mute);
                    } else {
                        sprintf(result, "%s %s=%s=%f=%d", i->name, (*p)->name, (*p)->description, vol / 100.0, i->mute);
                    }
                } else {
                    char *buf = pa_xstrdup(result);
                    if (i->description) {
                        sprintf(result, "%s %s=%s(%s)=%f=%d;%s", i->name, (*p)->name, (*p)->description, i->description, vol / 100.0, i->mute, buf);
                    } else {
                        sprintf(result, "%s %s=%s=%f=%d;%s", i->name, (*p)->name, (*p)->description, vol / 100.0, i->mute, buf);
                    }
                    pa_xfree(buf);
                }
            } else {
                if (i->description) {
                    sprintf(result + strlen(result), "%s%s %s=%s(%s)",	count ? ";" : "", i->name, (*p)->name, (*p)->description, i->description);
                } else {
                    sprintf(result + strlen(result), "%s%s %s=%s",	count ? ";" : "", i->name, (*p)->name, (*p)->description);
                }
            }
            count++;
        }
    }
    if (((struct pa_callback_args *)userdata)->info) {
        char info[1024] = {0};
        bool find_default_sink = pa_startswith(result, ((struct pa_callback_args *)userdata)->sink_name);
        if (find_default_sink) {
            sprintf(info, "%s;%s", result, ((struct pa_callback_args *)userdata)->info);
        } else {
            sprintf(info, "%s;%s", ((struct pa_callback_args *)userdata)->info, result);
        }
        pa_xfree(((struct pa_callback_args *)userdata)->info);
        ((struct pa_callback_args *)userdata)->info = pa_xstrdup(info);
    } else {
        ((struct pa_callback_args *)userdata)->info = pa_xstrdup(result);
    }
}

static void get_output_devs_callback(pa_context *c, const pa_server_info *i, void *userdata)
{
    if (!i) {
        ALOGE("(%d)Failed to get server information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }
    ALOGV("Default Sink: %s", i->default_sink_name);
    ((struct pa_callback_args *)userdata)->sink_name = pa_xstrdup(i->default_sink_name);

    pa_operation_unref(pa_context_get_sink_info_list(c, get_output_devs_get_sink_info_callback, userdata));
}

static void set_output_dev_volume_callback(pa_context *c, const pa_sink_info *i, int is_last, void *userdata)
{
    pa_cvolume cv;
    if (is_last < 0) {
        ALOGE("(%d)Failed to get source information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        return;
    }

    pa_assert(i);

    cv = i->volume;
    fill_volume(&cv, &((struct pa_callback_args *)userdata)->m_volume, i->channel_map.channels);

    pa_operation_unref(pa_context_set_sink_volume_by_name(c, ((struct pa_callback_args *)userdata)->sink_name, 
        &cv, simple_callback, userdata));
}

static void get_output_default_dev_info_callback(pa_context *c, const pa_sink_info *i, int is_last, void *userdata)
{
    if (is_last < 0) {
        ALOGE("(%d)Failed to get source information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        drain(userdata);
        return;
    }

    pa_assert(i);

    if (i->ports == NULL) {
        return;
    }

    char info[1024] = {0};
    if (i->active_port && strcmp(i->active_port->name, ((struct pa_callback_args *)userdata)->port_name) == 0) {
        pa_volume_t volume = i->volume.values[0];
        int vol = (int)(((uint64_t)volume * 100 + (uint64_t)PA_VOLUME_NORM / 2) / (uint64_t)PA_VOLUME_NORM);
        sprintf(info, "%f=%d", vol / 100.0, i->mute);
        ((struct pa_callback_args *)userdata)->info = pa_xstrdup(info);
    }
}

static void set_default_sink_callback(pa_context *c, int success, void *userdata)
{
    if (!success) {
        ALOGE("(%d)Failure: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }
    pa_operation_unref(pa_context_get_sink_info_list(c, get_output_default_dev_info_callback, userdata));
}

static void set_sink_port_by_name_callback(pa_context *c, int success, void *userdata)
{
    if (!success) {
        ALOGE("Failure: %s", pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }
    pa_operation_unref(pa_context_set_default_sink(c, ((struct pa_callback_args *)userdata)->sink_name, 
        ((struct pa_callback_args *)userdata)->need_info ? set_default_sink_callback : simple_callback, userdata));
}

static void set_output_default_dev_get_sink_info_callback(pa_context *c, const pa_sink_info *i, int is_last, void *userdata)
{
    if (is_last < 0) {
        ALOGE("(%d)Failed to get source information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        if (((struct pa_callback_args *)userdata)->ret != 1) {
            drain(userdata);
        }
        ((struct pa_callback_args *)userdata)->ret = 0;
        return;
    }

    pa_assert(i);

    if (i->ports == NULL) {
        return;
    }

    pa_sink_port_info **p;
    int count = 0;
    for (p = i->ports; *p; p++) {
        if ((*p)->available != PA_PORT_AVAILABLE_NO) {
            count++;
        }
    }
    if (count == 1) {
        if (((struct pa_callback_args *)userdata)->need_info) {
            pa_operation_unref(pa_context_set_default_sink(c, ((struct pa_callback_args *)userdata)->sink_name, set_default_sink_callback, userdata));
        } else {
            pa_operation_unref(pa_context_set_default_sink(c, ((struct pa_callback_args *)userdata)->sink_name, simple_callback, userdata));
        }
        ((struct pa_callback_args *)userdata)->ret = 1;
    } else if (count > 1) {
        pa_operation_unref(pa_context_set_sink_port_by_name(c, ((struct pa_callback_args *)userdata)->sink_name,
            ((struct pa_callback_args *)userdata)->port_name, set_sink_port_by_name_callback, userdata));
        ((struct pa_callback_args *)userdata)->ret = 1;
    }
}

static void get_input_devs_source_info_by_name_callback(pa_context *c, const pa_source_info *i, int is_last, void *userdata)
{
    if (is_last < 0) {
        ALOGE("(%d)Failed to get source information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        drain(userdata);
        return;
    }

    pa_assert(i);

    const char *split_state = NULL;
    char *n = NULL;
    uint32_t info_count = 0;
    char info[1024] = {0};
    pa_volume_t volume = i->volume.values[0];
    int vol = (int)(((uint64_t)volume * 100 + (uint64_t)PA_VOLUME_NORM / 2) / (uint64_t)PA_VOLUME_NORM);
    if (strstr(((struct pa_callback_args *)userdata)->info, ";")) {
        const char *devs[128] = {0};
        char cmp_buf[1024] = {0};
        uint32_t dest_pos = 0;
        if (i->active_port) {
            sprintf(cmp_buf, " %s ", i->active_port->name);
        } else {
            char *first_dot = strstr(i->name, ".");
            if (first_dot) {
                snprintf(cmp_buf, strstr(first_dot + 1, ".") - first_dot + 1, "%s", first_dot);
            } else {
                sprintf(cmp_buf, " %s ", " ");
            }
        }
        while (n = pa_split(((struct pa_callback_args *)userdata)->info, ";", &split_state)) {
            devs[info_count] = n;
            if (strstr(n, cmp_buf)) {
                dest_pos = info_count;
            }
            info_count++;
        }
        sprintf(info, "%s=%f=%d", devs[dest_pos], vol / 100.0, i->mute);
        pa_xfree(devs[dest_pos]);
        int j;
        for (j = 0; j < info_count; j++) {
            if (j != dest_pos) {
                sprintf(info + strlen(info), ";%s", devs[j]);
                pa_xfree(devs[j]);
            }
        }
    } else {
        const char *profile_to_switch;
        const char *card_name;
        const char *port_name;
        bool have_bt = false;
        info_count = 0;
        split_state = NULL;
        while ((n = pa_split(((struct pa_callback_args *)userdata)->info, " ", &split_state))) {
            if (info_count == 0) {
                card_name = n;
            } else if (info_count == 1) {
                port_name = n;
            } else if (info_count == 3) {
                if (strstr(n, "=")) {
                    have_bt = true;
                    const char *split_state2 = NULL;
                    profile_to_switch = pa_split(n, "=", &split_state2);
                } else {
                    pa_xfree(n);
                }
            } else {
                pa_xfree(n);
            }
            info_count++;
        }
        if (have_bt) {
            if (i->active_port && strcmp(i->active_port->name, port_name) == 0) {
                sprintf(info, "%s=%f=%d;NONE%s %s=none=0.5=0", ((struct pa_callback_args *)userdata)->info, vol / 100.0, i->mute, card_name, profile_to_switch);
            } else {
                sprintf(info, "NONE%s %s=none=0.5=0;%s", card_name, profile_to_switch, ((struct pa_callback_args *)userdata)->info);
            }
        } else {
            sprintf(info, "%s=%f=%d", ((struct pa_callback_args *)userdata)->info, vol / 100.0, i->mute);
        }
        pa_xfree(card_name);
        pa_xfree(port_name);
        pa_xfree(profile_to_switch);
    }
    pa_xfree(((struct pa_callback_args *)userdata)->info);
    ((struct pa_callback_args *)userdata)->info = pa_xstrdup(info);
}

static void get_input_devs_source_info_by_name_master_callback(pa_context *c, const pa_source_info *i, int is_last, void *userdata)
{
    if (is_last < 0) {
        ALOGE("(%d)Failed to get source information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        return;
    }

    pa_assert(i);

    const char *master_device = pa_proplist_gets(i->proplist, "device.master_device");
    if (master_device) {
        ((struct pa_callback_args *)userdata)->source_name = pa_xstrdup(master_device);
        pa_operation_unref(pa_context_get_source_info_by_name(c, master_device, 
                        get_input_devs_source_info_by_name_callback, userdata));
    } else {
        pa_xfree(((struct pa_callback_args *)userdata)->info);
        ((struct pa_callback_args *)userdata)->info = pa_xstrdup("NONENONE=none=0.5=0");
        drain(userdata);
    }
}

static void get_input_devs_server_info_callback(pa_context *c, const pa_server_info *i, void *userdata)
{
    if (!i) {
        ALOGE("(%d)Failed to get server information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    ALOGV(("Default Source: %s\n"), i->default_source_name);

    char info[1024] = {0};
    const char *default_source_name_first_dot = strstr(i->default_source_name, ".");
    if (default_source_name_first_dot) {
        strncpy(info, default_source_name_first_dot, strstr(default_source_name_first_dot + 1, ".") - default_source_name_first_dot);
    }

    if (default_source_name_first_dot && strstr(((struct pa_callback_args *)userdata)->info, info)) {
        ((struct pa_callback_args *)userdata)->source_name = pa_xstrdup(i->default_source_name);
        pa_operation_unref(pa_context_get_source_info_by_name(c, i->default_source_name, 
                get_input_devs_source_info_by_name_callback, userdata));
    } else {
        pa_operation_unref(pa_context_get_source_info_by_name(c, i->default_source_name, 
                        get_input_devs_source_info_by_name_master_callback, userdata));
    }
}

static void get_input_devs_get_card_info_callback(pa_context *c, const pa_card_info *i, int is_last, void *userdata)
{
    char t[32];
    char *pl;

    if (is_last < 0) {
        ALOGE("(%d)Failed to get card information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        if (((struct pa_callback_args *)userdata)->info) {
            pa_operation_unref(pa_context_get_server_info(c, get_input_devs_server_info_callback, userdata));
        } else {
            ((struct pa_callback_args *)userdata)->info = pa_xstrdup("NONENONE=none=0.5=0");
            drain(userdata);
        }
        return;
    }

    pa_assert(i);

    if (i->ports == NULL) {
        return;
    }

    pa_card_port_info **p;
    char result[1024] = {0};
    int count = 0;
    int active_profile_count = 0;
    for (p = i->ports; *p; p++) {
        if ((*p)->available != PA_PORT_AVAILABLE_NO && (*p)->direction == PA_DIRECTION_INPUT) {
            const char *profile_to_switch = NULL;
            pa_card_profile_info **pr = (*p)->profiles;
            const char *device_bus = pa_proplist_gets(i->proplist, "device.bus");
            bool bluetooth = device_bus && strcmp(device_bus, "bluetooth") == 0;

            if (bluetooth) {
                pa_card_profile_info2 **p2;
                for (p2 = i->profiles2; *p2; p2++) {
                    if ((*p2)->name && strcmp((*p2)->name, (*pr)->name) != 0 && strcmp((*p2)->name, "off") != 0) {
                        profile_to_switch = (*p2)->name;
                        break;
                    }
                }
            }
            const char *device_description = pa_proplist_gets(i->proplist, "device.description");
            pa_card_profile_info **tmp_pr = pr;
            const char *better_profile = NULL;
            if (i->active_profile->name) {
                while (*tmp_pr) {
                    if (strcmp((*tmp_pr)->name, i->active_profile->name) == 0) {
                        better_profile = (*tmp_pr)->name;
                        break;
                    }
                    tmp_pr++;
                }
                if (better_profile == NULL) {
                    tmp_pr = pr;
                    better_profile = (*tmp_pr)->name;
                    tmp_pr++;
                    while (*tmp_pr) {
                        if (strrchr((*tmp_pr)->name, '+')) {
                            better_profile = (*tmp_pr)->name;
                            break;
                        }
                        tmp_pr++;
                    }
                }
            } else {
                ALOGE("(%d)not be here: %s", __LINE__);
            }
            if (device_description) {
                sprintf(result + strlen(result), "%s%s %s %s%s%s=%s(%s)",	count ? ";" : "", i->name, (*p)->name, better_profile, 
                    profile_to_switch ? " " : "", profile_to_switch ? profile_to_switch : "", (*p)->description, device_description);
            } else {
                sprintf(result + strlen(result), "%s%s %s %s%s%s=%s",	count ? ";" : "", i->name, (*p)->name, better_profile,
                    profile_to_switch ? " " : "", profile_to_switch ? profile_to_switch : "", (*p)->description);
            }
            count++;
        }
    }
    if ((strlen(result) != 0)) {
        if (((struct pa_callback_args *)userdata)->info) {
            char info[1024] = {0};
            sprintf(info, "%s;%s",	((struct pa_callback_args *)userdata)->info, result);
            pa_xfree(((struct pa_callback_args *)userdata)->info);
            ((struct pa_callback_args *)userdata)->info = pa_xstrdup(info);
        } else {
            ((struct pa_callback_args *)userdata)->info = pa_xstrdup(result);
        }
    }
}

static void set_input_default_dev_get_source_info_list_callback(pa_context *c, const pa_source_info *i, int is_last, void *userdata)
{
    if (is_last < 0) {
        ALOGE("(%d)Failed to get source information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        if (((struct pa_callback_args *)userdata)->ret != 1) {
            drain(userdata);
        }
        ((struct pa_callback_args *)userdata)->ret = 0;
        return;
    }

    pa_assert(i);

    if (i->ports == NULL) {
        return;
    }

    const char *bus_or_mac = strchr(((struct pa_callback_args *)userdata)->source_name, '.');
    if (!bus_or_mac) {
        ALOGE("(%d)args error", __LINE__);
        quit(1, userdata);
        return;
    }

    if (i->name && strstr(i->name, bus_or_mac)) {
        pa_volume_t volume = i->volume.values[0];
        int vv = (int)(((uint64_t)volume * 100 + (uint64_t)PA_VOLUME_NORM / 2) / (uint64_t)PA_VOLUME_NORM);
        char temp[1024] = {0};
        sprintf(temp, "%f=%d", vv / 100.0, i->mute);
        ((struct pa_callback_args *)userdata)->info = pa_xstrdup(temp);
        if (strcmp(i->active_port, ((struct pa_callback_args *)userdata)->port_name) != 0) {
            pa_operation_unref(pa_context_set_source_port_by_name(c, i->name,
                ((struct pa_callback_args *)userdata)->port_name, NULL, userdata));
        }
        pa_operation_unref(pa_context_set_default_source(c, i->name, simple_callback, userdata));
        ((struct pa_callback_args *)userdata)->ret = 1;
    }
}

static void set_input_default_dev_set_card_profile_by_name_callback(pa_context *c, int success, void *userdata)
{
    if (!success) {
        ALOGE("(%d)Failure: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }
    pa_operation_unref(pa_context_get_source_info_list(c, set_input_default_dev_get_source_info_list_callback, userdata));
}

static void set_input_vol_get_source_info_list_callback(pa_context *c, const pa_source_info *i, int is_last, void *userdata)
{
    if (is_last < 0) {
        ALOGE("(%d)Failed to get source information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        if (((struct pa_callback_args *)userdata)->ret != 1) {
            drain(userdata);
        }
        ((struct pa_callback_args *)userdata)->ret = 0;
        return;
    }

    pa_assert(i);

    if (i->ports == NULL) {
        return;
    }

    const char *bus_or_mac = strchr(((struct pa_callback_args *)userdata)->source_name, '.');
    if (!bus_or_mac) {
        ALOGE("(%d)args error", __LINE__);
        quit(1, userdata);
        return;
    }
    if (i->name && strstr(i->name, bus_or_mac) && i->active_port && strcmp(i->active_port->name, ((struct pa_callback_args *)userdata)->port_name) == 0) {
        pa_cvolume cv = i->volume;
        fill_volume(&cv, &((struct pa_callback_args *)userdata)->m_volume, i->channel_map.channels);
        pa_operation_unref(pa_context_set_source_volume_by_name(c, i->name, &cv, simple_callback, userdata));
        ((struct pa_callback_args *)userdata)->ret = 1;
    }
}

static void set_input_mute_get_source_info_list_callback(pa_context *c, const pa_source_info *i, int is_last, void *userdata)
{
    if (is_last < 0) {
        ALOGE("(%d)Failed to get source information: %s", __LINE__, pa_strerror(pa_context_errno(c)));
        quit(1, userdata);
        return;
    }

    if (is_last) {
        if (((struct pa_callback_args *)userdata)->ret != 1) {
            drain(userdata);
        }
        ((struct pa_callback_args *)userdata)->ret = 0;
        return;
    }

    pa_assert(i);

    if (i->ports == NULL) {
        return;
    }

    const char *bus_or_mac = strchr(((struct pa_callback_args *)userdata)->source_name, '.');
    if (!bus_or_mac) {
        ALOGE("(%d)args error", __LINE__);
        quit(1, userdata);
        return;
    }

    if (i->name && strstr(i->name, bus_or_mac) && i->active_port && strcmp(i->active_port->name, ((struct pa_callback_args *)userdata)->port_name) == 0) {
        pa_operation_unref(pa_context_set_source_mute_by_name(c, i->name, ((struct pa_callback_args *)userdata)->muted, simple_callback, userdata));
        ((struct pa_callback_args *)userdata)->ret = 1;
    }
}

static void context_state_callback(pa_context *c, void *userdata)
{
    pa_operation *o = NULL;
	enum action m_action = ((struct pa_callback_args *)userdata)->m_action;

    pa_assert(c);
	ALOGV("pa_context_get_state: %d, action: %d", pa_context_get_state(c), m_action);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;
        case PA_CONTEXT_READY:
            switch (m_action) {
				case SET_MASTER_VOLUME:
					((struct pa_callback_args *)userdata)->quit_count = 2;
					o = pa_context_get_server_info(c, set_master_volume_callback, userdata);
                    break;
				case GET_MASTER_VOLUME:
					o = pa_context_get_server_info(c, get_master_volume_callback, userdata);
                    break;
				case SET_MASTER_MUTE:
					((struct pa_callback_args *)userdata)->quit_count = 2;
                    o = pa_context_get_server_info(c, set_master_mute_callback, userdata);
                    break;
				case GET_MASTER_MUTE:
                    o = pa_context_get_server_info(c, get_master_mute_callback, userdata);
                    break;
                case GET_INPUT_DEVS:
					o = pa_context_get_card_info_list(c, get_input_devs_get_card_info_callback, userdata);
                    break;
                case GET_OUTPUT_DEVS:
                    o = pa_context_get_server_info(c, get_output_devs_callback, userdata);
                    break;
                case SET_INPUT_DEV_VOLUME:
					o = pa_context_get_source_info_list(c, set_input_vol_get_source_info_list_callback, userdata);
                    break;
                case SET_OUTPUT_DEV_VOLUME:
                    o = pa_context_get_sink_info_by_name(c, ((struct pa_callback_args *)userdata)->sink_name, 
                            set_output_dev_volume_callback, userdata);
                    break;
                case SET_INPUT_DEV_MUTE:
					o = pa_context_get_source_info_list(c, set_input_mute_get_source_info_list_callback, userdata);
                    break;
                case SET_OUTPUT_DEV_MUTE:
                    o = pa_context_set_sink_mute_by_name(c, ((struct pa_callback_args *)userdata)->sink_name, 
                            ((struct pa_callback_args *)userdata)->muted, simple_callback, userdata);
                    break;
                case SET_INPUT_DEFAULT_DEV: {
                    bool is_none = ((struct pa_callback_args *)userdata)->ret;
                    if (is_none) {
                        ((struct pa_callback_args *)userdata)->info = pa_xstrdup("0.5=0");
                    }
                    o = pa_context_set_card_profile_by_name(c, ((struct pa_callback_args *)userdata)->source_name, 
                            ((struct pa_callback_args *)userdata)->sink_name, is_none ? simple_callback :
                            set_input_default_dev_set_card_profile_by_name_callback, userdata);
                    break;
				}
                case SET_OUTPUT_DEFAULT_DEV:
                    o = pa_context_get_sink_info_by_name(c, ((struct pa_callback_args *)userdata)->sink_name, 
                            set_output_default_dev_get_sink_info_callback, userdata);
                    ((struct pa_callback_args *)userdata)->ret = 0;
                    break;
                default:
                    pa_assert_not_reached();
            }

            if (o) {
                pa_operation_unref(o);
            }
            break;

        case PA_CONTEXT_TERMINATED:
            quit(0, userdata);
            break;

        case PA_CONTEXT_FAILED:
        default:
		    ALOGE("Connection failure: %s", pa_strerror(pa_context_errno(c)));
            quit(1, userdata);
    }
}

static int pa_process(enum action action, struct pa_callback_args * callback_args)
{
	pa_mainloop *m = NULL;
	int ret = -1;
	char *server = NULL;

	if (!(m = pa_mainloop_new())) {
		ALOGE("pa_mainloop_new() failed.");
		goto quit;
	}

	callback_args->mainloop_api = pa_mainloop_get_api(m);

	if (!(callback_args->context = pa_context_new_with_proplist(callback_args->mainloop_api, NULL, NULL))) {
		ALOGE("pa_context_new() failed.");
		goto quit;
	}

	callback_args->m_action = action;
	pthread_mutex_init (&callback_args->lock, (const pthread_mutexattr_t *)NULL);

	pa_context_set_state_callback(callback_args->context, context_state_callback, callback_args);

	if (pa_context_connect(callback_args->context, server, 0, NULL) < 0) {
		ALOGE("pa_context_connect() failed: %s", pa_strerror(pa_context_errno(callback_args->context)));
		goto quit;
	}

	if (pa_mainloop_run(m, &ret) < 0) {
		ALOGE("pa_mainloop_run() failed.");
		goto quit;
	}
	ret = callback_args->ret;

quit:

	if (callback_args->context) {
		pa_context_unref(callback_args->context);
    }

	if (m) {
		pa_mainloop_free(m);
	}

	pa_xfree(server);
	pa_xfree(callback_args->sink_name);
	pa_xfree(callback_args->source_name);
	pa_xfree(callback_args->port_name);
	pthread_mutex_destroy(&callback_args->lock);

	return ret;
}

static int pa_set_dev_volume(bool input, const char *dev_name, float volume)
{
    struct pa_callback_args callback_args;
    const char *split_state = NULL;
    char *n = NULL;
    int split_count = 0;
    if (input) {
        if (!dev_name) {
            return -1;
        }
        if (strncmp(dev_name, "NONE", strlen("NONE")) == 0) {
            return 0;
        } else {
            while ((n = pa_split(dev_name, " ", &split_state))) {
                if (split_count == 0) {
                    callback_args.source_name = n;
                } else if (split_count == 1) {
                    callback_args.port_name = n;
                } else {
                    pa_xfree(n);
                    break;
                }
                split_count++;
            }
        }
    } else {
        char *name = NULL;
        name = pa_split(dev_name, " ", &split_state);
        if (name[0] != '\0') {
            callback_args.sink_name = name;
        } else {
            pa_xfree(name);
            ALOGE("parse dev_name fail");
            return -1;
        }
    }

    callback_args.m_volume.channels = 1;
    callback_args.m_volume.values[0] = (pa_volume_t)((int)(volume * 100) * (double) PA_VOLUME_NORM / 100);
    return pa_process(input ? SET_INPUT_DEV_VOLUME : SET_OUTPUT_DEV_VOLUME, &callback_args);
}

static int pa_set_dev_mute(bool input, const char *dev_name, bool mute)
{
    struct pa_callback_args callback_args;
    const char *split_state = NULL;
    char *n = NULL;
    int split_count = 0;

    if (input) {
        if (!dev_name) {
            return -1;
        }
        if (strncmp(dev_name, "NONE", strlen("NONE")) == 0) {
            return 0;
        } else {
            while ((n = pa_split(dev_name, " ", &split_state))) {
                if (split_count == 0) {
                    callback_args.source_name = n;
                } else if (split_count == 1) {
                    callback_args.port_name = n;
                } else {
                    pa_xfree(n);
                    break;
                }
                split_count++;
            }
        }
    } else {
        char *name = NULL;
        name = pa_split(dev_name, " ", &split_state);
        if (name[0] != '\0') {
            callback_args.sink_name = name;
        } else {
            pa_xfree(name);
            ALOGE("parse dev_name fail");
            return -1;
        }
    }

    callback_args.muted = mute;
    return pa_process(input ? SET_INPUT_DEV_MUTE : SET_OUTPUT_DEV_MUTE, &callback_args);
}

static char *pa_set_default_dev(bool input, const char *dev_name, bool need_info)
{
    struct pa_callback_args callback_args;
    callback_args.need_info = need_info;
    const char *split_state = NULL;
    char *n = NULL;
    int split_count = 0;

    if (input) {
        if (!dev_name) {
            return pa_xstrdup("1");
        }
        char card_profile[256] = {0};
        if (strncmp(dev_name, "NONENONE", strlen("NONENONE")) == 0) {
            return pa_xstrdup(need_info ? "0.5=0" : "0");
        } else if (strncmp(dev_name, "NONE", strlen("NONE")) == 0) {
            sprintf(card_profile, "%s", dev_name + strlen("NONE"));
            while ((n = pa_split(card_profile, " ", &split_state))) {
                if (split_count == 0) {
                    callback_args.source_name = n;
                } else if (split_count == 1) {
                    callback_args.sink_name = n;
                } else {
                    pa_xfree(n);
                    ALOGE("(%d): %s", __LINE__, "more args!!");
                    break;
                }
                split_count++;
            }
            callback_args.ret = 1;
        } else {
            while ((n = pa_split(dev_name, " ", &split_state))) {
                if (split_count == 0) {
                    callback_args.source_name = n;
                } else if (split_count == 1) {
                    callback_args.port_name = n;
                } else if (split_count == 2) {
                    callback_args.sink_name = n;
                } else {
                    pa_xfree(n);
                    break;
                }
                split_count++;
            }
        }
    } else {
        while ((n = pa_split(dev_name, " ", &split_state))) {
            if (split_count == 0) {
                callback_args.sink_name = n;
            } else if (split_count == 1) {
                callback_args.port_name = n;
            } else {
                pa_xfree(n);
                ALOGE("(%d): %s", __LINE__, "more args!!");
                break;
            }
            split_count++;
        }
    }
    int ret = pa_process(input ? SET_INPUT_DEFAULT_DEV : SET_OUTPUT_DEFAULT_DEV, &callback_args);
    return ret ? pa_xstrdup("1") : callback_args.need_info ? callback_args.info : pa_xstrdup("0");
}

static char *pa_get_devs(bool input)
{
    struct pa_callback_args callback_args;
    int ret = pa_process(input ? GET_INPUT_DEVS : GET_OUTPUT_DEVS, &callback_args);
    return ret ? pa_xstrdup("") : callback_args.info;
}

int pa_set_master_volume(float volume)
{
    struct pa_callback_args callback_args;
    callback_args.m_volume.channels = 1;
    callback_args.m_volume.values[0] = (pa_volume_t)((int)(volume * 100) * (double) PA_VOLUME_NORM / 100);

    return pa_process(SET_MASTER_VOLUME, &callback_args);
}

int pa_get_master_volume(float *volume)
{
    struct pa_callback_args callback_args;
    int ret = pa_process(GET_MASTER_VOLUME, &callback_args);
    if (ret < 0) {
        return ret;
    }
	
    *volume = ret / 100.0;
    ALOGV("volume: %f", *volume);
    return 0;
}

int pa_set_master_mute(bool muted)
{
    struct pa_callback_args callback_args;
    callback_args.muted = muted;
    return pa_process(SET_MASTER_MUTE, &callback_args);
}

int pa_get_master_mute(bool *muted)
{
    struct pa_callback_args callback_args;
    int ret = pa_process(GET_MASTER_MUTE, &callback_args);
    if (ret < 0) {
        return ret;
    }

    *muted = ret;
    ALOGV("muted: %d", ret);
    return 0;
}

char *pa_get_input_devs()
{
    return pa_get_devs(true);
}

char *pa_get_output_devs()
{
    return pa_get_devs(false);
}

int pa_set_input_dev_volume(const char *dev_name, float volume)
{
    return pa_set_dev_volume(true, dev_name, volume);
}

int pa_set_output_dev_volume(const char *dev_name, float volume)
{
    return pa_set_dev_volume(false, dev_name, volume);
}

int pa_set_input_dev_mute(const char *dev_name, bool mute)
{
    return pa_set_dev_mute(true, dev_name, mute);
}

int pa_set_output_dev_mute(const char *dev_name, bool mute)
{
    return pa_set_dev_mute(false, dev_name, mute);
}

char *pa_set_input_default_dev(const char *dev_name, bool need_info)
{
    return pa_set_default_dev(true, dev_name, need_info);
}

char *pa_set_output_default_dev(const char *dev_name, bool need_info)
{
    return pa_set_default_dev(false, dev_name, need_info);
}

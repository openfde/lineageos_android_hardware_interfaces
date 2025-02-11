#ifndef PA_VOLUME_CTL_H
#define PA_VOLUME_CTL_H
enum action {
    SET_MASTER_VOLUME,
    GET_MASTER_VOLUME,
    SET_MASTER_MUTE,
    GET_MASTER_MUTE,
    GET_INPUT_DEVS,
    GET_OUTPUT_DEVS,
    SET_INPUT_DEV_VOLUME,
    SET_OUTPUT_DEV_VOLUME,
    SET_INPUT_DEV_MUTE,
    SET_OUTPUT_DEV_MUTE,
    SET_INPUT_DEFAULT_DEV,
    SET_OUTPUT_DEFAULT_DEV
};
#ifdef __cplusplus
extern "C" {
#endif

int pa_set_master_volume(float volume);
int pa_get_master_volume(float *volume);
int pa_set_master_mute(bool muted);
int pa_get_master_mute(bool *muted);
char *pa_get_input_devs();
char *pa_get_output_devs();
int pa_set_input_dev_volume(const char *dev_name, float volume);
int pa_set_output_dev_volume(const char *dev_name, float volume);
int pa_set_input_dev_mute(const char *dev_name, bool mute);
int pa_set_output_dev_mute(const char *dev_name, bool mute);
char *pa_set_input_default_dev(const char *dev_name, bool needInfo);
char *pa_set_output_default_dev(const char *dev_name, bool needInfo);
#ifdef __cplusplus
}
#endif

#endif

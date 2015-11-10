/*
 * Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 * Not a contribution.
 *
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "msm8960_platform"
/*#define LOG_NDEBUG 0*/
#define LOG_NDDEBUG 0

#include <stdlib.h>
#include <dlfcn.h>
#include <cutils/log.h>
#include <cutils/properties.h>
#include <audio_hw.h>
#include <platform_api.h>
#include "platform.h"
#include "audio_extn.h"

#define LIB_ACDB_LOADER "libacdbloader.so"
#define LIB_CSD_CLIENT "libcsd-client.so"

/*
 * This is the sysfs path for the HDMI audio data block
 */
#define AUDIO_DATA_BLOCK_PATH "/sys/class/graphics/fb1/audio_data_block"
#define MIXER_XML_PATH "/system/etc/mixer_paths.xml"

/*
 * This file will have a maximum of 38 bytes:
 *
 * 4 bytes: number of audio blocks
 * 4 bytes: total length of Short Audio Descriptor (SAD) blocks
 * Maximum 10 * 3 bytes: SAD blocks
 */
#define MAX_SAD_BLOCKS      10
#define SAD_BLOCK_SIZE      3

/* EDID format ID for LPCM audio */
#define EDID_FORMAT_LPCM    1

struct audio_block_header
{
    int reserved;
    int length;
};


typedef void (*acdb_deallocate_t)();
typedef int  (*acdb_init_t)();
typedef void (*acdb_send_audio_cal_t)(int, int);
typedef void (*acdb_send_voice_cal_t)(int, int);

typedef int (*csd_client_init_t)();
typedef int (*csd_client_deinit_t)();
typedef int (*csd_disable_device_t)();
typedef int (*csd_enable_device_t)(int, int, uint32_t);
#ifndef MULTI_VOICE_SESSION_ENABLED
typedef int (*csd_volume_t)(int);
typedef int (*csd_mic_mute_t)(int);
typedef int (*csd_start_voice_t)();
typedef int (*csd_stop_voice_t)();
#else
typedef int (*csd_volume_t)(uint32_t, int);
typedef int (*csd_mic_mute_t)(uint32_t, int);
typedef int (*csd_start_voice_t)(uint32_t);
typedef int (*csd_stop_voice_t)(uint32_t);
#endif


struct platform_data {
    struct audio_device *adev;
    bool fluence_in_spkr_mode;
    bool fluence_in_voice_call;
    bool fluence_in_voice_rec;
    int  fluence_type;
    int  dualmic_config;

    void *hw_info;

    /* Audio calibration related functions */
    void *acdb_handle;
    acdb_init_t acdb_init;
    acdb_deallocate_t acdb_deallocate;
    acdb_send_audio_cal_t acdb_send_audio_cal;
    acdb_send_voice_cal_t acdb_send_voice_cal;

    /* CSD Client related functions for voice call */
    void *csd_client;
    csd_client_init_t csd_client_init;
    csd_client_deinit_t csd_client_deinit;
    csd_disable_device_t csd_disable_device;
    csd_enable_device_t csd_enable_device;
    csd_volume_t csd_volume;
    csd_mic_mute_t csd_mic_mute;
    csd_start_voice_t csd_start_voice;
    csd_stop_voice_t csd_stop_voice;
};

static const int pcm_device_table[AUDIO_USECASE_MAX][2] = {
    [USECASE_AUDIO_PLAYBACK_DEEP_BUFFER] = {0, 0},
    [USECASE_AUDIO_PLAYBACK_LOW_LATENCY] = {14, 14},
    [USECASE_AUDIO_PLAYBACK_MULTI_CH] = {1, 1},
    [USECASE_AUDIO_RECORD] = {0, 0},
    [USECASE_AUDIO_RECORD_LOW_LATENCY] = {14, 14},
    [USECASE_VOICE_CALL] = {12, 12},
    [USECASE_AUDIO_PLAYBACK_FM] = {5, 6},
};

/* Array to store sound devices */
static const char * const device_table[SND_DEVICE_MAX] = {
    [SND_DEVICE_NONE] = "none",
    /* Playback sound devices */
    [SND_DEVICE_OUT_HANDSET] = "handset",
    [SND_DEVICE_OUT_SPEAKER] = "speaker",
    [SND_DEVICE_OUT_SPEAKER_REVERSE] = "speaker-reverse",
    [SND_DEVICE_OUT_HEADPHONES] = "headphones",
    [SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES] = "speaker-and-headphones",
    [SND_DEVICE_OUT_VOICE_SPEAKER] = "voice-speaker",
    [SND_DEVICE_OUT_VOICE_HEADPHONES] = "voice-headphones",
    [SND_DEVICE_OUT_HDMI] = "hdmi",
    [SND_DEVICE_OUT_SPEAKER_AND_HDMI] = "speaker-and-hdmi",
    [SND_DEVICE_OUT_BT_SCO] = "bt-sco-headset",
    [SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES] = "voice-tty-full-headphones",
    [SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES] = "voice-tty-vco-headphones",
    [SND_DEVICE_OUT_VOICE_TTY_HCO_HANDSET] = "voice-tty-hco-handset",
    [SND_DEVICE_OUT_TRANSMISSION_FM] = "transmission-fm",
    [SND_DEVICE_OUT_AFE_PROXY] = "afe-proxy",
    [SND_DEVICE_OUT_USB_HEADSET] = "usb-headphones",
    [SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET] = "speaker-and-usb-headphones",

    /* Capture sound devices */
    [SND_DEVICE_IN_HANDSET_MIC] = "handset-mic",
    [SND_DEVICE_IN_SPEAKER_MIC] = "speaker-mic",
    [SND_DEVICE_IN_HEADSET_MIC] = "headset-mic",
    [SND_DEVICE_IN_HANDSET_MIC_AEC] = "handset-mic",
    [SND_DEVICE_IN_SPEAKER_MIC_AEC] = "voice-speaker-mic",
    [SND_DEVICE_IN_HEADSET_MIC_AEC] = "headset-mic",
    [SND_DEVICE_IN_VOICE_SPEAKER_MIC] = "voice-speaker-mic",
    [SND_DEVICE_IN_VOICE_HEADSET_MIC] = "voice-headset-mic",
    [SND_DEVICE_IN_HDMI_MIC] = "hdmi-mic",
    [SND_DEVICE_IN_BT_SCO_MIC] = "bt-sco-mic",
    [SND_DEVICE_IN_CAMCORDER_MIC] = "camcorder-mic",
    [SND_DEVICE_IN_VOICE_DMIC] = "voice-dmic-ef",
    [SND_DEVICE_IN_VOICE_SPEAKER_DMIC] = "voice-speaker-dmic-ef",
    [SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC] = "voice-tty-full-headset-mic",
    [SND_DEVICE_IN_VOICE_TTY_VCO_HANDSET_MIC] = "voice-tty-vco-handset-mic",
    [SND_DEVICE_IN_VOICE_TTY_HCO_HEADSET_MIC] = "voice-tty-hco-headset-mic",
    [SND_DEVICE_IN_VOICE_REC_MIC] = "voice-rec-mic",
    [SND_DEVICE_IN_VOICE_REC_DMIC] = "voice-rec-dmic-ef",
    [SND_DEVICE_IN_VOICE_REC_DMIC_FLUENCE] = "voice-rec-dmic-ef-fluence",
    [SND_DEVICE_IN_CAPTURE_FM] = "capture-fm",
    [SND_DEVICE_IN_USB_HEADSET_MIC] = "usb-headset-mic",
};

/* ACDB IDs (audio DSP path configuration IDs) for each sound device */
static const int acdb_device_table[SND_DEVICE_MAX] = {
    [SND_DEVICE_NONE] = -1,
    [SND_DEVICE_OUT_HANDSET] = 7,
    [SND_DEVICE_OUT_SPEAKER] = 14,
    [SND_DEVICE_OUT_SPEAKER_REVERSE] = 14,
    [SND_DEVICE_OUT_HEADPHONES] = 10,
    [SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES] = 10,
    [SND_DEVICE_OUT_VOICE_SPEAKER] = 14,
    [SND_DEVICE_OUT_VOICE_HEADPHONES] = 10,
    [SND_DEVICE_OUT_HDMI] = 18,
    [SND_DEVICE_OUT_SPEAKER_AND_HDMI] = 14,
    [SND_DEVICE_OUT_BT_SCO] = 22,
    [SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES] = 17,
    [SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES] = 17,
    [SND_DEVICE_OUT_VOICE_TTY_HCO_HANDSET] = 37,
    [SND_DEVICE_OUT_TRANSMISSION_FM] = 0,
    [SND_DEVICE_OUT_AFE_PROXY] = 0,
    [SND_DEVICE_OUT_USB_HEADSET] = 22,
    [SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET] = 22,

    [SND_DEVICE_IN_HANDSET_MIC] = 4,
    [SND_DEVICE_IN_SPEAKER_MIC] = 4,
    [SND_DEVICE_IN_HEADSET_MIC] = 8,
    [SND_DEVICE_IN_HANDSET_MIC_AEC] = 40,
    [SND_DEVICE_IN_SPEAKER_MIC_AEC] = 42,
    [SND_DEVICE_IN_HEADSET_MIC_AEC] = 47,
    [SND_DEVICE_IN_VOICE_SPEAKER_MIC] = 11,
    [SND_DEVICE_IN_VOICE_HEADSET_MIC] = 8,
    [SND_DEVICE_IN_HDMI_MIC] = 4,
    [SND_DEVICE_IN_BT_SCO_MIC] = 21,
    [SND_DEVICE_IN_CAMCORDER_MIC] = 61,
    [SND_DEVICE_IN_VOICE_DMIC] = 6,
    [SND_DEVICE_IN_VOICE_SPEAKER_DMIC] = 13,
    [SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC] = 16,
    [SND_DEVICE_IN_VOICE_TTY_VCO_HANDSET_MIC] = 36,
    [SND_DEVICE_IN_VOICE_TTY_HCO_HEADSET_MIC] = 16,
    [SND_DEVICE_IN_VOICE_REC_MIC] = 62,
    /* TODO: Update with proper acdb ids */
    [SND_DEVICE_IN_VOICE_REC_DMIC] = 62,
    [SND_DEVICE_IN_VOICE_REC_DMIC_FLUENCE] = 6,
    [SND_DEVICE_IN_CAPTURE_FM] = 0,
    [SND_DEVICE_IN_USB_HEADSET_MIC] = 21,
};

#define DEEP_BUFFER_PLATFORM_DELAY (29*1000LL)
#define LOW_LATENCY_PLATFORM_DELAY (13*1000LL)

static void set_echo_reference(struct audio_device *adev, bool enable)
{
    if (enable)
        audio_route_apply_and_update_path(adev->audio_route, "echo-reference");
    else
        audio_route_reset_and_update_path(adev->audio_route, "echo-reference");

    ALOGV("Setting EC Reference: %d", enable);
}

void *platform_init(struct audio_device *adev)
{
    char platform[PROPERTY_VALUE_MAX];
    char baseband[PROPERTY_VALUE_MAX];
    char value[PROPERTY_VALUE_MAX];
    struct platform_data *my_data;
    const char *snd_card_name;

    adev->mixer = mixer_open(MIXER_CARD);

    if (!adev->mixer) {
        ALOGE("Unable to open the mixer, aborting.");
        return NULL;
    }

    adev->audio_route = audio_route_init(MIXER_CARD, MIXER_XML_PATH);
    if (!adev->audio_route) {
        ALOGE("%s: Failed to init audio route controls, aborting.", __func__);
        return NULL;
    }

    my_data = calloc(1, sizeof(struct platform_data));

    snd_card_name = mixer_get_name(adev->mixer);
    my_data->hw_info = hw_info_init(snd_card_name);
    if (!my_data->hw_info) {
        ALOGE("%s: Failed to init hardware info", __func__);
    }

    my_data->adev = adev;
    my_data->fluence_in_spkr_mode = false;
    my_data->fluence_in_voice_call = false;
    my_data->fluence_in_voice_rec = false;
    my_data->fluence_type = FLUENCE_NONE;

    property_get("ro.qc.sdk.audio.fluencetype", value, "");
    if (!strncmp("fluencepro", value, sizeof("fluencepro"))) {
        my_data->fluence_type = FLUENCE_QUAD_MIC;
    } else if (!strncmp("fluence", value, sizeof("fluence"))) {
        my_data->fluence_type = FLUENCE_DUAL_MIC;
    } else {
        my_data->fluence_type = FLUENCE_NONE;
    }

    if (my_data->fluence_type != FLUENCE_NONE) {
        property_get("persist.audio.fluence.voicecall",value,"");
        if (!strncmp("true", value, sizeof("true"))) {
            my_data->fluence_in_voice_call = true;
        }

        property_get("persist.audio.fluence.voicerec",value,"");
        if (!strncmp("true", value, sizeof("true"))) {
            my_data->fluence_in_voice_rec = true;
        }

        property_get("persist.audio.fluence.speaker",value,"");
        if (!strncmp("true", value, sizeof("true"))) {
            my_data->fluence_in_spkr_mode = true;
        }
    }

    my_data->acdb_handle = dlopen(LIB_ACDB_LOADER, RTLD_NOW);
    if (my_data->acdb_handle == NULL) {
        ALOGE("%s: DLOPEN failed for %s", __func__, LIB_ACDB_LOADER);
    } else {
        ALOGV("%s: DLOPEN successful for %s", __func__, LIB_ACDB_LOADER);
        my_data->acdb_deallocate = (acdb_deallocate_t)dlsym(my_data->acdb_handle,
                                                    "acdb_loader_deallocate_ACDB");
        my_data->acdb_send_audio_cal = (acdb_send_audio_cal_t)dlsym(my_data->acdb_handle,
                                                    "acdb_loader_send_audio_cal");
        if (!my_data->acdb_send_audio_cal)
            ALOGW("%s: Could not find the symbol acdb_send_audio_cal from %s",
                  __func__, LIB_ACDB_LOADER);
        my_data->acdb_send_voice_cal = (acdb_send_voice_cal_t)dlsym(my_data->acdb_handle,
                                                    "acdb_loader_send_voice_cal");
        my_data->acdb_init = (acdb_init_t)dlsym(my_data->acdb_handle,
                                                    "acdb_loader_init_ACDB");
        if (my_data->acdb_init == NULL)
            ALOGE("%s: dlsym error %s for acdb_loader_init_ACDB", __func__, dlerror());
        else
            my_data->acdb_init();
    }

    /* If platform is Fusion3, load CSD Client specific symbols
     * Voice call is handled by MDM and apps processor talks to
     * MDM through CSD Client
     */
    property_get("ro.board.platform", platform, "");
    property_get("ro.baseband", baseband, "");
    if (!strcmp("msm8960", platform) && !strcmp("mdm", baseband)) {
        my_data->csd_client = dlopen(LIB_CSD_CLIENT, RTLD_NOW);
        if (my_data->csd_client == NULL)
            ALOGE("%s: DLOPEN failed for %s", __func__, LIB_CSD_CLIENT);
    }

    if (my_data->csd_client) {
        ALOGV("%s: DLOPEN successful for %s", __func__, LIB_CSD_CLIENT);
        my_data->csd_client_deinit = (csd_client_deinit_t)dlsym(my_data->csd_client,
                                                    "csd_client_deinit");
        my_data->csd_disable_device = (csd_disable_device_t)dlsym(my_data->csd_client,
                                                    "csd_client_disable_device");
        my_data->csd_enable_device = (csd_enable_device_t)dlsym(my_data->csd_client,
                                                    "csd_client_enable_device");
        my_data->csd_start_voice = (csd_start_voice_t)dlsym(my_data->csd_client,
                                                    "csd_client_start_voice");
        my_data->csd_stop_voice = (csd_stop_voice_t)dlsym(my_data->csd_client,
                                                    "csd_client_stop_voice");
        my_data->csd_volume = (csd_volume_t)dlsym(my_data->csd_client,
                                                    "csd_client_volume");
        my_data->csd_mic_mute = (csd_mic_mute_t)dlsym(my_data->csd_client,
                                                    "csd_client_mic_mute");
        my_data->csd_client_init = (csd_client_init_t)dlsym(my_data->csd_client,
                                                    "csd_client_init");

        if (my_data->csd_client_init == NULL) {
            ALOGE("%s: dlsym error %s for csd_client_init", __func__, dlerror());
        } else {
            my_data->csd_client_init();
        }
    }

    /* init usb */
    audio_extn_usb_init(adev);

    return my_data;
}

void platform_deinit(void *platform)
{
    struct platform_data *my_data = (struct platform_data *)platform;

    hw_info_deinit(my_data->hw_info);
    free(platform);
    /* deinit usb */
    audio_extn_usb_deinit();
}

const char *platform_get_snd_device_name(snd_device_t snd_device)
{
    if (snd_device >= SND_DEVICE_MIN && snd_device < SND_DEVICE_MAX)
        return device_table[snd_device];
    else
        return "";
}

int platform_get_snd_device_name_extn(void *platform, snd_device_t snd_device,
                                      char *device_name)
{
    struct platform_data *my_data = (struct platform_data *)platform;

    if (snd_device >= SND_DEVICE_MIN && snd_device < SND_DEVICE_MAX) {
        strlcpy(device_name, device_table[snd_device], DEVICE_NAME_MAX_SIZE);
        hw_info_append_hw_type(my_data->hw_info, snd_device, device_name);
    } else {
        strlcpy(device_name, "", DEVICE_NAME_MAX_SIZE);
        return -EINVAL;
    }

    return 0;
}

void platform_add_backend_name(char *mixer_path, snd_device_t snd_device)
{
    if (snd_device == SND_DEVICE_IN_BT_SCO_MIC)
        strlcat(mixer_path, " bt-sco", MIXER_PATH_MAX_LENGTH);
    else if (snd_device == SND_DEVICE_IN_BT_SCO_MIC_WB)
        strlcat(mixer_path, " bt-sco-wb", MIXER_PATH_MAX_LENGTH);
    else if(snd_device == SND_DEVICE_OUT_BT_SCO)
        strlcat(mixer_path, " bt-sco", MIXER_PATH_MAX_LENGTH);
    else if(snd_device == SND_DEVICE_OUT_BT_SCO_WB)
        strlcat(mixer_path, " bt-sco-wb", MIXER_PATH_MAX_LENGTH);
    else if (snd_device == SND_DEVICE_OUT_HDMI)
        strlcat(mixer_path, " hdmi", MIXER_PATH_MAX_LENGTH);
    else if (snd_device == SND_DEVICE_OUT_SPEAKER_AND_HDMI)
        strlcat(mixer_path, " speaker-and-hdmi", MIXER_PATH_MAX_LENGTH);
    else if (snd_device == SND_DEVICE_IN_CAPTURE_FM)
        strlcat(mixer_path, " capture-fm", MIXER_PATH_MAX_LENGTH);
    else if (snd_device == SND_DEVICE_OUT_TRANSMISSION_FM)
        strlcat(mixer_path, " transmission-fm", MIXER_PATH_MAX_LENGTH);
    else if (snd_device == SND_DEVICE_OUT_USB_HEADSET)
        strlcat(mixer_path, " usb-headphones", MIXER_PATH_MAX_LENGTH);
    else if (snd_device == SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET)
        strlcat(mixer_path, " speaker-and-usb-headphones",
                MIXER_PATH_MAX_LENGTH);
    else if (snd_device == SND_DEVICE_IN_USB_HEADSET_MIC)
        strlcat(mixer_path, " usb-headset-mic", MIXER_PATH_MAX_LENGTH);
}

int platform_get_pcm_device_id(audio_usecase_t usecase, int device_type)
{
    int device_id;
    if (device_type == PCM_PLAYBACK)
        device_id = pcm_device_table[usecase][0];
    else
        device_id = pcm_device_table[usecase][1];
    return device_id;
}

int platform_get_snd_device_index(char *snd_device_index_name)
{
    return -ENODEV;
}

int platform_set_snd_device_acdb_id(snd_device_t snd_device, unsigned int acdb_id)
{
    return -ENODEV;
}

int platform_send_audio_calibration(void *platform, snd_device_t snd_device)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    int acdb_dev_id, acdb_dev_type;

    acdb_dev_id = acdb_device_table[snd_device];
    if (acdb_dev_id < 0) {
        ALOGE("%s: Could not find acdb id for device(%d)",
              __func__, snd_device);
        return -EINVAL;
    }
    if (my_data->acdb_send_audio_cal) {
        ("%s: sending audio calibration for snd_device(%d) acdb_id(%d)",
              __func__, snd_device, acdb_dev_id);
        if (snd_device >= SND_DEVICE_OUT_BEGIN &&
                snd_device < SND_DEVICE_OUT_END)
            acdb_dev_type = ACDB_DEV_TYPE_OUT;
        else
            acdb_dev_type = ACDB_DEV_TYPE_IN;
        my_data->acdb_send_audio_cal(acdb_dev_id, acdb_dev_type);
    }
    return 0;
}

int platform_switch_voice_call_device_pre(void *platform)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    int ret = 0;

    if (my_data->csd_client != NULL) {
        /* This must be called before disabling the mixer controls on APQ side */
        if (my_data->csd_disable_device == NULL) {
            ALOGE("%s: dlsym error for csd_disable_device", __func__);
        } else {
            ret = my_data->csd_disable_device();
            if (ret < 0) {
                ALOGE("%s: csd_client_disable_device, failed, error %d",
                      __func__, ret);
            }
        }
    }
    return ret;
}

int platform_switch_voice_call_device_post(void *platform,
                                           snd_device_t out_snd_device,
                                           snd_device_t in_snd_device)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    int acdb_rx_id, acdb_tx_id;
    int ret = 0;

    if (my_data->csd_client) {
        if (my_data->csd_enable_device == NULL) {
            ALOGE("%s: dlsym error for csd_enable_device",
                  __func__);
        } else {
            acdb_rx_id = acdb_device_table[out_snd_device];
            acdb_tx_id = acdb_device_table[in_snd_device];

            if (acdb_rx_id > 0 || acdb_tx_id > 0) {
                ret = my_data->csd_enable_device(acdb_rx_id, acdb_tx_id,
                                                    my_data->adev->acdb_settings);
                if (ret < 0) {
                    ALOGE("%s: csd_enable_device, failed, error %d",
                          __func__, ret);
                }
            } else {
                ALOGE("%s: Incorrect ACDB IDs (rx: %d tx: %d)", __func__,
                      acdb_rx_id, acdb_tx_id);
            }
        }
    }

    return ret;
}

int platform_start_voice_call(void *platform, uint32_t vsid)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    int ret = 0;

    if (my_data->csd_client) {
        if (my_data->csd_start_voice == NULL) {
            ALOGE("dlsym error for csd_client_start_voice");
            ret = -ENOSYS;
        } else {
#ifndef MULTI_VOICE_SESSION_ENABLED
            ret = my_data->csd_start_voice();
#else
            ret = my_data->csd_start_voice(vsid);
#endif
            if (ret < 0) {
                ALOGE("%s: csd_start_voice error %d\n", __func__, ret);
            }
        }
    }

    return ret;
}

int platform_stop_voice_call(void *platform, uint32_t vsid)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    int ret = 0;

    if (my_data->csd_client) {
        if (my_data->csd_stop_voice == NULL) {
            ALOGE("dlsym error for csd_stop_voice");
        } else {
#ifndef MULTI_VOICE_SESSION_ENABLED
            ret = my_data->csd_stop_voice();
#else
            ret = my_data->csd_stop_voice(vsid);
#endif
            if (ret < 0) {
                ALOGE("%s: csd_stop_voice error %d\n", __func__, ret);
            }
        }
    }

    return ret;
}

int platform_get_sample_rate(void *platform, uint32_t *rate)
{
    return 0;
}

int platform_set_voice_volume(void *platform, int volume)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    int ret = 0;

    if (my_data->csd_client) {
        if (my_data->csd_volume == NULL) {
            ALOGE("%s: dlsym error for csd_volume", __func__);
        } else {
#ifndef MULTI_VOICE_SESSION_ENABLED
            ret = my_data->csd_volume(volume);
#else
            ret = my_data->csd_volume(ALL_SESSION_VSID, volume);
#endif
            if (ret < 0) {
                ALOGE("%s: csd_volume error %d", __func__, ret);
            }
        }
    } else {
        ALOGE("%s: No CSD Client present", __func__);
    }

    return ret;
}

int platform_set_mic_mute(void *platform, bool state)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    int ret = 0;

    if (my_data->adev->mode == AUDIO_MODE_IN_CALL) {
        if (my_data->csd_client) {
            if (my_data->csd_mic_mute == NULL) {
                ALOGE("%s: dlsym error for csd_mic_mute", __func__);
            } else {
#ifndef MULTI_VOICE_SESSION_ENABLED
                ret = my_data->csd_mic_mute(state);
#else
                ret = my_data->csd_mic_mute(ALL_SESSION_VSID, state);
#endif
                if (ret < 0) {
                    ALOGE("%s: csd_mic_mute error %d", __func__, ret);
                }
            }
        } else {
            ALOGE("%s: No CSD Client present", __func__);
        }
    }

    return ret;
}

int platform_set_device_mute(void *platform, bool state, char *dir)
{
    ALOGE("%s: Not implemented", __func__);
    return -ENOSYS;
}

snd_device_t platform_get_output_snd_device(void *platform, audio_devices_t devices)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    struct audio_device *adev = my_data->adev;
    audio_mode_t mode = adev->mode;
    snd_device_t snd_device = SND_DEVICE_NONE;

    ALOGV("%s: enter: output devices(%#x)", __func__, devices);
    if (devices == AUDIO_DEVICE_NONE ||
        devices & AUDIO_DEVICE_BIT_IN) {
        ALOGV("%s: Invalid output devices (%#x)", __func__, devices);
        goto exit;
    }

    if(devices & AUDIO_DEVICE_OUT_PROXY) {
        ALOGD("%s: setting sink capability for Proxy", __func__);
        audio_extn_set_afe_proxy_channel_mixer(adev, audio_extn_get_afe_proxy_channel_count());
    }

    if (mode == AUDIO_MODE_IN_CALL) {
        if (devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE ||
            devices & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            if (adev->voice.tty_mode == TTY_MODE_FULL)
                snd_device = SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES;
            else if (adev->voice.tty_mode == TTY_MODE_VCO)
                snd_device = SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES;
            else if (adev->voice.tty_mode == TTY_MODE_HCO)
                snd_device = SND_DEVICE_OUT_VOICE_TTY_HCO_HANDSET;
            else
                snd_device = SND_DEVICE_OUT_VOICE_HEADPHONES;
        } else if (devices & AUDIO_DEVICE_OUT_ALL_SCO) {
            snd_device = SND_DEVICE_OUT_BT_SCO;
        } else if (devices & AUDIO_DEVICE_OUT_SPEAKER) {
            snd_device = SND_DEVICE_OUT_VOICE_SPEAKER;
        } else if (devices & AUDIO_DEVICE_OUT_FM_TX) {
            snd_device = SND_DEVICE_OUT_TRANSMISSION_FM;
        } else if (devices & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET ||
                    devices & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) {
            snd_device = SND_DEVICE_OUT_USB_HEADSET;
        } else if (devices & AUDIO_DEVICE_OUT_EARPIECE) {
            snd_device = SND_DEVICE_OUT_HANDSET;
        }
        if (snd_device != SND_DEVICE_NONE) {
            goto exit;
        }
    }

    if (popcount(devices) == 2) {
        if (devices == (AUDIO_DEVICE_OUT_WIRED_HEADPHONE |
                        AUDIO_DEVICE_OUT_SPEAKER)) {
            snd_device = SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES;
        } else if (devices == (AUDIO_DEVICE_OUT_WIRED_HEADSET |
                               AUDIO_DEVICE_OUT_SPEAKER)) {
            snd_device = SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES;
        } else if (devices == (AUDIO_DEVICE_OUT_AUX_DIGITAL |
                               AUDIO_DEVICE_OUT_SPEAKER)) {
            snd_device = SND_DEVICE_OUT_SPEAKER_AND_HDMI;
        } else if (devices == (AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET |
                               AUDIO_DEVICE_OUT_SPEAKER)) {
            snd_device = SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET;
        } else {
            ALOGE("%s: Invalid combo device(%#x)", __func__, devices);
            goto exit;
        }
        if (snd_device != SND_DEVICE_NONE) {
            goto exit;
        }
    }

    if (popcount(devices) != 1) {
        ALOGE("%s: Invalid output devices(%#x)", __func__, devices);
        goto exit;
    }

    if (devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE ||
        devices & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
        snd_device = SND_DEVICE_OUT_HEADPHONES;
    } else if (devices & AUDIO_DEVICE_OUT_SPEAKER) {
        if (adev->speaker_lr_swap)
            snd_device = SND_DEVICE_OUT_SPEAKER_REVERSE;
        else
            snd_device = SND_DEVICE_OUT_SPEAKER;
    } else if (devices & AUDIO_DEVICE_OUT_ALL_SCO) {
        snd_device = SND_DEVICE_OUT_BT_SCO;
    } else if (devices & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        snd_device = SND_DEVICE_OUT_HDMI ;
    } else if (devices & AUDIO_DEVICE_OUT_FM_TX) {
        snd_device = SND_DEVICE_OUT_TRANSMISSION_FM;
    } else if (devices & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET ||
               devices & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) {
        snd_device = SND_DEVICE_OUT_USB_HEADSET;
    } else if (devices & AUDIO_DEVICE_OUT_EARPIECE) {
        snd_device = SND_DEVICE_OUT_HANDSET;
    } else if (devices & AUDIO_DEVICE_OUT_PROXY) {
        ALOGD("%s: setting sink capability for Proxy", __func__);
        audio_extn_set_afe_proxy_channel_mixer(adev, audio_extn_get_afe_proxy_channel_count());
                snd_device = SND_DEVICE_OUT_AFE_PROXY;
    } else {
        ALOGE("%s: Unknown device(s) %#x", __func__, devices);
    }
exit:
    ALOGV("%s: exit: snd_device(%s)", __func__, device_table[snd_device]);
    return snd_device;
}

snd_device_t platform_get_input_snd_device(void *platform, audio_devices_t out_device)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    struct audio_device *adev = my_data->adev;
    audio_source_t  source = (adev->active_input == NULL) ?
                                AUDIO_SOURCE_DEFAULT : adev->active_input->source;

    audio_mode_t    mode   = adev->mode;
    audio_devices_t in_device = ((adev->active_input == NULL) ?
                                    AUDIO_DEVICE_NONE : adev->active_input->device)
                                & ~AUDIO_DEVICE_BIT_IN;
    audio_channel_mask_t channel_mask = (adev->active_input == NULL) ?
                                AUDIO_CHANNEL_IN_MONO : adev->active_input->channel_mask;
    snd_device_t snd_device = SND_DEVICE_NONE;

    ALOGV("%s: enter: out_device(%#x) in_device(%#x)",
          __func__, out_device, in_device);
    if (mode == AUDIO_MODE_IN_CALL) {
        if (out_device == AUDIO_DEVICE_NONE) {
            ALOGE("%s: No output device set for voice call", __func__);
            goto exit;
        }
        if (adev->voice.tty_mode != TTY_MODE_OFF) {
            if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE ||
                out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
                switch (adev->voice.tty_mode) {
                case TTY_MODE_FULL:
                    snd_device = SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC;
                    break;
                case TTY_MODE_VCO:
                    snd_device = SND_DEVICE_IN_VOICE_TTY_VCO_HANDSET_MIC;
                    break;
                case TTY_MODE_HCO:
                    snd_device = SND_DEVICE_IN_VOICE_TTY_HCO_HEADSET_MIC;
                    break;
                default:
                    ALOGE("%s: Invalid TTY mode (%#x)", __func__, adev->voice.tty_mode);
                }
                goto exit;
            }
        }
        if (out_device & AUDIO_DEVICE_OUT_EARPIECE ||
            out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE) {
            if (my_data->fluence_type == FLUENCE_NONE ||
                my_data->fluence_in_voice_call == false) {
                snd_device = SND_DEVICE_IN_HANDSET_MIC;
            } else {
                snd_device = SND_DEVICE_IN_VOICE_DMIC;
                adev->acdb_settings |= DMIC_FLAG;
            }
        } else if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            snd_device = SND_DEVICE_IN_VOICE_HEADSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_ALL_SCO) {
            snd_device = SND_DEVICE_IN_BT_SCO_MIC ;
        } else if (out_device & AUDIO_DEVICE_OUT_SPEAKER) {
            if (my_data->fluence_type != FLUENCE_NONE &&
                my_data->fluence_in_voice_call &&
                my_data->fluence_in_spkr_mode) {
                if(my_data->fluence_type == FLUENCE_DUAL_MIC) {
                    adev->acdb_settings |= DMIC_FLAG;
                    snd_device = SND_DEVICE_IN_VOICE_SPEAKER_DMIC;
                } else {
                    adev->acdb_settings |= QMIC_FLAG;
                    snd_device = SND_DEVICE_IN_VOICE_SPEAKER_QMIC;
                }
            } else {
                snd_device = SND_DEVICE_IN_VOICE_SPEAKER_MIC;
            }
        }
    } else if (source == AUDIO_SOURCE_CAMCORDER) {
        if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC ||
            in_device & AUDIO_DEVICE_IN_BACK_MIC) {
            snd_device = SND_DEVICE_IN_CAMCORDER_MIC;
        }
    } else if (source == AUDIO_SOURCE_VOICE_RECOGNITION) {
        if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC) {
            if (channel_mask == AUDIO_CHANNEL_IN_FRONT_BACK)
                snd_device = SND_DEVICE_IN_VOICE_REC_DMIC;
            else if (my_data->fluence_in_voice_rec)
                snd_device = SND_DEVICE_IN_VOICE_REC_DMIC_FLUENCE;

            if (snd_device == SND_DEVICE_NONE)
                snd_device = SND_DEVICE_IN_VOICE_REC_MIC;
            else
                adev->acdb_settings |= DMIC_FLAG;
        }
    } else if (source == AUDIO_SOURCE_VOICE_COMMUNICATION) {
        if (out_device & AUDIO_DEVICE_OUT_SPEAKER)
            in_device = AUDIO_DEVICE_IN_BACK_MIC;
        if (adev->active_input) {
            if (adev->active_input->enable_aec) {
                if (in_device & AUDIO_DEVICE_IN_BACK_MIC) {
                    snd_device = SND_DEVICE_IN_SPEAKER_MIC_AEC;
                } else if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC) {
                    snd_device = SND_DEVICE_IN_HANDSET_MIC_AEC;
                } else if (in_device & AUDIO_DEVICE_IN_WIRED_HEADSET) {
                    snd_device = SND_DEVICE_IN_HEADSET_MIC_AEC;
                }
                set_echo_reference(adev, true);
            } else
                set_echo_reference(adev, false);
        }
    } else if (source == AUDIO_SOURCE_FM_RX) {
        if (in_device & AUDIO_DEVICE_IN_FM_RX) {
            snd_device = SND_DEVICE_IN_CAPTURE_FM;
        }
    } else if (source == AUDIO_SOURCE_DEFAULT) {
        goto exit;
    }


    if (snd_device != SND_DEVICE_NONE) {
        goto exit;
    }

    if (in_device != AUDIO_DEVICE_NONE &&
            !(in_device & AUDIO_DEVICE_IN_VOICE_CALL) &&
            !(in_device & AUDIO_DEVICE_IN_COMMUNICATION)) {
        if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC) {
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_BACK_MIC) {
            snd_device = SND_DEVICE_IN_SPEAKER_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_WIRED_HEADSET) {
            snd_device = SND_DEVICE_IN_HEADSET_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
            snd_device = SND_DEVICE_IN_BT_SCO_MIC ;
        } else if (in_device & AUDIO_DEVICE_IN_AUX_DIGITAL) {
            snd_device = SND_DEVICE_IN_HDMI_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_ANLG_DOCK_HEADSET ||
                in_device & AUDIO_DEVICE_IN_DGTL_DOCK_HEADSET) {
            snd_device = SND_DEVICE_IN_USB_HEADSET_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_FM_RX) {
            snd_device = SND_DEVICE_IN_CAPTURE_FM;
        } else {
            ALOGE("%s: Unknown input device(s) %#x", __func__, in_device);
            ALOGW("%s: Using default handset-mic", __func__);
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        }
    } else {
        if (out_device & AUDIO_DEVICE_OUT_EARPIECE) {
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            snd_device = SND_DEVICE_IN_HEADSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_SPEAKER) {
            snd_device = SND_DEVICE_IN_SPEAKER_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE) {
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET) {
            snd_device = SND_DEVICE_IN_BT_SCO_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
            snd_device = SND_DEVICE_IN_HDMI_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET ||
                out_device & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) {
            snd_device = SND_DEVICE_IN_USB_HEADSET_MIC;
        } else {
            ALOGE("%s: Unknown output device(s) %#x", __func__, out_device);
            ALOGW("%s: Using default handset-mic", __func__);
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        }
    }
exit:
    ALOGV("%s: exit: in_snd_device(%s)", __func__, device_table[snd_device]);
    return snd_device;
}

int platform_set_hdmi_channels(void *platform,  int channel_count)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    struct audio_device *adev = my_data->adev;
    struct mixer_ctl *ctl;
    const char *channel_cnt_str = NULL;
    const char *mixer_ctl_name = "HDMI_RX Channels";
    switch (channel_count) {
    case 8:
        channel_cnt_str = "Eight"; break;
    case 7:
        channel_cnt_str = "Seven"; break;
    case 6:
        channel_cnt_str = "Six"; break;
    case 5:
        channel_cnt_str = "Five"; break;
    case 4:
        channel_cnt_str = "Four"; break;
    case 3:
        channel_cnt_str = "Three"; break;
    default:
        channel_cnt_str = "Two"; break;
    }
    ctl = mixer_get_ctl_by_name(adev->mixer, mixer_ctl_name);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s",
              __func__, mixer_ctl_name);
        return -EINVAL;
    }
    ALOGV("HDMI channel count: %s", channel_cnt_str);
    mixer_ctl_set_enum_by_string(ctl, channel_cnt_str);
    return 0;
}

int platform_edid_get_max_channels(void *platform)
{
    FILE *file;
    struct audio_block_header header;
    char block[MAX_SAD_BLOCKS * SAD_BLOCK_SIZE];
    char *sad = block;
    int num_audio_blocks;
    int channel_count;
    int max_channels = 0;
    int i;

    file = fopen(AUDIO_DATA_BLOCK_PATH, "rb");
    if (file == NULL) {
        ALOGE("Unable to open '%s'", AUDIO_DATA_BLOCK_PATH);
        return 0;
    }

    /* Read audio block header */
    fread(&header, 1, sizeof(header), file);

    /* Read SAD blocks, clamping the maximum size for safety */
    if (header.length > (int)sizeof(block))
        header.length = (int)sizeof(block);
    fread(&block, header.length, 1, file);

    fclose(file);

    /* Calculate the number of SAD blocks */
    num_audio_blocks = header.length / SAD_BLOCK_SIZE;

    for (i = 0; i < num_audio_blocks; i++) {
        /* Only consider LPCM blocks */
        if ((sad[0] >> 3) != EDID_FORMAT_LPCM)
            continue;

        channel_count = (sad[0] & 0x7) + 1;
        if (channel_count > max_channels)
            max_channels = channel_count;

        /* Advance to next block */
        sad += 3;
    }

    return max_channels;
}

void platform_get_parameters(void *platform, struct str_parms *query,
                             struct str_parms *reply)
{
    ALOGE("%s: Not implemented", __func__);
}

int platform_set_parameters(void *platform, struct str_parms *parms)
{
    ALOGE("%s: Not implemented", __func__);
    return -ENOSYS;
}

int platform_set_incall_recording_session_id(void *platform,
                                             uint32_t session_id, int rec_mode)
{
    ALOGE("%s: Not implemented", __func__);
    return -ENOSYS;
}

int platform_stop_incall_recording_usecase(void *platform)
{
    ALOGE("%s: Not implemented", __func__);
    return -ENOSYS;
}

/* Delay in Us */
int64_t platform_render_latency(audio_usecase_t usecase)
{
    switch (usecase) {
        case USECASE_AUDIO_PLAYBACK_DEEP_BUFFER:
            return DEEP_BUFFER_PLATFORM_DELAY;
        case USECASE_AUDIO_PLAYBACK_LOW_LATENCY:
            return LOW_LATENCY_PLATFORM_DELAY;
        default:
            return 0;
    }
}

int platform_update_usecase_from_source(int source, int usecase)
{
    ALOGV("%s: input source :%d", __func__, source);
    return usecase;
}

bool platform_listen_device_needs_event(snd_device_t snd_device)
{
    return false;
}

bool platform_listen_usecase_needs_event(audio_usecase_t uc_id)
{
    return false;
}

bool platform_check_24_bit_support() {
    return false;
}

bool platform_check_and_set_codec_backend_cfg(struct audio_device* adev, struct audio_usecase *usecase) {
    return false;
}

int platform_get_usecase_index(const char * usecase __unused)
{
    return -ENOSYS;
}

int platform_set_usecase_pcm_id(audio_usecase_t usecase __unused, int32_t type __unused,
                                int32_t pcm_id __unused)
{
    return -ENOSYS;
}

int platform_set_snd_device_backend(snd_device_t snd_device __unused,
                                    const char * backend __unused)
{
    return -ENOSYS;
}

int platform_set_snd_device_name(snd_device_t snd_device __unused,
                                 const char * name __unused)
{
    return -ENOSYS;
}
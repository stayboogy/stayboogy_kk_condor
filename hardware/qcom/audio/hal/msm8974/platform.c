/*
 * Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright (C) 2013-2014 The Android Open Source Project
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

#define LOG_TAG "msm8974_platform"
#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0

#include <stdlib.h>
#include <dlfcn.h>
#include <cutils/log.h>
#include <cutils/properties.h>
#include <audio_hw.h>
#include <platform_api.h>
#include "platform.h"
#include "audio_extn.h"
#include "sound/compress_params.h"

#define MIXER_XML_PATH "/system/etc/mixer_paths.xml"
#define LIB_ACDB_LOADER "libacdbloader.so"
#define AUDIO_DATA_BLOCK_MIXER_CTL "HDMI EDID"

#define DUALMIC_CONFIG_NONE 0      /* Target does not contain 2 mics */
#define DUALMIC_CONFIG_ENDFIRE 1
#define DUALMIC_CONFIG_BROADSIDE 2

#define MAX_COMPRESS_OFFLOAD_FRAGMENT_SIZE (256 * 1024)
#define MIN_COMPRESS_OFFLOAD_FRAGMENT_SIZE (2 * 1024)
#define COMPRESS_OFFLOAD_FRAGMENT_SIZE_FOR_AV_STREAMING (2 * 1024)
#define COMPRESS_OFFLOAD_FRAGMENT_SIZE (32 * 1024)

/* Used in calculating fragment size for pcm offload */
#define PCM_OFFLOAD_BUFFER_DURATION_FOR_AV 1000 /* 1 sec */
#define PCM_OFFLOAD_BUFFER_DURATION_FOR_AV_STREAMING 80 /* 80 millisecs */

/* MAX PCM fragment size cannot be increased  further due
 * to flinger's cblk size of 1mb,and it has to be a multiple of
 * 24 - lcm of channels supported by DSP
 */
#define MAX_PCM_OFFLOAD_FRAGMENT_SIZE (240 * 1024)
#define MIN_PCM_OFFLOAD_FRAGMENT_SIZE (4 * 1024)

#define ALIGN( num, to ) (((num) + (to-1)) & (~(to-1)))
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

/* Retry for delay in FW loading*/
#define RETRY_NUMBER 10
#define RETRY_US 500000

#define MAX_VOL_INDEX 5
#define MIN_VOL_INDEX 0
#define percent_to_index(val, min, max) \
                ((val) * ((max) - (min)) * 0.01 + (min) + .5)

struct audio_block_header
{
    int reserved;
    int length;
};

typedef void (*acdb_deallocate_t)();
typedef int  (*acdb_init_t)();
typedef void (*acdb_send_audio_cal_t)(int, int);
typedef void (*acdb_send_voice_cal_t)(int, int);

/* Audio calibration related functions */
struct platform_data {
    struct audio_device *adev;
    bool fluence_in_spkr_mode;
    bool fluence_in_voice_call;
    bool fluence_in_voice_rec;
    int  dualmic_config;

    void *acdb_handle;
    acdb_init_t acdb_init;
    acdb_deallocate_t acdb_deallocate;
    acdb_send_audio_cal_t acdb_send_audio_cal;
    acdb_send_voice_cal_t acdb_send_voice_cal;
};

static const int pcm_device_table[AUDIO_USECASE_MAX][2] = {
    [USECASE_AUDIO_PLAYBACK_DEEP_BUFFER] = {0, 0},
    [USECASE_AUDIO_PLAYBACK_LOW_LATENCY] = {15, 15},
    [USECASE_AUDIO_PLAYBACK_MULTI_CH] = {1, 1},
    [USECASE_AUDIO_PLAYBACK_OFFLOAD] = {9, 9},
    [USECASE_AUDIO_RECORD] = {0, 0},
    [USECASE_AUDIO_RECORD_LOW_LATENCY] = {15, 15},
    [USECASE_VOICE_CALL] = {2, 2},
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
    [SND_DEVICE_OUT_VOICE_HANDSET] = "voice-handset",
    [SND_DEVICE_OUT_VOICE_SPEAKER] = "voice-speaker",
    [SND_DEVICE_OUT_VOICE_HEADPHONES] = "voice-headphones",
    [SND_DEVICE_OUT_HDMI] = "hdmi",
    [SND_DEVICE_OUT_SPEAKER_AND_HDMI] = "speaker-and-hdmi",
    [SND_DEVICE_OUT_BT_SCO] = "bt-sco-headset",
    [SND_DEVICE_OUT_VOICE_HANDSET_TMUS] = "voice-handset-tmus",
    [SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES] = "voice-tty-full-headphones",
    [SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES] = "voice-tty-vco-headphones",
    [SND_DEVICE_OUT_VOICE_TTY_HCO_HANDSET] = "voice-tty-hco-handset",
    [SND_DEVICE_OUT_AFE_PROXY] = "afe-proxy",
    [SND_DEVICE_OUT_USB_HEADSET] = "usb-headphones",
    [SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET] = "speaker-and-usb-headphones",
    [SND_DEVICE_OUT_ANC_HEADSET] = "anc-headphones",
    [SND_DEVICE_OUT_ANC_FB_HEADSET] = "anc-fb-headphones",
    [SND_DEVICE_OUT_VOICE_ANC_HEADSET] = "voice-anc-headphones",
    [SND_DEVICE_OUT_VOICE_ANC_FB_HEADSET] = "voice-anc-fb-headphones",
    [SND_DEVICE_OUT_SPEAKER_AND_ANC_HEADSET] = "speaker-and-anc-headphones",
    [SND_DEVICE_OUT_ANC_HANDSET] = "anc-handset",

    /* Capture sound devices */
    [SND_DEVICE_IN_HANDSET_MIC] = "handset-mic",
    [SND_DEVICE_IN_SPEAKER_MIC] = "speaker-mic",
    [SND_DEVICE_IN_HEADSET_MIC] = "headset-mic",
    [SND_DEVICE_IN_HANDSET_MIC_AEC] = "handset-mic",
    [SND_DEVICE_IN_SPEAKER_MIC_AEC] = "speaker-mic-aec",
    [SND_DEVICE_IN_HEADSET_MIC_AEC] = "headset-mic",
    [SND_DEVICE_IN_VOICE_SPEAKER_MIC] = "voice-speaker-mic",
    [SND_DEVICE_IN_VOICE_HEADSET_MIC] = "voice-headset-mic",
    [SND_DEVICE_IN_HDMI_MIC] = "hdmi-mic",
    [SND_DEVICE_IN_BT_SCO_MIC] = "bt-sco-mic",
    [SND_DEVICE_IN_CAMCORDER_MIC] = "camcorder-mic",
    [SND_DEVICE_IN_VOICE_DMIC_EF] = "voice-dmic-ef",
    [SND_DEVICE_IN_VOICE_DMIC_BS] = "voice-dmic-bs",
    [SND_DEVICE_IN_VOICE_DMIC_EF_TMUS] = "voice-dmic-ef-tmus",
    [SND_DEVICE_IN_VOICE_SPEAKER_DMIC_EF] = "voice-speaker-dmic-ef",
    [SND_DEVICE_IN_VOICE_SPEAKER_DMIC_BS] = "voice-speaker-dmic-bs",
    [SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC] = "voice-tty-full-headset-mic",
    [SND_DEVICE_IN_VOICE_TTY_VCO_HANDSET_MIC] = "voice-tty-vco-handset-mic",
    [SND_DEVICE_IN_VOICE_TTY_HCO_HEADSET_MIC] = "voice-tty-hco-headset-mic",
    [SND_DEVICE_IN_VOICE_REC_MIC] = "voice-rec-mic",
    [SND_DEVICE_IN_VOICE_REC_DMIC_EF] = "voice-rec-dmic-ef",
    [SND_DEVICE_IN_VOICE_REC_DMIC_BS] = "voice-rec-dmic-bs",
    [SND_DEVICE_IN_VOICE_REC_DMIC_EF_FLUENCE] = "voice-rec-dmic-ef-fluence",
    [SND_DEVICE_IN_VOICE_REC_DMIC_BS_FLUENCE] = "voice-rec-dmic-bs-fluence",
    [SND_DEVICE_IN_USB_HEADSET_MIC] = "usb-headset-mic",
    [SND_DEVICE_IN_AANC_HANDSET_MIC] = "aanc-handset-mic",
};

/* ACDB IDs (audio DSP path configuration IDs) for each sound device */
static const int acdb_device_table[SND_DEVICE_MAX] = {
    [SND_DEVICE_NONE] = -1,
    [SND_DEVICE_OUT_HANDSET] = 7,
    [SND_DEVICE_OUT_SPEAKER] = 15,
    [SND_DEVICE_OUT_SPEAKER_REVERSE] = 15,
    [SND_DEVICE_OUT_HEADPHONES] = 10,
    [SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES] = 10,
    [SND_DEVICE_OUT_VOICE_HANDSET] = 7,
    [SND_DEVICE_OUT_VOICE_SPEAKER] = 15,
    [SND_DEVICE_OUT_VOICE_HEADPHONES] = 10,
    [SND_DEVICE_OUT_HDMI] = 18,
    [SND_DEVICE_OUT_SPEAKER_AND_HDMI] = 15,
    [SND_DEVICE_OUT_BT_SCO] = 22,
    [SND_DEVICE_OUT_VOICE_HANDSET_TMUS] = 88,
    [SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES] = 17,
    [SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES] = 17,
    [SND_DEVICE_OUT_VOICE_TTY_HCO_HANDSET] = 37,
    [SND_DEVICE_OUT_AFE_PROXY] = 0,
    [SND_DEVICE_OUT_USB_HEADSET] = 45,
    [SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET] = 14,
    [SND_DEVICE_OUT_ANC_HEADSET] = 26,
    [SND_DEVICE_OUT_ANC_FB_HEADSET] = 26,
    [SND_DEVICE_OUT_VOICE_ANC_HEADSET] = 26,
    [SND_DEVICE_OUT_VOICE_ANC_FB_HEADSET] = 26,
    [SND_DEVICE_OUT_SPEAKER_AND_ANC_HEADSET] = 26,
    [SND_DEVICE_OUT_ANC_HANDSET] = 103,

    [SND_DEVICE_IN_HANDSET_MIC] = 4,
    [SND_DEVICE_IN_SPEAKER_MIC] = 4, /* ToDo: Check if this needs to changed to 11 */
    [SND_DEVICE_IN_HEADSET_MIC] = 8,
    [SND_DEVICE_IN_HANDSET_MIC_AEC] = 40,
    [SND_DEVICE_IN_SPEAKER_MIC_AEC] = 42,
    [SND_DEVICE_IN_HEADSET_MIC_AEC] = 47,
    [SND_DEVICE_IN_VOICE_SPEAKER_MIC] = 11,
    [SND_DEVICE_IN_VOICE_HEADSET_MIC] = 8,
    [SND_DEVICE_IN_HDMI_MIC] = 4,
    [SND_DEVICE_IN_BT_SCO_MIC] = 21,
    [SND_DEVICE_IN_CAMCORDER_MIC] = 61,
    [SND_DEVICE_IN_VOICE_DMIC_EF] = 41,
    [SND_DEVICE_IN_VOICE_DMIC_BS] = 5,
    [SND_DEVICE_IN_VOICE_DMIC_EF_TMUS] = 89,
    [SND_DEVICE_IN_VOICE_SPEAKER_DMIC_EF] = 43,
    [SND_DEVICE_IN_VOICE_SPEAKER_DMIC_BS] = 12,
    [SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC] = 16,
    [SND_DEVICE_IN_VOICE_TTY_VCO_HANDSET_MIC] = 36,
    [SND_DEVICE_IN_VOICE_TTY_HCO_HEADSET_MIC] = 16,
    [SND_DEVICE_IN_VOICE_REC_MIC] = 62,
    [SND_DEVICE_IN_USB_HEADSET_MIC] = 44,
    [SND_DEVICE_IN_AANC_HANDSET_MIC] = 104,
    /* TODO: Update with proper acdb ids */
    [SND_DEVICE_IN_VOICE_REC_DMIC_EF] = 62,
    [SND_DEVICE_IN_VOICE_REC_DMIC_BS] = 62,
    [SND_DEVICE_IN_VOICE_REC_DMIC_EF_FLUENCE] = 6,
    [SND_DEVICE_IN_VOICE_REC_DMIC_BS_FLUENCE] = 5,
};

#define DEEP_BUFFER_PLATFORM_DELAY (29*1000LL)
#define LOW_LATENCY_PLATFORM_DELAY (13*1000LL)

static pthread_once_t check_op_once_ctl = PTHREAD_ONCE_INIT;
static bool is_tmus = false;

static void check_operator()
{
#ifndef DISABLE_TMUS_AUDIO
    char value[PROPERTY_VALUE_MAX];
    int mccmnc;
    property_get("gsm.sim.operator.numeric",value,"0");
    mccmnc = atoi(value);
    ALOGD("%s: tmus mccmnc %d", __func__, mccmnc);
    switch(mccmnc) {
    /* TMUS MCC(310), MNC(490, 260, 026) */
    case 310490:
    case 310260:
    case 310026:
    /* Add new TMUS MNC(800, 660, 580, 310, 270, 250, 240, 230, 220, 210, 200, 160) */
    case 310800:
    case 310660:
    case 310580:
    case 310310:
    case 310270:
    case 310250:
    case 310240:
    case 310230:
    case 310220:
    case 310210:
    case 310200:
    case 310160:
        is_tmus = true;
        break;
    }
#endif
}

bool is_operator_tmus()
{
    pthread_once(&check_op_once_ctl, check_operator);
    return is_tmus;
}

static int set_volume_values(int type, int volume, int* values)
{
    values[0] = volume;
    values[1] = ALL_SESSION_VSID;

    switch(type) {
    case VOLUME_SET:
        values[2] = DEFAULT_VOLUME_RAMP_DURATION_MS;
        break;
    case MUTE_SET:
        values[2] = DEFAULT_MUTE_RAMP_DURATION;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static int set_echo_reference(struct mixer *mixer, const char* ec_ref)
{
    struct mixer_ctl *ctl;
    const char *mixer_ctl_name = "EC_REF_RX";

    ctl = mixer_get_ctl_by_name(mixer, mixer_ctl_name);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s",
              __func__, mixer_ctl_name);
        return -EINVAL;
    }
    ALOGV("Setting EC Reference: %s", ec_ref);
    mixer_ctl_set_enum_by_string(ctl, ec_ref);
    return 0;
}

void *platform_init(struct audio_device *adev)
{
    char value[PROPERTY_VALUE_MAX];
    struct platform_data *my_data;
    int retry_num = 0, ret;

    adev->mixer = mixer_open(MIXER_CARD);

    while (!adev->mixer && retry_num < RETRY_NUMBER) {
        usleep(RETRY_US);
        adev->mixer = mixer_open(MIXER_CARD);
        retry_num++;
    }

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

    my_data->adev = adev;
    my_data->dualmic_config = DUALMIC_CONFIG_NONE;
    my_data->fluence_in_spkr_mode = false;
    my_data->fluence_in_voice_call = false;
    my_data->fluence_in_voice_rec = false;

    property_get("persist.audio.dualmic.config",value,"");
    if (!strcmp("broadside", value)) {
        my_data->dualmic_config = DUALMIC_CONFIG_BROADSIDE;
        adev->acdb_settings |= DMIC_FLAG;
    } else if (!strcmp("endfire", value)) {
        my_data->dualmic_config = DUALMIC_CONFIG_ENDFIRE;
        adev->acdb_settings |= DMIC_FLAG;
    }

    if (my_data->dualmic_config != DUALMIC_CONFIG_NONE) {
        property_get("persist.audio.fluence.voicecall",value,"");
        if (!strcmp("true", value)) {
            my_data->fluence_in_voice_call = true;
        }

        property_get("persist.audio.fluence.voicerec",value,"");
        if (!strcmp("true", value)) {
            my_data->fluence_in_voice_rec = true;
        }

        property_get("persist.audio.fluence.speaker",value,"");
        if (!strcmp("true", value)) {
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

    /* init usb */
    audio_extn_usb_init(adev);

    return my_data;
}

void platform_deinit(void *platform)
{
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

void platform_add_backend_name(char *mixer_path, snd_device_t snd_device)
{
    if (snd_device == SND_DEVICE_IN_BT_SCO_MIC)
        strcat(mixer_path, " bt-sco");
    else if(snd_device == SND_DEVICE_OUT_BT_SCO)
        strcat(mixer_path, " bt-sco");
    else if (snd_device == SND_DEVICE_OUT_HDMI)
        strcat(mixer_path, " hdmi");
    else if (snd_device == SND_DEVICE_OUT_SPEAKER_AND_HDMI)
        strcat(mixer_path, " speaker-and-hdmi");
    else if (snd_device == SND_DEVICE_OUT_AFE_PROXY)
        strlcat(mixer_path, " afe-proxy", MIXER_PATH_MAX_LENGTH);
#ifdef USB_HEADSET_ENABLED
    else if (snd_device == SND_DEVICE_OUT_USB_HEADSET)
        strlcat(mixer_path, " usb-headphones", MIXER_PATH_MAX_LENGTH);
    else if (snd_device == SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET)
        strlcat(mixer_path, " speaker-and-usb-headphones",
                MIXER_PATH_MAX_LENGTH);
    else if (snd_device == SND_DEVICE_IN_USB_HEADSET_MIC)
        strlcat(mixer_path, " usb-headset-mic", MIXER_PATH_MAX_LENGTH);
#endif
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
    return 0;
}

int platform_switch_voice_call_device_post(void *platform,
                                           snd_device_t out_snd_device,
                                           snd_device_t in_snd_device)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    int acdb_rx_id, acdb_tx_id;

    if (my_data->acdb_send_voice_cal == NULL) {
        ALOGE("%s: dlsym error for acdb_send_voice_call", __func__);
    } else {
        acdb_rx_id = acdb_device_table[out_snd_device];
        acdb_tx_id = acdb_device_table[in_snd_device];

        if (acdb_rx_id > 0 && acdb_tx_id > 0)
            my_data->acdb_send_voice_cal(acdb_rx_id, acdb_tx_id);
        else
            ALOGE("%s: Incorrect ACDB IDs (rx: %d tx: %d)", __func__,
                  acdb_rx_id, acdb_tx_id);
    }

    return 0;
}

int platform_start_voice_call(void *platform)
{
    return 0;
}

int platform_stop_voice_call(void *platform)
{
    return 0;
}

int platform_set_voice_volume(void *platform, int volume)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    struct audio_device *adev = my_data->adev;
    struct mixer_ctl *ctl;
    const char *mixer_ctl_name = "Voice Rx Gain";
    int values[VOLUME_CTL_PARAM_NUM];
    int ret = 0;

    // Voice volume levels are mapped to adsp volume levels as follows.
    // 100 -> 5, 80 -> 4, 60 -> 3, 40 -> 2, 20 -> 1  0 -> 0
    // But this values don't changed in kernel. So, below change is need.
    volume = (int)percent_to_index(volume, MIN_VOL_INDEX, MAX_VOL_INDEX);

    ctl = mixer_get_ctl_by_name(adev->mixer, mixer_ctl_name);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s",
              __func__, mixer_ctl_name);
        return -EINVAL;
    }
    ret = set_volume_values(VOLUME_SET, volume, values);
    if (ret < 0) {
        ALOGV("%s: failed setting volume by incorrect type", __func__);
        return -EINVAL;
    }
    ret = mixer_ctl_set_array(ctl, values, sizeof(values)/sizeof(int));
    if (ret < 0) {
        ALOGV("%s: failed set mixer ctl by %d", __func__, ret);
        return -EINVAL;
    }

    return 0;
}

int platform_set_mic_mute(void *platform, bool state)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    struct audio_device *adev = my_data->adev;
    struct mixer_ctl *ctl;
    const char *mixer_ctl_name = "Voice Tx Mute";
    int values[VOLUME_CTL_PARAM_NUM];
    int ret = 0;

    if (adev->mode == AUDIO_MODE_IN_CALL) {
        ctl = mixer_get_ctl_by_name(adev->mixer, mixer_ctl_name);
        if (!ctl) {
            ALOGE("%s: Could not get ctl for mixer cmd - %s",
                  __func__, mixer_ctl_name);
            return -EINVAL;
        }
        ALOGV("Setting mic mute: %d", state);
        ret = set_volume_values(MUTE_SET, state, values);
        if (ret < 0) {
            ALOGV("%s: failed setting mute by incorrect type", __func__);
            return -EINVAL;
        }
        ret = mixer_ctl_set_array(ctl, values, sizeof(values)/sizeof(int));
        if (ret < 0) {
            ALOGV("%s: failed set mixer ctl by %d", __func__, ret);
            return -EINVAL;
        }
    }

    return 0;
}

snd_device_t platform_get_output_snd_device(void *platform, audio_devices_t devices)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    struct audio_device *adev = my_data->adev;
    audio_mode_t mode = adev->mode;
    snd_device_t snd_device = SND_DEVICE_NONE;

    audio_channel_mask_t channel_mask = (adev->active_input == NULL) ?
                                AUDIO_CHANNEL_IN_MONO : adev->active_input->channel_mask;
    int channel_count = popcount(channel_mask);

    ALOGV("%s: enter: output devices(%#x)", __func__, devices);
    if (devices == AUDIO_DEVICE_NONE ||
        devices & AUDIO_DEVICE_BIT_IN) {
        ALOGV("%s: Invalid output devices (%#x)", __func__, devices);
        goto exit;
    }

    if (mode == AUDIO_MODE_IN_CALL) {
        if (devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE ||
            devices & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            if (adev->tty_mode == TTY_MODE_FULL)
                snd_device = SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES;
            else if (adev->tty_mode == TTY_MODE_VCO)
                snd_device = SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES;
            else if (adev->tty_mode == TTY_MODE_HCO)
                snd_device = SND_DEVICE_OUT_VOICE_TTY_HCO_HANDSET;
            else if (audio_extn_get_anc_enabled()) {
                if (audio_extn_should_use_fb_anc())
                    snd_device = SND_DEVICE_OUT_VOICE_ANC_FB_HEADSET;
                else
                    snd_device = SND_DEVICE_OUT_VOICE_ANC_HEADSET;
           }
            else
                snd_device = SND_DEVICE_OUT_VOICE_HEADPHONES;
        } else if (devices & AUDIO_DEVICE_OUT_ALL_SCO) {
            snd_device = SND_DEVICE_OUT_BT_SCO;
        } else if (devices & AUDIO_DEVICE_OUT_SPEAKER) {
            snd_device = SND_DEVICE_OUT_VOICE_SPEAKER;
#ifdef USB_HEADSET_ENABLED
        } else if (devices & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET ||
                   devices & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) {
            snd_device = SND_DEVICE_OUT_USB_HEADSET;
#endif
        } else if (devices & AUDIO_DEVICE_OUT_EARPIECE) {
            if (is_operator_tmus())
                snd_device = SND_DEVICE_OUT_VOICE_HANDSET_TMUS;
            else if (audio_extn_should_use_handset_anc(channel_count))
                snd_device = SND_DEVICE_OUT_ANC_HANDSET;
            else
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
            if (audio_extn_get_anc_enabled())
                snd_device = SND_DEVICE_OUT_SPEAKER_AND_ANC_HEADSET;
            else
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
        if (devices & AUDIO_DEVICE_OUT_WIRED_HEADSET
            && audio_extn_get_anc_enabled()) {
            if (audio_extn_should_use_fb_anc())
                snd_device = SND_DEVICE_OUT_ANC_FB_HEADSET;
            else
                snd_device = SND_DEVICE_OUT_ANC_HEADSET;
        }
        else
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
#ifdef USB_HEADSET_ENABLED
    } else if (devices & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET ||
               devices & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) {
        snd_device = SND_DEVICE_OUT_USB_HEADSET;
#endif
    } else if (devices & AUDIO_DEVICE_OUT_EARPIECE) {
        snd_device = SND_DEVICE_OUT_HANDSET;
    } else if (devices & AUDIO_DEVICE_OUT_PROXY) {
        ALOGD("%s: setting sink capability for Proxy", __func__);
        audio_extn_set_afe_proxy_channel_mixer(adev);
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
    int channel_count = popcount(channel_mask);

    ALOGV("%s: enter: out_device(%#x) in_device(%#x)",
          __func__, out_device, in_device);
    if (mode == AUDIO_MODE_IN_CALL) {
        if (out_device == AUDIO_DEVICE_NONE) {
            ALOGE("%s: No output device set for voice call", __func__);
            goto exit;
        }
        if (adev->tty_mode != TTY_MODE_OFF) {
            if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE ||
                out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
                switch (adev->tty_mode) {
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
                    ALOGE("%s: Invalid TTY mode (%#x)", __func__, adev->tty_mode);
                }
                goto exit;
            }
        }
        if (out_device & AUDIO_DEVICE_OUT_EARPIECE ||
            out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE) {
            if (out_device & AUDIO_DEVICE_OUT_EARPIECE &&
                audio_extn_should_use_handset_anc(channel_count)) {
                snd_device = SND_DEVICE_IN_AANC_HANDSET_MIC;
            } else if (my_data->fluence_in_voice_call == false) {
                snd_device = SND_DEVICE_IN_HANDSET_MIC;
            } else {
                if (my_data->dualmic_config == DUALMIC_CONFIG_ENDFIRE) {
                    if (is_operator_tmus())
                        snd_device = SND_DEVICE_IN_VOICE_DMIC_EF_TMUS;
                    else
                        snd_device = SND_DEVICE_IN_VOICE_DMIC_EF;
                } else if(my_data->dualmic_config == DUALMIC_CONFIG_BROADSIDE)
                    snd_device = SND_DEVICE_IN_VOICE_DMIC_BS;
                else
                    snd_device = SND_DEVICE_IN_HANDSET_MIC;
            }
        } else if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            snd_device = SND_DEVICE_IN_VOICE_HEADSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_ALL_SCO) {
            snd_device = SND_DEVICE_IN_BT_SCO_MIC ;
        } else if (out_device & AUDIO_DEVICE_OUT_SPEAKER) {
            if (my_data->fluence_in_voice_call && my_data->fluence_in_spkr_mode &&
                    my_data->dualmic_config == DUALMIC_CONFIG_ENDFIRE) {
                snd_device = SND_DEVICE_IN_VOICE_SPEAKER_DMIC_EF;
            } else if (my_data->fluence_in_voice_call && my_data->fluence_in_spkr_mode &&
                    my_data->dualmic_config == DUALMIC_CONFIG_BROADSIDE) {
                snd_device = SND_DEVICE_IN_VOICE_SPEAKER_DMIC_BS;
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
            if (my_data->dualmic_config == DUALMIC_CONFIG_ENDFIRE) {
                if (channel_mask == AUDIO_CHANNEL_IN_FRONT_BACK)
                    snd_device = SND_DEVICE_IN_VOICE_REC_DMIC_EF;
                else if (my_data->fluence_in_voice_rec)
                    snd_device = SND_DEVICE_IN_VOICE_REC_DMIC_EF_FLUENCE;
            } else if (my_data->dualmic_config == DUALMIC_CONFIG_BROADSIDE) {
                if (channel_mask == AUDIO_CHANNEL_IN_FRONT_BACK)
                    snd_device = SND_DEVICE_IN_VOICE_REC_DMIC_BS;
                else if (my_data->fluence_in_voice_rec)
                    snd_device = SND_DEVICE_IN_VOICE_REC_DMIC_BS_FLUENCE;
            }

            if (snd_device == SND_DEVICE_NONE) {
                snd_device = SND_DEVICE_IN_VOICE_REC_MIC;
            }
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
                set_echo_reference(adev->mixer, "SLIM_RX");
            } else
                set_echo_reference(adev->mixer, "NONE");
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
#ifdef USB_HEADSET_ENABLED
        } else if (in_device & AUDIO_DEVICE_IN_ANLG_DOCK_HEADSET ||
                   in_device & AUDIO_DEVICE_IN_DGTL_DOCK_HEADSET) {
            snd_device = SND_DEVICE_IN_USB_HEADSET_MIC;
#endif
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
#ifdef USB_HEADSET_ENABLED
        } else if (out_device & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET ||
                   out_device & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) {
            snd_device = SND_DEVICE_IN_USB_HEADSET_MIC;
#endif
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
    struct platform_data *my_data = (struct platform_data *)platform;
    struct audio_device *adev = my_data->adev;
    char block[MAX_SAD_BLOCKS * SAD_BLOCK_SIZE];
    char *sad = block;
    int num_audio_blocks;
    int channel_count;
    int max_channels = 0;
    int i, ret, count;

    struct mixer_ctl *ctl;

    ctl = mixer_get_ctl_by_name(adev->mixer, AUDIO_DATA_BLOCK_MIXER_CTL);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s",
              __func__, AUDIO_DATA_BLOCK_MIXER_CTL);
        return 0;
    }

    mixer_ctl_update(ctl);

    count = mixer_ctl_get_num_values(ctl);

    /* Read SAD blocks, clamping the maximum size for safety */
    if (count > (int)sizeof(block))
        count = (int)sizeof(block);

    ret = mixer_ctl_get_array(ctl, block, count);
    if (ret != 0) {
        ALOGE("%s: mixer_ctl_get_array() failed to get EDID info", __func__);
        return 0;
    }

    /* Calculate the number of SAD blocks */
    num_audio_blocks = count / SAD_BLOCK_SIZE;

    for (i = 0; i < num_audio_blocks; i++) {
        /* Only consider LPCM blocks */
        if ((sad[0] >> 3) != EDID_FORMAT_LPCM) {
            sad += 3;
            continue;
        }

        channel_count = (sad[0] & 0x7) + 1;
        if (channel_count > max_channels)
            max_channels = channel_count;

        /* Advance to next block */
        sad += 3;
    }

    return max_channels;
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

/* Read  offload buffer size from a property.
 * If value is not power of 2  round it to
 * power of 2.
 */
uint32_t platform_get_compress_offload_buffer_size(audio_offload_info_t* info)
{
    char value[PROPERTY_VALUE_MAX] = {0};
    uint32_t fragment_size = COMPRESS_OFFLOAD_FRAGMENT_SIZE;
    if((property_get("audio.offload.buffer.size.kb", value, "")) &&
            atoi(value)) {
        fragment_size =  atoi(value) * 1024;
    }

#ifdef FLAC_OFFLOAD_ENABLED
    // For FLAC use max size since it is loss less, and has sampling rates
    // upto 192kHZ
    if (info != NULL && !info->has_video &&
        info->format == AUDIO_FORMAT_FLAC) {
       fragment_size = MAX_COMPRESS_OFFLOAD_FRAGMENT_SIZE;
       ALOGV("FLAC fragment size %d", fragment_size);
    }
#endif

    if (info != NULL && info->has_video && info->is_streaming) {
        fragment_size = COMPRESS_OFFLOAD_FRAGMENT_SIZE_FOR_AV_STREAMING;
        ALOGV("%s: offload fragment size reduced for AV streaming to %d",
               __func__, fragment_size);
    }

    fragment_size = ALIGN( fragment_size, 1024);

    if(fragment_size < MIN_COMPRESS_OFFLOAD_FRAGMENT_SIZE)
        fragment_size = MIN_COMPRESS_OFFLOAD_FRAGMENT_SIZE;
    else if(fragment_size > MAX_COMPRESS_OFFLOAD_FRAGMENT_SIZE)
        fragment_size = MAX_COMPRESS_OFFLOAD_FRAGMENT_SIZE;
    ALOGV("%s: fragment_size %d", __func__, fragment_size);
    return fragment_size;
}

uint32_t platform_get_pcm_offload_buffer_size(audio_offload_info_t* info)
{
    uint32_t fragment_size = MIN_PCM_OFFLOAD_FRAGMENT_SIZE;
    uint32_t bits_per_sample = 16;

    if (info->format == AUDIO_FORMAT_PCM_24_BIT_OFFLOAD) {
        bits_per_sample = 32;
    }

    if (!info->has_video) {
        fragment_size = MAX_PCM_OFFLOAD_FRAGMENT_SIZE;

    } else if (info->has_video && info->is_streaming) {
        fragment_size = (PCM_OFFLOAD_BUFFER_DURATION_FOR_AV_STREAMING
                                     * info->sample_rate
                                     * (bits_per_sample >> 3)
                                     * popcount(info->channel_mask))/1000;

    } else if (info->has_video) {
        fragment_size = (PCM_OFFLOAD_BUFFER_DURATION_FOR_AV
                                     * info->sample_rate
                                     * (bits_per_sample >> 3)
                                     * popcount(info->channel_mask))/1000;
    }

    char value[PROPERTY_VALUE_MAX] = {0};
    if((property_get("audio.offload.pcm.buffer.size", value, "")) &&
            atoi(value)) {
        fragment_size =  atoi(value) * 1024;
        ALOGV("Using buffer size from sys prop %d", fragment_size);
    }

    fragment_size = ALIGN( fragment_size, 1024);

    if(fragment_size < MIN_PCM_OFFLOAD_FRAGMENT_SIZE)
        fragment_size = MIN_PCM_OFFLOAD_FRAGMENT_SIZE;
    else if(fragment_size > MAX_PCM_OFFLOAD_FRAGMENT_SIZE)
        fragment_size = MAX_PCM_OFFLOAD_FRAGMENT_SIZE;

    ALOGV("%s: fragment_size %d", __func__, fragment_size);
    return fragment_size;
}

bool platform_check_24_bit_support() {

    char value[PROPERTY_VALUE_MAX] = {0};
    property_get("audio.offload.24bit.enable", value, "0");
    if (atoi(value)) {
        ALOGW("Property audio.offload.24bit.enable is set");
        return true;
    }
    return false;
}

static unsigned int get_best_backend_sample_rate(unsigned int sample_rate) {

    // codec backend can take 48K, 96K, and 192K
    if (sample_rate <= 48000)
        return 48000;
    if (sample_rate <= 96000)
        return 96000;
    if (sample_rate <= 192000)
        return 192000;
    return CODEC_BACKEND_DEFAULT_SAMPLE_RATE;
}

static unsigned int get_best_backend_bit_width(unsigned int bit_width) {

    if (bit_width == 24)
        return 24;
    return CODEC_BACKEND_DEFAULT_BIT_WIDTH;
}

int platform_set_codec_backend_cfg(struct audio_device* adev,
                         unsigned int bit_width, unsigned int sample_rate)
{
    ALOGV("platform_set_codec_backend_cfg bw %d, sr %d", bit_width, sample_rate);

    int ret = 0;
    if (bit_width != adev->cur_codec_backend_bit_width) {
        const char * mixer_ctl_name = "SLIM_0_RX Format";
        struct  mixer_ctl *ctl;
        ctl = mixer_get_ctl_by_name(adev->mixer, mixer_ctl_name);
        if (!ctl) {
            ALOGE("%s: Could not get ctl for mixer command - %s",
                    __func__, mixer_ctl_name);
            return -EINVAL;
        }

        if (bit_width == 24) {
                mixer_ctl_set_enum_by_string(ctl, "S24_LE");
        } else {
            mixer_ctl_set_enum_by_string(ctl, "S16_LE");
            sample_rate = CODEC_BACKEND_DEFAULT_SAMPLE_RATE;
        }
        adev->cur_codec_backend_bit_width = bit_width;
        ALOGE("Backend bit width is set to %d ", bit_width);
    }

    if ((adev->cur_codec_backend_bit_width == CODEC_BACKEND_DEFAULT_BIT_WIDTH &&
             adev->cur_codec_backend_samplerate != CODEC_BACKEND_DEFAULT_SAMPLE_RATE) ||
        (adev->cur_codec_backend_samplerate != sample_rate)) {

            char *rate_str = NULL;
            const char * mixer_ctl_name = "SLIM_0_RX SampleRate";
            struct  mixer_ctl *ctl;

            switch (sample_rate) {
            case 8000:
                rate_str = "KHZ_8";
                break;
            case 11025:
                rate_str = "KHZ_11_025";
                break;
            case 16000:
                rate_str = "KHZ_16";
                break;
            case 22050:
                rate_str = "KHZ_22_05";
                break;
            case 32000:
                rate_str = "KHZ_32";
                break;
            case 44100:
                rate_str = "KHZ_44_1";
                break;
            case 48000:
                rate_str = "KHZ_48";
                break;
            case 64000:
                rate_str = "KHZ_64";
                break;
            case 88200:
                rate_str = "KHZ_88_2";
                break;
            case 96000:
                rate_str = "KHZ_96";
                break;
            case 176400:
                rate_str = "KHZ_176_4";
                break;
            case 192000:
                rate_str = "KHZ_192";
                break;
            default:
                rate_str = "KHZ_48";
                break;
            }

            ctl = mixer_get_ctl_by_name(adev->mixer, mixer_ctl_name);
            if(!ctl) {
                ALOGE("%s: Could not get ctl for mixer command - %s",
                    __func__, mixer_ctl_name);
                return -EINVAL;
            }

            ALOGV("Set sample rate as rate_str = %s", rate_str);
            mixer_ctl_set_enum_by_string(ctl, rate_str);
            adev->cur_codec_backend_samplerate = sample_rate;
    }

    return ret;
}

bool platform_check_codec_backend_cfg(struct audio_device* adev,
                                   struct audio_usecase* usecase,
                                   unsigned int* new_bit_width,
                                   unsigned int* new_sample_rate)
{
    bool backend_change = false;
    struct listnode *node;
    struct stream_out *out = NULL;
    unsigned int cur_sr, cur_bw, best_bw = 0, best_sr = 0;

    // For voice calls use default configuration
    // force routing is not required here, caller will do it anyway
    if (adev->mode == AUDIO_MODE_IN_CALL ||
        adev->mode == AUDIO_MODE_IN_COMMUNICATION) {
        ALOGW("%s:Use default bw and sr for voice/voip calls ",__func__);
        *new_bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
        *new_sample_rate =  CODEC_BACKEND_DEFAULT_SAMPLE_RATE;
        backend_change = true;
    }


    if (!backend_change) {
        // go through all the offload usecases, and
        // find the max bit width and samplerate
        list_for_each(node, &adev->usecase_list) {
            struct audio_usecase *curr_usecase;
            curr_usecase = node_to_item(node, struct audio_usecase, list);
            struct stream_out *out =
                       (struct stream_out*) curr_usecase->stream.out;
            if (out != NULL) {
                cur_sr = get_best_backend_sample_rate(out->sample_rate);
                cur_bw = get_best_backend_bit_width(out->bit_width);

                ALOGV("Playback running bw %d sr %d standby %d",
                          cur_bw, cur_sr, out->standby);

                if (cur_bw > best_bw) {
                    best_bw = cur_bw;
                }

                if (cur_sr > best_sr) {
                    best_sr = cur_sr;
                }
            }
        }
    }
    *new_bit_width = best_bw;
    *new_sample_rate = best_sr;

    // Force routing if the expected bitwdith or samplerate
    // is not same as current backend comfiguration
    if ((*new_bit_width != adev->cur_codec_backend_bit_width) ||
        (*new_sample_rate != adev->cur_codec_backend_samplerate)) {
        backend_change = true;
        ALOGW("Codec backend needs to be updated");
    }

    return backend_change;
}

bool platform_check_and_set_codec_backend_cfg(struct audio_device* adev, struct audio_usecase *usecase)
{
    // check if 24bit configuration is enabled first
    if (!platform_check_24_bit_support()) {
        ALOGW("24bit not enable, no need to check for backend change");
        return false;
    }

    ALOGV("platform_check_and_set_codec_backend_cfg usecase = %d",usecase->id );

    unsigned int new_bit_width = 0, old_bit_width;
    unsigned int new_sample_rate = 0, old_sample_rate;

    old_bit_width = adev->cur_codec_backend_bit_width;
    old_sample_rate = adev->cur_codec_backend_samplerate;

    ALOGW("Codec backend bitwidth %d, samplerate %d", old_bit_width, old_sample_rate);
    if (platform_check_codec_backend_cfg(adev, usecase,
                                      &new_bit_width, &new_sample_rate)) {
        platform_set_codec_backend_cfg(adev, new_bit_width, new_sample_rate);
    }

    if (old_bit_width != adev->cur_codec_backend_bit_width ||
        old_sample_rate != adev->cur_codec_backend_samplerate) {
        ALOGW("New codec backend bit width %d, sample rate %d",
                    adev->cur_codec_backend_bit_width, adev->cur_codec_backend_samplerate);
        return true;
    }

    return false;
}

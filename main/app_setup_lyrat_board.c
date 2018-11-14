/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2018 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "tcpip_adapter.h"

#if (defined CONFIG_ESP_LYRAT_V4_3_BOARD || defined CONFIG_ESP_LYRAT_V4_2_BOARD)

#ifdef ESP_AUTO_PLAY
#include "esp_decoder.h"
#if CONFIG_CLASSIC_BT_ENABLED
#include "mp3_decoder.h"
#include "wav_decoder.h"
#include "pcm_decoder.h"
#endif //CONFIG_CLASSIC_BT_ENABLED
#else
#include "fdkaac_decoder.h"
#include "mp3_decoder.h"
#include "ogg_decoder.h"
#include "flac_decoder.h"
#include "wav_decoder.h"
#include "opus_decoder.h"
#include "aac_decoder.h"
#include "amr_decoder.h"
#include "apus_decoder.h"
#include "pcm_decoder.h"
#endif
#include "pcm_encoder.h"
#include "amrnb_encoder.h"
#include "amrwb_encoder.h"
#include "apus_encoder.h"
#include "opus_encoder.h"

#include "MediaControl.h"
#include "DeviceController.h"
#include "TerminalControlService.h"
#include "TouchControlService.h"
#include "led.h"
#include "userconfig.h"
#include "EspAudio.h"
#include "InterruptionSal.h"
#include "HeadphoneConfig.h"
#include "MediaHal.h"

#include "ES8388_interface.h"
#include "ES8374_interface.h"
#include "led_light.h"
#include "HeadphoneConfig.h"

#if IDF_3_0 && CONFIG_BT_ENABLED
#include "esp_bt.h"
#endif

//select classic BT in menuconfig if want to use BT audio, otherwise Wi-Fi audio is on by default
#ifdef CONFIG_CLASSIC_BT_ENABLED
#include "BluetoothControlService.h"
#else
#include "EspAppControlService.h"
#include "OtaService.h"
#include "tcpip_adapter.h"
#if CONFIG_ENABLE_DLNA_SERVICE
#include "DlnaService.h"
#endif

#include "dd_mqtt.h"
#include "dd_http.h"
#include "dd_cloud_service.h"

#if (defined CONFIG_ENABLE_TURINGAIWIFI_SERVICE || defined CONFIG_ENABLE_TURINGWECHAT_SERVICE)
#include "TuringRobotService.h"
#endif
#endif /* CONFIG_CLASSIC_BT_ENABLED */

#include "AlarmService.h"

#define APP_TAG "AUDIO_MAIN"

const PlayerConfig playerConfig = {
    .bufCfg = {
#if (CONFIG_SPIRAM_BOOT_INIT || CONFIG_MEMMAP_SPIRAM_ENABLE_MALLOC)
        .inputBufferLength = 1 * 1024 * 1024, //input buffer size in bytes, this buffer is used to store the data downloaded from http or fetched from TF-Card or MIC
#else
        .inputBufferLength = 20 * 1024, //input buffer size in bytes, this buffer is used to store the data downloaded from http or fetched from TF-Card or MIC
#endif
        .outputBufferLength = 20 * 1024,//output buffer size in bytes, is used to store the data before play or write into TF-Card
#ifndef CONFIG_CLASSIC_BT_ENABLED
        .processBufferLength = 10
#endif
    },
    //comment any of below streams to disable corresponding streams in order to save RAM.
    .inputStreamType = IN_STREAM_I2S    //this is recording stream that records data from mic
    | IN_STREAM_HTTP   //http stream can support fetching data from a website and play it
    | IN_STREAM_LIVE   //supports m3u8 stream playing.
    | IN_STREAM_FLASH  //support flash music playing, refer to [Flash Music] section in "docs/ESP-Audio_Programming_Guide.md" for detail
    | IN_STREAM_SDCARD //support TF-Card playing. Refer to "SDCard***.c" for more information
    | IN_STREAM_ARRAY  //for tone stored in array. Refre to 'EspAudioTonePlay()' for details
    ,
    .outputStreamType = OUT_STREAM_I2S      //output the music via i2s protocol, see i2s configuration in "MediaHal.c"
    | OUT_STREAM_SDCARD  //write the music data into TF-Card, usually for recording
    ,

#ifndef CONFIG_CLASSIC_BT_ENABLED
    .playListEn = ESP_AUDIO_ENABLE,
#else
    .playListEn = ESP_AUDIO_ENABLE,
#endif
    .toneEn = ESP_AUDIO_ENABLE,
    .playMode = MEDIA_PLAY_ONE_SONG,
};


void Init_MediaCodec()
{
    int ret = 0;
#if (defined CONFIG_CODEC_CHIP_IS_ES8388)
    Es8388Config  Es8388Conf =  AUDIO_CODEC_ES8388_DEFAULT();
    ret = MediaHalInit(&Es8388Conf);
    if (ret) {
        ESP_AUDIO_LOGE(APP_TAG, "MediaHal init failed, line:%d", __LINE__);
    }
    ESP_AUDIO_LOGI(APP_TAG, "CONFIG_CODEC_CHIP_IS_ES8388");
#elif (defined CONFIG_CODEC_CHIP_IS_ES8374)
    Es8374Config  Es8374Conf =  AUDIO_CODEC_ES8374_DEFAULT();
    ret = MediaHalInit(&Es8374Conf);
    if (ret) {
        ESP_AUDIO_LOGI(APP_TAG, "MediaHal init failed, line:%d", __LINE__);
    }
    ESP_AUDIO_LOGI(APP_TAG, "CONFIG_CODEC_CHIP_IS_ES8374");
#endif

}

void board_app_setup()
{
    GpioInterInstall();
    headphone_intr_init();
    led_config_t led_cfg = LED_LIGHT_CONFIG_DEFAULT();
    LedInit(&led_cfg);
    nvs_flash_init();
    tcpip_adapter_init();
    Init_MediaCodec();

    MediaControl *player;
    if (EspAudioInit(&playerConfig, &player) != AUDIO_ERR_NO_ERROR)
        return;
    EspAudioSamplingSetup(48000);
    /*--------------------------------------------------------------
    |    Any service can be commented out to disable such feature   |
    ---------------------------------------------------------------*/

    /* Touch and Button service, refer to "components//DeviceController/TouchManager" directory*/
    TouchControlService *touch = TouchControlCreate();
    player->addService(player, (MediaService *)touch);

    /* shell service (uart terminal), refer to "/components/TerminalService" directory*/
    TerminalControlService *term = TerminalControlCreate();
    player->addService(player, (MediaService *) term);

    /*------------------------------------------------------------------------------------
    |    Wi-Fi and TF-Card and Button and Auxiliary are components of DeviceController    |
    |    Any device can be commented out to disable such feature                          |
    -------------------------------------------------------------------------------------*/
    player->deviceController = DeviceCtrlCreate(player);
    DeviceController *deviceController =  player->deviceController;

    ESP_LOGI(APP_TAG, "Manually select audio lib");

    EspAudioCodecLibAdd(CODEC_DECODER, 5 * 1024, "aac", aac_decoder_open, aac_decoder_process, aac_decoder_close, aac_decoder_trigger_stop, aac_decoder_get_pos);

    EspAudioCodecLibAdd(CODEC_DECODER, 5 * 1024, "amr", amr_decoder_open, amr_decoder_process, amr_decoder_close, NULL, amr_decoder_get_pos);
    EspAudioCodecLibAdd(CODEC_DECODER, 5 * 1024, "mp3", mp3_decoder_open, mp3_decoder_process, mp3_decoder_close, mp3_decoder_trigger_stop, mp3_decoder_get_pos);
    EspAudioCodecLibAdd(CODEC_DECODER, 3 * 1024, "wav", wav_decoder_open, wav_decoder_process, wav_decoder_close, NULL, wav_decoder_get_pos);
    //EspAudioCodecLibAdd(CODEC_DECODER, 3 * 1024, "pcm", pcm_decoder_open, pcm_decoder_process, pcm_decoder_close, NULL, pcm_decoder_get_pos);


    /* encoder */
    EspAudioCodecLibAdd(CODEC_ENCODER, 8 * 1024, "amr", amrnb_encoder_open, amrnb_encoder_process, amrnb_encoder_close, amrnb_encoder_trigger_stop, NULL); ///according to my test, 6 will stack overflow. 7 is ok. Here use 8
    EspAudioCodecLibAdd(CODEC_ENCODER, 50 * 1024, "opus", opus_encoder_open, opus_encoder_process, opus_encoder_close, NULL, NULL); ///for 1 channe, 35k is OK, for 2 ch, need more stack 50k
    EspAudioCodecLibAdd(CODEC_ENCODER, 2 * 1024, "pcm", pcm_encoder_open, pcm_encoder_process, pcm_encoder_close, NULL, NULL);

    deviceController->enableAuxIn(deviceController);
    deviceController->enableTouch(deviceController);
    deviceController->enableWifi(deviceController);
    deviceController->enableSDcard(deviceController);
    if ((playerConfig.inputStreamType & IN_STREAM_SDCARD) || (playerConfig.outputStreamType & IN_STREAM_SDCARD))
        if (playerConfig.inputStreamType & IN_STREAM_HTTP) {
            /* A http service communicating with a cellphone installed with ESP-App, refer to "/components/EspAppService" directory*/
            EspAppControlService *mobile = EspAppControlCreate();
            player->addService(player, (MediaService *) mobile);
#if 0
            /* Duer OS voice recognition*/
#include "DuerAppService.h"

            DuerAppService *talk = DuerAppServiceCreate();
            player->addService(player, (MediaService *) talk);
#endif
    }

#ifdef CONFIG_ENABLE_TURINGAIWIFI_SERVICE
    TuringAiwifiService *turingaiwifi = AiwifiControllerServiceCreate();
    player->addService(player, (MediaService *) turingaiwifi);
#ifdef CONFIG_ENABLE_TURING_WAKEUP
    EspAudioSamplingSetup(48000);
    TuringWakeupService *turingwakeup = TuringWakeupServiceCreate();
    player->addService(player, (MediaService *) turingwakeup);
#endif
#endif

#ifdef CONFIG_ENABLE_TURINGWECHAT_SERVICE
    TuringWechatService *turingwechat = WechatControllerServiceCreate();
    player->addService(player, (MediaService *) turingwechat);
#endif


#if (defined CONFIG_ENABLE_TURINGAIWIFI_SERVICE || defined CONFIG_ENABLE_TURINGWECHAT_SERVICE)
    AlarmService *alarm = AlarmServiceCreate();
    player->addService(player, (AlarmService *) alarm);
#endif

	DdMqttService *ddmqtt = DdmqttServiceCreate();
    player->addService(player, (DdMqttService *) ddmqtt);

	DdHttpService *ddhttp = DdHttpServiceCreate();
	player->addService(player, (DdHttpService *) ddhttp);

	DdCloudService *ddcloud = DdCloudServiceCreate();
	player->addService(player, (DdCloudService *) ddcloud);

#if CONFIG_ENABLE_OTA_SERVICE
    /* Over-The-Air software upgrade, refer to "/components/OtaService" directory */
    OtaService *ota = OtaServiceCreate();
    player->addService(player, (MediaService *) ota);
#endif


    /* Start all the services and device-controller*/
    player->activeServices(player);
    ESP_AUDIO_LOGI(APP_TAG, "[APP] This board is %s, freemem: %d...\r\n", BOARD_INFO, esp_get_free_heap_size());
}
#endif // (defined CONFIG_ESP_LYRAT_V4_3_BOARD || defined CONFIG_ESP_LYRAT_V4_2_BOARD)


#include "configuration.h"

#ifdef T_LORA_PAGER

#include "AudioBoard.h"
#include "PagerNotifications.h"

DriverPins PinsAudioBoardES8311;
AudioBoard board(AudioDriverES8311, PinsAudioBoardES8311);

// Used by PagerNotifications (which can't include AudioBoard.h alongside main.h)
void pagerSetCodecVolume(uint8_t percent)
{
    board.setVolume(percent);
}

// TLora Pager specific init
void lateInitVariant()
{
    // AudioDriverLogger.begin(Serial, AudioDriverLogLevel::Debug);
    // I2C: function, scl, sda
    PinsAudioBoardES8311.addI2C(PinFunction::CODEC, Wire);
    // I2S: function, mclk, bck, ws, data_out, data_in
    PinsAudioBoardES8311.addI2S(PinFunction::CODEC, DAC_I2S_MCLK, DAC_I2S_BCK, DAC_I2S_WS, DAC_I2S_DOUT, DAC_I2S_DIN);

    // configure codec
    CodecConfig cfg;
    cfg.input_device = ADC_INPUT_LINE1;
    cfg.output_device = DAC_OUTPUT_ALL;
    cfg.i2s.bits = BIT_LENGTH_16BITS;
    cfg.i2s.rate = RATE_44K;
    board.begin(cfg);

    // Sound level (Off/1-5) is a user setting: System menu > Notifications, or Shift+Enter / Shift+Backspace /
    // Shift+Space on the keyboard. This loads it and sets the codec volume.
    PagerNotifications::begin();
}
#endif
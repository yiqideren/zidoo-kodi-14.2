 /*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "AESinkAUDIOTRACK.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/AudioEngine/Utils/AERingBuffer.h"
#include "android/activity/XBMCApp.h"
#include "settings/Settings.h"
#if defined(HAS_LIBAMCODEC)
#include "utils/AMLUtils.h"
#endif
#include "utils/log.h"

#include "android/jni/AudioFormat.h"
#include "android/jni/AudioManager.h"
#include "android/jni/AudioTrack.h"

using namespace jni;

#define AW_AUDIO_RAW "/data/audio/audio_raw"

int write_node_value(const char *path, const char *val)
{
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC);
  if (fd >= 0)
  {
    int ret = write(fd, val, strlen(val));
	if (ret < 0)
	{
      CLog::Log(0, "####[star] write %s failed: %s", val, strerror(errno));
	  return -1;
	}
    CLog::Log(0, "####[star] write %s ok", val);
    close(fd);
    return 0;
  }
  else
  {
    CLog::Log(0, "####[star] open %s failed: %s", path, strerror(errno));
    return -1;
  }
}

int chmod_node(const char *path, const char *mode)
{
  char cmd[64];
  sprintf(cmd, "chmod %s %s", mode, path);
  int ret = system(cmd);
  if (ret < 0)
  {
    CLog::Log(0, "####[star] %s failed: %s", cmd, strerror(errno));
    return -1;
  }
  else
  {
    CLog::Log(0, "####[star] %s ok", cmd);
  }
  return 0;
}


#if 0 //defined(__ARM_NEON__)
#include <arm_neon.h>
#include "utils/CPUInfo.h"

// LGPLv2 from PulseAudio
// float values from AE are pre-clamped so we do not need to clamp again here
static void pa_sconv_s16le_from_f32ne_neon(unsigned n, const float32_t *a, int16_t *b)
{
  unsigned int i;

  const float32x4_t half4     = vdupq_n_f32(0.5f);
  const float32x4_t scale4    = vdupq_n_f32(32767.0f);
  const uint32x4_t  mask4     = vdupq_n_u32(0x80000000);

  for (i = 0; i < (n & ~3); i += 4)
  {
    const float32x4_t v4 = vmulq_f32(vld1q_f32(&a[i]), scale4);
    const float32x4_t w4 = vreinterpretq_f32_u32(
      vorrq_u32(vandq_u32(vreinterpretq_u32_f32(v4), mask4), vreinterpretq_u32_f32(half4)));
    vst1_s16(&b[i], vmovn_s32(vcvtq_s32_f32(vaddq_f32(v4, w4))));
  }
  // leftovers
  for ( ; i < n; i++)
    b[i] = (int16_t) lrintf(a[i] * 0x7FFF);
}
#endif

/*
 * ADT-1 on L preview as of 2014-07 downmixes all non-5.1 content
 * to stereo, so use 5.1 for all multichannel content for now to
 * avoid that (except passthrough).
 * If other devices surface that support other multichannel layouts,
 * this should be disabled or adapted accordingly.
 */
#define LIMIT_TO_STEREO_AND_5POINT1 1

static const AEChannel KnownChannels[] = { AE_CH_FL, AE_CH_FR, AE_CH_FC, AE_CH_LFE, AE_CH_BL, AE_CH_BR, AE_CH_BC, AE_CH_BLOC, AE_CH_BROC, AE_CH_NULL };

static AEChannel AUDIOTRACKChannelToAEChannel(int atChannel)
{
  AEChannel aeChannel;

  /* cannot use switch since CJNIAudioFormat is populated at runtime */

       if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT)            aeChannel = AE_CH_FL;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT)           aeChannel = AE_CH_FR;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_CENTER)          aeChannel = AE_CH_FC;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_LOW_FREQUENCY)         aeChannel = AE_CH_LFE;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_BACK_LEFT)             aeChannel = AE_CH_BL;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_BACK_RIGHT)            aeChannel = AE_CH_BR;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT_OF_CENTER)  aeChannel = AE_CH_FLOC;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT_OF_CENTER) aeChannel = AE_CH_FROC;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_BACK_CENTER)           aeChannel = AE_CH_BC;
  else                                                                      aeChannel = AE_CH_UNKNOWN1;

  return aeChannel;
}

static int AEChannelToAUDIOTRACKChannel(AEChannel aeChannel)
{
  int atChannel;
  switch (aeChannel)
  {
    case AE_CH_FL:    atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT; break;
    case AE_CH_FR:    atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT; break;
    case AE_CH_FC:    atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_CENTER; break;
    case AE_CH_LFE:   atChannel = CJNIAudioFormat::CHANNEL_OUT_LOW_FREQUENCY; break;
    case AE_CH_BL:    atChannel = CJNIAudioFormat::CHANNEL_OUT_BACK_LEFT; break;
    case AE_CH_BR:    atChannel = CJNIAudioFormat::CHANNEL_OUT_BACK_RIGHT; break;
    case AE_CH_BC:    atChannel = CJNIAudioFormat::CHANNEL_OUT_BACK_CENTER; break;
    case AE_CH_FLOC:  atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT_OF_CENTER; break;
    case AE_CH_FROC:  atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT_OF_CENTER; break;
    default:          atChannel = CJNIAudioFormat::CHANNEL_INVALID; break;
  }
  return atChannel;
}

static CAEChannelInfo AUDIOTRACKChannelMaskToAEChannelMap(int atMask)
{
  CAEChannelInfo info;

  int mask = 0x1;
  for (unsigned int i = 0; i < sizeof(int32_t) * 8; i++)
  {
    if (atMask & mask)
      info += AUDIOTRACKChannelToAEChannel(mask);
    mask <<= 1;
  }

  return info;
}

static int AEChannelMapToAUDIOTRACKChannelMask(CAEChannelInfo info)
{
#ifdef LIMIT_TO_STEREO_AND_5POINT1
  if (info.Count() > 2 && info[0] != AE_CH_RAW)
    return CJNIAudioFormat::CHANNEL_OUT_5POINT1;
  else
    return CJNIAudioFormat::CHANNEL_OUT_STEREO;
#endif

  info.ResolveChannels(KnownChannels);

  int atMask = 0;

  for (unsigned int i = 0; i < info.Count(); i++)
    atMask |= AEChannelToAUDIOTRACKChannel(info[i]);

  return atMask;
}

static jni::CJNIAudioTrack *CreateAudioTrack(int sampleRate, int channelMask, int bufferSize)
{
  jni::CJNIAudioTrack *jniAt = NULL;

  try
  {
    jniAt = new CJNIAudioTrack(CJNIAudioManager::STREAM_MUSIC,
                               sampleRate,
                               channelMask,
                               CJNIAudioFormat::ENCODING_PCM_16BIT,
                               bufferSize,
                               CJNIAudioTrack::MODE_STREAM);
  }
  catch (const std::invalid_argument& e)
  {
    CLog::Log(LOGINFO, "AESinkAUDIOTRACK - AudioTrack creation (channelMask 0x%08x): %s", channelMask, e.what());
  }

  return jniAt;
}


CAEDeviceInfo CAESinkAUDIOTRACK::m_info;
////////////////////////////////////////////////////////////////////////////////////////////
CAESinkAUDIOTRACK::CAESinkAUDIOTRACK()
{
  m_alignedS16 = NULL;
  m_min_frames = 0;
  m_sink_frameSize = 0;
  m_audiotrackbuffer_sec = 0.0;
  m_volume = 1.0;
  m_at_jni = NULL;
  m_frames_written = 0;
}

CAESinkAUDIOTRACK::~CAESinkAUDIOTRACK()
{
  Deinitialize();
}

bool CAESinkAUDIOTRACK::Initialize(AEAudioFormat &format, std::string &device)
{
  m_lastFormat  = format;
  m_format      = format;
  int m_sampleRate = m_format.m_sampleRate;

  if (AE_IS_RAW(m_format.m_dataFormat))
    m_passthrough = true;
  else
    m_passthrough = false;

#if defined(HAS_LIBAMCODEC)
  if (CSettings::Get().GetBool("videoplayer.useamcodec"))
    aml_set_audio_passthrough(m_passthrough);
#endif

  int atChannelMask = AEChannelMapToAUDIOTRACKChannelMask(m_format.m_channelLayout);

  m_format.m_sampleRate     = CJNIAudioTrack::getNativeOutputSampleRate(CJNIAudioManager::STREAM_MUSIC);
  m_format.m_dataFormat     = AE_FMT_S16LE;

  while (!m_at_jni)
  {
    m_format.m_channelLayout  = AUDIOTRACKChannelMaskToAEChannelMap(atChannelMask);
    m_format.m_frameSize      = m_format.m_channelLayout.Count() *
                                (CAEUtil::DataFormatToBits(m_format.m_dataFormat) / 8);
    int min_buffer_size       = CJNIAudioTrack::getMinBufferSize( m_format.m_sampleRate,
                                                                  atChannelMask,
                                                                  CJNIAudioFormat::ENCODING_PCM_16BIT);
    m_sink_frameSize          = m_format.m_channelLayout.Count() *
                                (CAEUtil::DataFormatToBits(AE_FMT_S16LE) / 8);
    m_min_frames              = min_buffer_size / m_sink_frameSize;
    m_audiotrackbuffer_sec    = (double)m_min_frames / (double)m_format.m_sampleRate;

    m_at_jni                  = CreateAudioTrack(m_format.m_sampleRate,
                                                 atChannelMask,
                                                 min_buffer_size);

    if (!m_at_jni)
    {
      if (atChannelMask != CJNIAudioFormat::CHANNEL_OUT_STEREO &&
          atChannelMask != CJNIAudioFormat::CHANNEL_OUT_5POINT1)
      {
        atChannelMask = CJNIAudioFormat::CHANNEL_OUT_5POINT1;
        CLog::Log(LOGDEBUG, "AESinkAUDIOTRACK - Retrying multichannel playback with a 5.1 layout");
      }
      else if (atChannelMask != CJNIAudioFormat::CHANNEL_OUT_STEREO)
      {
        atChannelMask = CJNIAudioFormat::CHANNEL_OUT_STEREO;
        CLog::Log(LOGDEBUG, "AESinkAUDIOTRACK - Retrying with a stereo layout");
      }
      else
      {
        CLog::Log(LOGERROR, "AESinkAUDIOTRACK - Unable to create AudioTrack");
        return false;
      }
    }
  }

  m_format.m_frames         = m_min_frames / 2;

  m_format.m_frameSamples   = m_format.m_frames * m_format.m_channelLayout.Count();
  format                    = m_format;

  JNIEnv* jenv = xbmc_jnienv();
  // Set the initial volume
  float volume = 1.0;
  if (!m_passthrough)
    volume = m_volume;
  CXBMCApp::SetSystemVolume(jenv, volume);

  // for allwinner passthrough
  char flag_val[4];
  memset(flag_val, 0, sizeof(flag_val));
  strcpy(flag_val, m_passthrough ? "2" : "1");
  CLog::Log(LOGDEBUG, "Microphone, m_sampleRate = %d",m_sampleRate);
  switch(m_sampleRate){
	case 48000:
		strncat(flag_val,"1",sizeof("1"));
		break;
	default:
		strncat(flag_val,"0",sizeof("0"));
		break;
	}
  chmod_node(AW_AUDIO_RAW, "0666");
  system("rm /data/audio/audio_raw");
  write_node_value(AW_AUDIO_RAW, flag_val);
  chmod_node(AW_AUDIO_RAW, "0644");

  return true;
}

void CAESinkAUDIOTRACK::Deinitialize()
{
  if (!m_at_jni)
    return;

  char flag_val[4];
  memset(flag_val, 0, sizeof(flag_val));
  strcpy(flag_val, "10");	// default pcm 44100
  write_node_value(AW_AUDIO_RAW, flag_val);

  m_at_jni->stop();
  m_at_jni->flush();
  m_at_jni->release();
  
  m_frames_written = 0;

  delete m_at_jni;
  m_at_jni = NULL;
}

void CAESinkAUDIOTRACK::GetDelay(AEDelayStatus& status)
{
  if (!m_at_jni)
  {
    status.SetDelay(0);
    return;
  }

  // In their infinite wisdom, Google decided to make getPlaybackHeadPosition
  // return a 32bit "int" that you should "interpret as unsigned."  As such,
  // for wrap saftey, we need to do all ops on it in 32bit integer math.
  uint32_t head_pos = (uint32_t)m_at_jni->getPlaybackHeadPosition();

  double delay = (double)(m_frames_written - head_pos) / m_format.m_sampleRate;

  status.SetDelay(delay);
}

double CAESinkAUDIOTRACK::GetLatency()
{
#if defined(HAS_LIBAMCODEC)
  if (aml_present())
    return 0.250;
#endif
  return 0.100;
}

double CAESinkAUDIOTRACK::GetCacheTotal()
{
  // total amount that the audio sink can buffer in units of seconds
  return m_audiotrackbuffer_sec;
}

// this method is supposed to block until all frames are written to the device buffer
// when it returns ActiveAESink will take the next buffer out of a queue
unsigned int CAESinkAUDIOTRACK::AddPackets(uint8_t **data, unsigned int frames, unsigned int offset)
{
  if (!m_at_jni)
    return INT_MAX;

  uint8_t *buffer = data[0]+offset*m_format.m_frameSize;

  // write as many frames of audio as we can fit into our internal buffer.
  int written = 0;
  if (frames)
  {
    // android will auto pause the playstate when it senses idle,
    // check it and set playing if it does this. Do this before
    // writing into its buffer.
    if (m_at_jni->getPlayState() != CJNIAudioTrack::PLAYSTATE_PLAYING)
      m_at_jni->play();

    written = m_at_jni->write((char*)buffer, 0, frames * m_sink_frameSize);
    m_frames_written += written / m_sink_frameSize;
  }

  return (unsigned int)(written/m_sink_frameSize);
}

void CAESinkAUDIOTRACK::Drain()
{
  if (!m_at_jni)
    return;

  // TODO: does this block until last samples played out?
  // we should not return from drain as long the device is in playing state
  m_at_jni->stop();
  m_frames_written = 0;
}

bool CAESinkAUDIOTRACK::HasVolume()
{
  return true;
}

void  CAESinkAUDIOTRACK::SetVolume(float scale)
{
  if (!m_at_jni)
    return;

  m_volume = scale;
  if (!m_passthrough)
  {
    CXBMCApp::SetSystemVolume(xbmc_jnienv(), m_volume);
  }
}

void CAESinkAUDIOTRACK::EnumerateDevicesEx(AEDeviceInfoList &list, bool force)
{
  m_info.m_channels.Reset();
  m_info.m_dataFormats.clear();
  m_info.m_sampleRates.clear();

  m_info.m_deviceType = AE_DEVTYPE_HDMI;
  m_info.m_deviceName = "AudioTrack";
  m_info.m_displayName = "android";
  m_info.m_displayNameExtra = "audiotrack";
#ifdef LIMIT_TO_STEREO_AND_5POINT1
  m_info.m_channels = AE_CH_LAYOUT_5_1;
#else
  m_info.m_channels = KnownChannels;
#endif
  // m_info.m_sampleRates.push_back(CJNIAudioTrack::getNativeOutputSampleRate(CJNIAudioManager::STREAM_MUSIC));
  m_info.m_sampleRates.push_back(44100);
  m_info.m_sampleRates.push_back(48000);
  m_info.m_dataFormats.push_back(AE_FMT_S16LE);
  m_info.m_dataFormats.push_back(AE_FMT_AC3);
  m_info.m_dataFormats.push_back(AE_FMT_DTS);
#if 0 //defined(__ARM_NEON__)
  if (g_cpuInfo.GetCPUFeatures() & CPU_FEATURE_NEON)
    m_info.m_dataFormats.push_back(AE_FMT_FLOAT);
#endif

  list.push_back(m_info);
}


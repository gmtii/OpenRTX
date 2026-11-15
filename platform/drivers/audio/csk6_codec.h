/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CSK6_CODEC_H
#define CSK6_CODEC_H

#include <interfaces/audio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * C62 audio through the CSK6 codec and DSP: one 48 kHz stereo stream each way,
 * microphone/speaker on the left and the BK4819 on the right. The device
 * configuration is a bool, true for capture.
 */
extern const struct audioDriver csk6_codec_audio_driver;

/**
 * Configure the codec and start the DSP audio worker.
 * @return zero on success, a negative error code on failure.
 */
int csk6codec_init(void);

/** Cancel streams, join the worker and release DSP audio resources. */
void csk6codec_terminate(void);

/**
 * Enable or disable DSP audio for a board-selected route.
 * The caller handles GPIOs and radio AF muxing and ensures route compatibility.
 *
 * @param source: audio source.
 * @param sink: audio destination.
 * @param enabled: true to enable, false to disable and cancel its stream.
 */
void csk6codec_setRoute(enum AudioSource source, enum AudioSink sink,
                        bool enabled);

/**
 * \brief Capture sample just after a half of the radio input stream's double
 * buffer, when it was last filled: the stream's samples in the capture's
 * count.
 *
 * @param half: 0 for the first half, 1 for the second.
 * @param index: capture sample (48 kHz count since power on).
 * @return false if that half was not filled yet.
 */
bool csk6codec_rtxInputBoundary(int half, uint64_t *index);

/**
 * \brief When a capture sample was taken: uptime, estimated from the 48 kHz
 * sample count (before the analog and DSP delays of the capture, which are
 * not known).
 *
 * @param index: capture sample.
 * @param us: its time, in microseconds.
 * @return false until enough frames give the estimate.
 */
bool csk6codec_captureTime(uint64_t index, int64_t *us);

/**
 * \brief Capture samples taken so far.
 *
 * @return the count since power on.
 */
uint64_t csk6codec_captureCount(void);

/**
 * \brief Times the capture started since power on: it stops when no route
 * takes from it, and its sample count does not count the gap.
 *
 * @return the count.
 */
uint32_t csk6codec_captureStarts(void);

/**
 * \brief Boundaries the radio output stream went through since it started:
 * with a double buffer, the half it plays now (the stream's samples from
 * that number times half the buffer).
 *
 * @return the count.
 */
uint32_t csk6codec_rtxOutputHalves(void);

/**
 * \brief When sample n of the radio output stream leaves the DSP: its FIFO
 * and the latency the DSP states; not the analog delays.
 *
 * @param sample: sample of the stream, from its start.
 * @param us: its time, uptime in microseconds.
 * @return false until enough frames give the estimate, or before the
 * stream's first sample is played.
 */
bool csk6codec_rtxOutputTime(uint32_t sample, int64_t *us);

/**
 * \brief The playback latency the DSP states.
 *
 * @return milliseconds.
 */
uint32_t csk6codec_playLatencyMs(void);

/**
 * \brief Look for a test pulse written for the transmitter in the DSP's
 * echo reference of the DAC (built with C62_DAC_ECHO): its strongest sample
 * over the next n capture samples, to know in which capture sample a
 * written sample leaves the DAC.
 *
 * @param samples: capture samples to search.
 * @return false if the DSP gives no echo reference.
 */
bool csk6codec_echoArm(uint32_t samples);

/**
 * \brief The result of the search armed by csk6codec_echoArm().
 *
 * @param index: capture sample of the strongest echo sample.
 * @param peak: its magnitude, -1 if nothing was found.
 * @return true once the samples are searched.
 */
bool csk6codec_echoResult(uint64_t *index, int32_t *peak);

/**
 * \brief The capture layout the DSP took: channels asked for, channels
 * captured, echo channel (-1 none).
 *
 * @return a text for the console.
 */
const char *csk6codec_echoDiag(void);

#ifdef __cplusplus
}
#endif

#endif /* CSK6_CODEC_H */

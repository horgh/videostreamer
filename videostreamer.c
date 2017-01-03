//
// This library provides remuxing from a video stream (such as an RTSP URL) to
// an MP4 container. It writes a fragmented MP4 so that it can be streamed to a
// pipe.
//
// There is no re-encoding. The stream is copied as is.
//
// The logic here is heavily based on remuxing.c by Stefano Sabatini.
//

#include <errno.h>
#include <libavdevice/avdevice.h>
#include <libavutil/timestamp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "videostreamer.h"

static void
__vs_log_packet(const AVFormatContext * const,
		const AVPacket * const, const char * const);

void
vs_setup(void)
{
	// Set up library.

	// Register muxers, demuxers, and protocols.
	av_register_all();

	// Make formats available.
	avdevice_register_all();

	avformat_network_init();
}

struct VSInput *
vs_open_input(const char * const input_format_name,
		const char * const input_url, const bool verbose)
{
	if (!input_format_name || strlen(input_format_name) == 0 ||
			!input_url || strlen(input_url) == 0) {
		printf("%s\n", strerror(EINVAL));
		return NULL;
	}

	struct VSInput * const input = calloc(1, sizeof(struct VSInput));
	if (!input) {
		printf("%s\n", strerror(errno));
		return NULL;
	}


	AVInputFormat * const input_format = av_find_input_format(input_format_name);
	if (!input_format) {
		printf("input format not found\n");
		vs_destroy_input(input);
		return NULL;
	}

	if (avformat_open_input(&input->format_ctx, input_url, input_format,
				NULL) != 0) {
		printf("unable to open input\n");
		vs_destroy_input(input);
		return NULL;
	}

	if (avformat_find_stream_info(input->format_ctx, NULL) < 0) {
		printf("failed to find stream info\n");
		vs_destroy_input(input);
		return NULL;
	}


	if (verbose) {
		av_dump_format(input->format_ctx, 0, input_url, 0);
	}


	// Find the first video stream.

	input->video_stream_index = -1;

	for (unsigned int i = 0; i < input->format_ctx->nb_streams; i++) {
		AVStream * const in_stream = input->format_ctx->streams[i];

		if (in_stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
			if (verbose) {
				printf("skip non-video stream %u\n", i);
			}
			continue;
		}

		// I take the first video stream only.
		input->video_stream_index = (int) i;

		break;
	}

	if (input->video_stream_index == -1) {
		printf("no video stream found\n");
		vs_destroy_input(input);
		return NULL;
	}


	return input;
}

void
vs_destroy_input(struct VSInput * const input)
{
	if (!input) {
		return;
	}

	if (input->format_ctx) {
		avformat_close_input(&input->format_ctx);
		avformat_free_context(input->format_ctx);
	}

	free(input);
}

struct VSOutput *
vs_open_output(const char * const output_format_name,
		const char * const output_url, const struct VSInput * const input,
		const bool verbose)
{
	if (!output_format_name || strlen(output_format_name) == 0 ||
			!output_url || strlen(output_url) == 0 ||
			!input) {
		printf("%s\n", strerror(EINVAL));
		return NULL;
	}

	struct VSOutput * const output = calloc(1, sizeof(struct VSOutput));
	if (!output) {
		printf("%s\n", strerror(errno));
		return NULL;
	}


	AVOutputFormat * const output_format = av_guess_format(output_format_name,
			NULL, NULL);
	if (!output_format) {
		printf("output format not found\n");
		vs_destroy_output(output);
		return NULL;
	}

	if (avformat_alloc_output_context2(&output->format_ctx, output_format,
				NULL, NULL) < 0) {
		printf("unable to create output context\n");
		vs_destroy_output(output);
		return NULL;
	}


	// Copy the video stream.

	AVStream * const out_stream = avformat_new_stream(output->format_ctx, NULL);
	if (!out_stream) {
		printf("unable to add stream\n");
		vs_destroy_output(output);
		return NULL;
	}

	AVStream * const in_stream = input->format_ctx->streams[
		input->video_stream_index];

	if (avcodec_parameters_copy(out_stream->codecpar,
				in_stream->codecpar) < 0) {
		printf("unable to copy codec parameters\n");
		vs_destroy_output(output);
		return NULL;
	}


	if (verbose) {
		av_dump_format(output->format_ctx, 0, output_url, 1);
	}


	// Open output file.
	if (avio_open(&output->format_ctx->pb, output_url, AVIO_FLAG_WRITE) < 0) {
		printf("unable to open output file\n");
		vs_destroy_output(output);
		return NULL;
	}


	// Write file header.

	AVDictionary * opts = NULL;

	// -movflags frag_keyframe tells the mp4 muxer to fragment at each video
	// keyframe. This is necessary for it to support output to a non-seekable
	// file (e.g., pipe).
	//
	// -movflags isml+frag_keyframe is the same, except isml appears to be to
	// make the output a live smooth streaming feed (as opposed to not live). I'm
	// not sure the difference, but isml appears to be a microsoft
	// format/protocol.
	//
	// To specify both, use isml+frag_keyframe as the value.
	//
	// I found that while Chrome had no trouble displaying the resulting mp4 with
	// just frag_keyframe, Firefox would not until I also added empty_moov.
	// empty_moov apparently writes some info at the start of the file.
	if (av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov", 0) < 0) {
		printf("unable to set movflags opt\n");
		vs_destroy_output(output);
		return NULL;
	}

	if (av_dict_set_int(&opts, "flush_packets", 1, 0) < 0) {
		printf("unable to set flush_packets opt\n");
		vs_destroy_output(output);
		av_dict_free(&opts);
		return NULL;
	}

	if (avformat_write_header(output->format_ctx, &opts) < 0) {
		printf("unable to write header\n");
		vs_destroy_output(output);
		av_dict_free(&opts);
		return NULL;
	}


	// Check any options that were not set. Because I'm not sure if all are
	// appropriate to set through the avformat_write_header().
	if (av_dict_count(opts) != 0) {
		printf("some options not set\n");
		vs_destroy_output(output);
		av_dict_free(&opts);
		return NULL;
	}

	av_dict_free(&opts);


	return output;
}

void
vs_destroy_output(struct VSOutput * const output)
{
	if (!output) {
		return;
	}

	if (output->format_ctx) {
		if (av_write_trailer(output->format_ctx) != 0) {
			printf("unable to write trailer\n");
		}

		if (avio_closep(&output->format_ctx->pb) != 0) {
			printf("avio_closep failed\n");
		}

		avformat_free_context(output->format_ctx);
	}

	free(output);
}

// Read a compressed and encoded frame as a packet.
//
// Returns:
// -1 if error
// 0 if nothing useful read (e.g., non-video packet)
// 1 if read a packet
int
vs_read_packet(const struct VSInput * input, AVPacket * const pkt,
		const bool verbose)
{
	if (!input || !pkt) {
		printf("%s\n", strerror(errno));
		return -1;
	}

	memset(pkt, 0, sizeof(AVPacket));


	// Read encoded frame (as a packet).

	if (av_read_frame(input->format_ctx, pkt) != 0) {
		printf("unable to read frame\n");
		return -1;
	}


	// Ignore it if it's not our video stream.

	if (pkt->stream_index != input->video_stream_index) {
		if (verbose) {
			printf("skipping non video packet\n");
		}

		av_packet_unref(pkt);
		return 0;
	}


	if (verbose) {
		__vs_log_packet(input->format_ctx, pkt, "in");
	}

	return 1;
}

// We change the packet's pts, dts, duration, pos.
//
// We do not unref it.
//
// Returns:
// -1 if error
// 1 if wrote packet
int
vs_write_packet(const struct VSInput * const input,
		const struct VSOutput * const output, AVPacket * const pkt,
		const bool verbose)
{
	if (!input || !output || !pkt) {
		printf("%s\n", strerror(EINVAL));
		return -1;
	}

	AVStream * const in_stream  = input->format_ctx->streams[pkt->stream_index];
	AVStream * const out_stream = output->format_ctx->streams[pkt->stream_index];

	// Set pts/dts if not set. Otherwise we will receive warnings like
	//
	// [mp4 @ 0x55688397bc40] Timestamps are unset in a packet for stream 0. This
	// is deprecated and will stop working in the future. Fix your code to set
	// the timestamps properly
	//
	// [mp4 @ 0x55688397bc40] Encoder did not produce proper pts, making some up.
	if (pkt->pts == AV_NOPTS_VALUE) {
		pkt->pts = 0;
	} else {
		pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base,
				out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
	}

	if (pkt->dts == AV_NOPTS_VALUE) {
		pkt->dts = 0;
	} else {
		pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base,
				out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
	}

	pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base,
			out_stream->time_base);
	pkt->pos = -1;


	if (verbose) {
		__vs_log_packet(output->format_ctx, pkt, "out");
	}


	// Write encoded frame (as a packet).

	// av_interleaved_write_frame() works too, but I don't think it is needed.
	// Using av_write_frame() skips buffering.
	if (av_write_frame(output->format_ctx, pkt) != 0) {
		printf("unable to write frame\n");
		return -1;
	}

	return 1;
}

static void
__vs_log_packet(const AVFormatContext * const format_ctx,
		const AVPacket * const pkt, const char * const tag)
{
    AVRational * const time_base = &format_ctx->streams[pkt->stream_index]->time_base;

    printf("%s: pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           tag,
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

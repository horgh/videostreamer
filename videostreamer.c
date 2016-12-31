//
// This library provides remuxing from a video stream (such as an RTSP
// URL) to an MP4 container. It writes a fragmented MP4 so that it can be
// streamed to a pipe.
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

struct Videostreamer *
vs_open(const char * const input_format_name, const char * const input_url,
		const char * const output_format_name, const char * const output_url,
		const bool verbose)
{
	if (!input_format_name || strlen(input_format_name) == 0 ||
			!input_url || strlen(input_url) == 0 ||
			!output_format_name || strlen(output_format_name) == 0 ||
			!output_url || strlen(output_url) == 0) {
		printf("%s\n", strerror(EINVAL));
		return NULL;
	}

	struct Videostreamer * const vs = calloc(1, sizeof(struct Videostreamer));
	if (!vs) {
		printf("%s\n", strerror(errno));
		return NULL;
	}


	// Open input.

	AVInputFormat * const input_format = av_find_input_format(input_format_name);
	if (!input_format) {
		printf("input format not found\n");
		vs_destroy(vs);
		return NULL;
	}

	if (avformat_open_input(&vs->input_format_ctx, input_url, input_format,
				NULL) != 0) {
		printf("unable to open input\n");
		vs_destroy(vs);
		return NULL;
	}

	if (avformat_find_stream_info(vs->input_format_ctx, NULL) < 0) {
		printf("failed to find stream info\n");
		vs_destroy(vs);
		return NULL;
	}

	if (verbose) {
		av_dump_format(vs->input_format_ctx, 0, input_url, 0);
	}


	// Open output.

	AVOutputFormat * const output_format = av_guess_format(output_format_name,
			NULL, NULL);
	if (!output_format) {
		printf("output format not found\n");
		vs_destroy(vs);
		return NULL;
	}

	if (avformat_alloc_output_context2(&vs->output_format_ctx, output_format,
				NULL, NULL) < 0) {
		printf("unable to create output context\n");
		vs_destroy(vs);
		return NULL;
	}


	// Copy the first video stream.

	vs->video_stream_index = -1;

	for (unsigned int i = 0; i < vs->input_format_ctx->nb_streams; i++) {
		AVStream * const in_stream = vs->input_format_ctx->streams[i];

		if (in_stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
			if (verbose) {
				printf("skip non-video stream %u\n", i);
			}
			continue;
		}

		AVStream * const out_stream = avformat_new_stream(vs->output_format_ctx,
				NULL);
		if (!out_stream) {
			printf("unable to add stream\n");
			vs_destroy(vs);
			return NULL;
		}

		if (avcodec_parameters_copy(out_stream->codecpar,
					in_stream->codecpar) < 0) {
			printf("unable to copy codec parameters\n");
			vs_destroy(vs);
			return NULL;
		}

		// I take the first video stream only.
		vs->video_stream_index = (int) i;
		break;
	}

	if (vs->video_stream_index == -1) {
		printf("no video stream found\n");
		vs_destroy(vs);
		return NULL;
	}

	if (verbose) {
		av_dump_format(vs->output_format_ctx, 0, output_url, 1);
	}


	// Open output file.
	if (avio_open(&vs->output_format_ctx->pb, output_url, AVIO_FLAG_WRITE) < 0) {
		printf("unable to open output file\n");
		vs_destroy(vs);
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
		vs_destroy(vs);
		return NULL;
	}

	if (av_dict_set_int(&opts, "flush_packets", 1, 0) < 0) {
		printf("unable to set flush_packets opt\n");
		vs_destroy(vs);
		av_dict_free(&opts);
		return NULL;
	}

	if (avformat_write_header(vs->output_format_ctx, &opts) < 0) {
		printf("unable to write header\n");
		vs_destroy(vs);
		av_dict_free(&opts);
		return NULL;
	}


	// Check any options that were not set. Because I'm not sure if all are
	// appropriate to set through the avformat_write_header().
	if (av_dict_count(opts) != 0) {
		printf("some options not set\n");
		vs_destroy(vs);
		av_dict_free(&opts);
		return NULL;
	}

	av_dict_free(&opts);


	return vs;
}

void
vs_destroy(struct Videostreamer * const vs)
{
	if (!vs) {
		return;
	}

	if (vs->input_format_ctx) {
		avformat_close_input(&vs->input_format_ctx);
		avformat_free_context(vs->input_format_ctx);
	}

	if (vs->output_format_ctx) {
		// Write file trailer.
		if (av_write_trailer(vs->output_format_ctx) != 0) {
			printf("unable to write trailer\n");
		}

		if (avio_closep(&vs->output_format_ctx->pb) != 0) {
			printf("avio_closep failed\n");
		}

		avformat_free_context(vs->output_format_ctx);
	}

	free(vs);
}

int
vs_read_write(const struct Videostreamer * vs, const bool verbose)
{
	if (!vs) {
		printf("%s\n", strerror(errno));
		return -1;
	}


	// Read encoded frame (as a packet).
	AVPacket pkt;
	memset(&pkt, 0, sizeof(AVPacket));

	if (av_read_frame(vs->input_format_ctx, &pkt) != 0) {
		printf("unable to read frame\n");
		return -1;
	}


	// Ignore it if it's not our video stream.
	if (pkt.stream_index != vs->video_stream_index) {
		if (verbose) {
			printf("skipping non video packet\n");
		}
		return 0;
	}


	AVStream * const in_stream  = vs->input_format_ctx->streams[pkt.stream_index];
	AVStream * const out_stream =
		vs->output_format_ctx->streams[pkt.stream_index];

	if (verbose) {
		__vs_log_packet(vs->input_format_ctx, &pkt, "in");
	}


	// Copy packet
	pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base,
			out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
	pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base,
			out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
	pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base,
			out_stream->time_base);
	pkt.pos = -1;

	if (verbose) {
		__vs_log_packet(vs->output_format_ctx, &pkt, "out");
	}


	const int pkt_size = pkt.size;

	// Write encoded frame (as a packet).

	// av_interleaved_write_frame() works too, but I don't think it is needed.
	// Using av_write_frame() skips buffering.
	if (av_write_frame(vs->output_format_ctx, &pkt) != 0) {
		printf("unable to write frame\n");
		av_packet_unref(&pkt);
		return -1;
	}

	av_packet_unref(&pkt);

	return pkt_size;
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

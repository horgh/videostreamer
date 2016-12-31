#ifndef _VIDEOSTREAMER_H
#define _VIDEOSTREAMER_H

#include <libavformat/avformat.h>
#include <stdbool.h>

struct Videostreamer {
	AVFormatContext * input_format_ctx;
	AVFormatContext * output_format_ctx;
	int video_stream_index;
};

void
vs_setup(void);

struct Videostreamer *
vs_open(const char * const, const char * const,
		const char * const, const char * const,
		const bool);

void
vs_destroy(struct Videostreamer * const);

int
vs_read_write(const struct Videostreamer *, const bool);

#endif

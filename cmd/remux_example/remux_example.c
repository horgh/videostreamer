// This program remuxes from a given RTSP input to an mp4 container.

#include <stdbool.h>
#include <stdio.h>
#include <videostreamer.h>

int main(const int argc, const char * const * const argv)
{
	if (argc != 4) {
		printf("Usage: %s <input RTSP URL> <output mp4 file> <max frames>\n",
				argv[0]);
		return 1;
	}
	char const * const output_filename = argv[2];
	long long const max_frames = atoll(argv[3]);

	vs_setup();

	const char * const input_format = "rtsp";
	const char * const input_url = argv[1];
	const char * const output_format = "mp4";
	char output_url[4096] = {0};
	sprintf(output_url, "file:%s", output_filename);
	const bool verbose = true;

	struct VSInput * const input = vs_open_input(input_format, input_url,
			verbose);
	if (!input) {
		printf("unable to open input\n");
		return 1;
	}

	struct VSOutput * const output = vs_open_output(output_format, output_url,
			input, verbose);
	if (!output) {
		printf("unable to open output\n");
		vs_destroy_input(input);
		return 1;
	}

	long long i = 0;

	while (1) {
		AVPacket pkt;
		memset(&pkt, 0, sizeof(AVPacket));

		const int read_res = vs_read_packet(input, &pkt, verbose);
		if (read_res == -1) {
			printf("read failed\n");
			vs_destroy_input(input);
			vs_destroy_output(output);
			return 1;
		}

		if (read_res == 0) {
			continue;
		}

		const int write_res = vs_write_packet(input, output, &pkt, verbose);
		if (write_res == -1) {
			printf("write failed\n");
			vs_destroy_input(input);
			vs_destroy_output(output);
			av_packet_unref(&pkt);
			return 1;
		}

		av_packet_unref(&pkt);

		i++;
		if (i == max_frames) {
			break;
		}
	}

	vs_destroy_input(input);
	vs_destroy_output(output);

	return 0;
}

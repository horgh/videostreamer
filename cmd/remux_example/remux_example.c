#include <stdbool.h>
#include <stdio.h>
#include <videostreamer.h>

int main(const int argc, const char * const argv)
{
	if (argc != 2) {
		printf("Usage: %s <rtsp URL>\n", argv[0]);
		return 1;
	}

	vs_setup();

	const char * const input_format = "rtsp";
	const char * const input_url = argv[1];
	const char * const output_format = "mp4";
	const char * const output_url = "file:/tmp/out.mp4";
	const bool verbose = true;

	struct Videostreamer * const vs = vs_open(input_format, input_url,
			output_format, output_url, verbose);
	if (!vs) {
		printf("unable to open videostreamer\n");
		return 1;
	}

	const int max_frames = 100;
	int i = 0;

	while (1) {
		const int frame_size = vs_read_write(vs, verbose);
		if (frame_size == -1) {
			printf("read/write failed\n");
			vs_destroy(vs);
			return 1;
		}

		printf("frame size %d\n", frame_size);

		i++;
		if (i == max_frames) {
			break;
		}
	}

	vs_destroy(vs);

	return 0;
}

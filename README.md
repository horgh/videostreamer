# videostreamer
videostreamer provides a way to stream video from an input source to HTTP.
It remuxes a video input into an MP4 container which it streams to
connecting clients. This provides the ability to stream an input source
that may have limited connections (it opens at most one connection to the
input), is not accessible via HTTP, or is not easily embeddable in a
website.


## Build requirements
* ffmpeg libraries (libavcodec, libavformat, libavdevice, libavutil,
  libswresample).
  * It should work with versions 3.2.x or later.
  * It does not work with 3.0.x or earlier as it depends on new APIs.
  * I'm not sure whether it works with 3.1.x.
* C compiler. Currently it requires a compiler with C11 support.
* Go. It should work with any Go 1 version.


## Installation
* Install the build dependencies (including ffmpeg libraries and a C
  compiler).
  * On Debian/Ubuntu, these packages should include what you need:
    `git-core pkg-config libavutil-dev libavcodec-dev libavformat-dev
    libavdevice-dev`
* Build the daemon.
  * You need a working Go build environment.
  * Run `go get github.com/horgh/videostreamer`
  * This places the `videostreamer` binary at `$GOPATH/bin/videostreamer`.
* Place index.html somewhere accessible. Update the `<video>` element src
  attribute.
* Run the daemon. Its usage output shows the possible flags. There is no
  configuration file.

## Running with docker-compose

1. Copy the provided example environment file `.env.example`
```shell
cp .env.example .env
```
2. Edit the `.env` environment file with your config, especially the path towards your source input.

3. Run the app with docker-compose
```shell
docker-compose up
# docker-compose up --build # if you wish to rebuild the docker image
```

4. Use your favorite browser to open the `index.html` and you're good!

## Components
* `videostreamer`: The daemon.
* `index.html`: A small sample website with a `<video>` element which
  demonstrates how to stream from the daemon.
* `videostreamer.h`: A library using the ffmpeg libraries to read a video
  input and remux and write to another format.
* `cmd/remux_example`: A C program demonstrating using `videostreamer.h`.
  It remuxes an RTSP input to an MP4 file.


## Background
A friend has a camera that publishes an RTSP feed containing h264 video. It
permits a limited number of connections, and being RTSP is not easily
accessible on the web, such as in an HTML5 `<video>` element. videostreamer
solves both of these problems. It reads the RTSP feed as input, remuxes the
h264 video into an MP4 container, and streams it to HTTP clients. No matter
how many clients are streaming the video, videostreamer only opens one RTSP
connection.

Browsers support h264 in an MP4 container, so no transcoding is necessary.
However, in order to stream the MP4, it is fragmented using libavformat's
`frag_keyframe` option. For Firefox I also had to set the `empty_moov`
option.


# Difference from audiostreamer
I have a project for streaming audio called
[audiostreamer](https://github.com/horgh/audiostreamer). I wrote it before this
one, and seeing it was successful and gave me experience with the ffmpeg
libraries, I decided to build this too.

Initially I was going to make one larger project to stream either video or audio
(or both), but I found that video behaved a bit differently than audio.
Specifically, audiostreamer is able to create a single MP3 container and then
stream that to clients, starting at any frame position. For the video container
I wanted to use, I found this was not possible. First, I always needed to send
a header to each client, which meant I needed to keep the header around in some
way to send to every client. I found that I could not gain access to the header
from the libavformat API. Second, I was not able to read individual frames out
and send them independently like I could do with MP3. This meant it was
impossible to know where in the MP4 container's output I could begin sending to
a new client when another client was already streaming.

Since it became clear that I would have to structure the program quite
differently, and I was happy with how audiostreamer was, I decided to build
videostreamer as a separate project.

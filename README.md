videostreamer provides a way to stream video from an input source to HTTP. It
remuxes a given video input into an MP4 container which it then streams
endlessly to any connecting clients. This provides the ability to stream an
input source that may have limited connections or not accessible via HTTP.


# Background
A friend has a camera that publishes an RTSP feed containing h264 video. It
permits a limited number of connections, and being RTSP is not easily
accessible on the web, such as in a regular HTML5 `<video>` element. This
project solves both of these problems. It reads the RTSP feed as input, remuxes
the h264 video into an MP4 container, and streams that to HTTP clients. No
matter how many clients are streaming the video, videostreamer only opens one
RTSP connection.

Happily, browsers support h264 inside an MP4 container, so no transcoding is
necessary. However, in order to stream the MP4, it is fragmented using
libavformat's `frag_keyframe` option. I found that in Firefox, I also had to
set the `empty_moov` option.


# Difference from audiostreamer
I have a similar project for streaming audio called
[audiostreamer](https://github.com/horgh/audiostreamer). I wrote it before this
one, and seeing it was succesful and gave me experience with the ffmpeg
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
a client that connected when another client was already streaming.

Since it became clear that I would have to structure the program quite
differently, and I was happy with how audiostreamer was, I decided to split them
into two projects.


# Requirements
  * ffmpeg (libavcodec, libavformat, libavdevice, libavutil, libswresample). I
    developed using 3.2.2. On Debian this is in the package libavutil-dev.
  * C compiler. I developed using gcc 6.2.1.
  * Go. I developed using 1.7.3.


# Installation
  * go get github.com/horgh/videostreamer
  * go build
  * Place index.html somewhere accessible. Update the `<video>` element src
    attribute.
  * Run the daemon. Its usage output shows the possible flags. There is no
    configuration file.


# Components
  * videostreamer: A daemon. The main component of the project.
  * index.html: A small sample website with a `<video>` element which
    demonstrates how to stream from the daemon.
  * videostreamer.h: A library using the ffmpeg libraries to read a video input
    and remux and write to another format.
  * cmd/remux_example: A C program demonstrating using videostreamer.h. It
    remuxes from a given RTSP input to an MP4 file.

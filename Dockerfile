FROM golang:1.13-buster AS build
RUN apt-get update && apt-get install -y git-core pkg-config libavutil-dev libavcodec-dev libavformat-dev libavdevice-dev
WORKDIR /videostreamer
ADD . /videostreamer
RUN go build

FROM debian:buster
RUN apt-get update && apt-get install -y libavutil56 libavcodec58 libavformat58 libavdevice58
COPY --from=build /videostreamer/videostreamer /
CMD ["/videostreamer"]

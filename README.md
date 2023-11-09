# GstNvStabilize
This is gstreamer plugin to stabilize video input on nvidia devices. Tested on jetson nano.

Steps to compile the `gst-nvstabilize` sources natively:

1) Install gstreamer related packages on target using the command:

        sudo apt-get install libgstreamer1.0-dev \
                gstreamer1.0-plugins-base \
                gstreamer1.0-plugins-good \
                libgstreamer-plugins-base1.0-dev

2) Run the following commands to build and install `libgstnvstabilize.so`:

        cd "gst-nvstabilize"
        make
        make install
        or
        DEST_DIR=<dir> make install

  Note: `make install` will copy library `libgstnvstabilize.so`
  into `/usr/lib/aarch64-linux-gnu/gstreamer-1.0` directory.


## How to run
- Video file

```bash
gst-launch-1.0 filesrc location=/home/autoro/openpilot/video/video_tehran2_1280x720.mp4 ! qtdemux ! h264parse ! omxh264dec ! queue ! videoconvert ! 'video/x-raw,width=1280,height=720,format=RGBA' ! nvstabilize ! nvvidconv ! 'video/x-raw(memory:NVMM)' ! nvvidconv ! xvimagesink
```
- csi camera (nvcamera)
```bash
gst-launch-1.0 nvarguscamerasrc sensor_id=0 sensor_mode=4  ! nvvidconv ! videoconvert ! 'video/x-raw,width=1280,height=720,format=RGBA, framerate=30/1' ! nvstabilize ! 'video/x-raw,width=1280,height=720' ! nvvidconv ! 'video/x-raw(memory:NVMM)' ! nvvidconv ! xvimagesink
```
## Useful links:
- https://www.khronos.org/registry/OpenVX/specs/1.2/html/page_design.html#sec_host_memory
- https://www.khronos.org/files/openvx-12-reference-card.pdf
- https://on-demand.gputechconf.com/gtc/2016/presentation/s6739-thierry-lepley-visionworks-toolkit-programming.pdf
- https://developer.ridgerun.com/wiki/index.php?title=GStreamer_Video_Stabilizer_for_NVIDIA_Jetson_Boards/Examples/Nano,TX1,TX2,Xavier_Pipelines
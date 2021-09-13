# GstNvStabilize

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

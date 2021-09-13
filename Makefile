# Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice, this
#    list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its
#    contributors may be used to endorse or promote products derived from
#    this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

SO_NAME := libgstnvstabilize.so

CXX := g++

GST_INSTALL_DIR?=/usr/lib/aarch64-linux-gnu/gstreamer-1.0/
LIB_INSTALL_DIR?=/usr/lib/aarch64-linux-gnu/tegra/
CFLAGS:=
LIBS:= -lnvbuf_utils

STABILIZE_LIB += -L video_stabilizer/libs/ -l:libstabilize.a

SRCS := $(wildcard *.cpp)

INCLUDES += -I./

PKGS := gstreamer-1.0 \
	gstreamer-base-1.0 \
	gstreamer-video-1.0 \
	gstreamer-allocators-1.0 \
	glib-2.0

EXTERNAL_CFLAGS += $(shell pkg-config --cflags cudart-10.2)
EXTERNAL_LIBS += $(shell pkg-config --libs cudart-10.2)
EXTERNAL_CFLAGS += $(shell pkg-config --cflags visionworks)
EXTERNAL_LIBS += $(shell pkg-config --libs visionworks)
CUDA_CFLAGS := $(shell pkg-config --cflags cudart-10.2)
CUDA_LIB_PATH := $(subst -L$(PKG_CONFIG_SYSROOT_DIR),,$(shell pkg-config --libs-only-L cudart-10.2))
LDFLAGS += -Wl,-rpath=$(CUDA_LIB_PATH)

DEP:=video_stabilizer/video_stabilizer.a
# DEP_FILES:=$(wildcard video_stabilizer/libs/lib.* )
# DEP_FILES-=$(DEP)


OBJS := $(SRCS:.cpp=.o)
# OBJS += video_stabilizer/nvxio/src/NVX/FrameSource/ConvertFrame.o

CXXFLAGS += -fPIC -fpermissive

CXXFLAGS += `pkg-config --cflags $(PKGS)`

LDFLAGS = -Wl,--no-undefined -L$(LIB_INSTALL_DIR) -Wl,-rpath,$(LIB_INSTALL_DIR)

LIBS += -shared -Wl,-no-undefined \
	`pkg-config --libs $(PKGS)` \
	-L video_stabilizer/libs -lstabilize -lnvx -lovx \
	-L/usr/local/cuda-10.2/lib64/ -lcudart -ldl \
	$(EXTERNAL_LIBS) \
	-L$(LIB_INSTALL_DIR) -Wl,-rpath,$(LIB_INSTALL_DIR)

CXXFLAGS += -DCUDA_API_PER_THREAD_DEFAULT_STREAM -DUSE_GUI=1 -DUSE_GLFW=1 -DUSE_GLES=1 -DUSE_GSTREAMER=1 -DUSE_NVGSTCAMERA=1 -DUSE_GSTREAMER_OMX=1

NVXIO_CFLAGS := -Ivideo_stabilizer/nvxio/include -Ivideo_stabilizer/nvxio/src/ -Ivideo_stabilizer/nvxio/src/NVX/
NVXIO_LIBS += -L video_stabilizer/libs -l:libovx.a -l:libnvx.a

all: $(SO_NAME)

%.o: %.cpp
	$(CXX) -c $< $(CXXFLAGS) $(NVXIO_CFLAGS) $(CUDA_CFLAGS) $(INCLUDES) -o $@

%.o: %.cpp Makefile
	@echo $(CFLAGS) $(CXXFLAGS) $(NVXIO_CFLAGS) $(CUDA_CFLAGS) $(INCLUDES)
	$(CXX) -c -o $@ $(CFLAGS) $(CXXFLAGS) $(NVXIO_CFLAGS) $(CUDA_CFLAGS) $(INCLUDES) $< $(shell echo $<)

$(SO_NAME): $(OBJS) $(DEP) Makefile
	@echo $(CFLAGS) $(CXXFLAGS) $(NVXIO_CFLAGS) $(CUDA_CFLAGS) $(INCLUDES)
	$(CXX) -o $@ $(OBJS) $(LIBS)

$(DEP):
	$(MAKE) -C video_stabilizer/

.PHONY: install
DEST_DIR?= $(GST_INSTALL_DIR)
install: $(SO_NAME)
	cp -vp $(SO_NAME) $(DEST_DIR)

.PHONY: clean
clean:
	rm -rf $(OBJS) $(SO_NAME)

cmake_minimum_required(VERSION 3.22)
project(FFMpegSDLDemo)

set(CMAKE_CXX_STANDARD 14)

# FFmpeg的安装目录，可以通过命令"brew info ffmpeg"获取
set(FFMPEG_DIR /opt/homebrew/Cellar/ffmpeg/5.0.1)
set(SDL_DIR /opt/homebrew/Cellar/sdl2/2.0.20)

aux_source_directory(. DIR_SRCS)

# 头文件搜索路径
include_directories(${FFMPEG_DIR}/include/
        ${SDL_DIR}/include/)
# 动态链接库或静态链接库的搜索路径
link_directories(${FFMPEG_DIR}/lib/ ${SDL_DIR}/lib/)

add_executable(FFMpegSDLDemo ${DIR_SRCS})

#链接库
target_link_libraries(FFMpegSDLDemo
        #FFmpeg 库
        avcodec
        avfilter
        avformat
        avutil
        swresample #该模块可用于音频重采样，可以对数字音频进行声道数、数据格式、采样率等多种基本信息的转换
        swscale # 该模块是将图像进行格式转换的模块，比如，可以将YUV的数据转换为RGB的数据。
        #SDL
        SDL2
        )

#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#include <SDL2/SDL.h>
}

//Refresh
#define SFM_REFRESH_EVENT  (SDL_USEREVENT + 1)
//audio play end
#define SFM_AUDIOEND_EVENT  (SDL_USEREVENT + 2)



//先按照提供的acc文件播放，后续修改
#define SDL_AUDIO_BUFFER_SIZE 1024
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio
#define PLAY_REFRESH_TIME            10        //刷新时间间隔

#define MAX_AUDIO_SIZE (25 * 16 * 1024)
#define MAX_VIDEO_SIZE (25 * 256 * 1024)

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;        //首尾指针
    int nb_packets;                //包个数
    int size;                    //队列大小
    SDL_mutex *mutex;                //队列互斥锁
    SDL_cond *cond;                //队列全局变量
} PacketQueue;

typedef struct VideoState {
    AVFormatContext *pFormatCtx;
    AVCodecContext *aCodecCtx; //音频解码器
    AVCodecContext *vCodecCtx; //视频解码器
    AVFrame *audioFrame;// 解码音频过程中的使用缓存
    PacketQueue audioq; //音频队列
    unsigned int audio_buf_size;
    unsigned int audio_buf_index;
    //AVPacket audio_pkt;
//    uint8_t *audio_pkt_data;
//    int audio_pkt_size;
    uint8_t *audio_buf;

    // 做两个线程同步使用
    double audio_clock; ///音频时钟
    double video_clock; ///<pts of last decoded frame / predicted pts of next decoded frame

    AVStream *video_st; //视频流
    AVStream *audio_st; //音频流
    PacketQueue videoq;    //视频队列


    SDL_Thread *video_tid;  //视频线程id
    SDL_Thread *refresh_tid;  //视频刷新线程id
    SDL_AudioDeviceID audioID;    //audio模块打开的设备ID

    int audioindex;
    int videoindex;

} VideoState;

//SDL播放回调
void audio_callback(void *userdata, Uint8 *stream, int len);

//播放流队列初始化
void packet_queue_init(PacketQueue *q);

//播放流添加入队列
int packet_queue_put(PacketQueue *q, AVPacket *pkt);

//播放流出队列
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);

//从流中解码音频数据
int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size);

///显示视频帧
void Video_Display_To_Window(
        VideoState *is,            //video play 状态机
        AVPacket *packet,        //包
        AVFrame *pFrame,        //流
        AVFrame *pFrameYUV,    //YUV流
        AVCodecContext *pCodecCtx,        //解码上下文
        SDL_Texture *bmp,            //显示句柄
        SDL_Renderer *renderer,        //渲染器句柄
        struct SwsContext *img_convert_ctx,//转码数据
        SDL_Rect rect            //显示区域
);

int sfp_refresh_thread(void *data);    //SDL新线程入口

//调整同步时间
static double synchronize_video(VideoState *is, AVFrame *src_frame, double pts);

//音频处理
int audio_stream_component_open(VideoState *is, int stream_index);

//视频播放线程
int video_play(void *arg);

int media_read(void *arg);

int thread_exit = 0;

// 分配解码过程中的使用缓存
//AVFrame* audioFrame = avcodec_alloc_frame();
AVFrame *audioFrame = NULL;
//PacketQueue *audioq = NULL;
VideoState is = {0};

SDL_Window *screen;
SDL_Renderer *sdlRenderer;
SDL_Texture *sdlTexture;
SDL_Rect sdlRect;

#undef main

int main(int argc, char *argv[]) {
    AVFormatContext *pFormatCtx;                //视频上下文
    AVCodecContext *audioCodeCtx, *videoCodeCtx;                //解码上下文
    int i, audioindex, videoindex;
    const AVCodec *audioCodec, *videoCodec;        //解码器
    AVPacket *packet, *VideoPacket;            //打包的流数据
    AVFrame *FrameAudio, *FrameVideo, *FrameYUV;
    uint8_t *out_buffer;
    int numBytes;
    int wait_flag = 0;
    SDL_Event event;

    //const char *filename = "https://v.geilicdn.com/video/follow1563148759-6688000001842d5327fb0a20e7c7_1280_1280.mp4";
    const char *filename = "/Users/blackox626/CLionProjects/FFMpegSDLDemo/resource/media.mp4";

    //SDL thread

//    if (1 == argc) {
//        printf("%s:%d parameter error \n", __func__, __LINE__);
//        return -1;
//    }

    audioFrame = av_frame_alloc();

//==================固定的结构================start====================
    //1、注册所有的解码器
//    av_register_all();
    //printf("%s -- %d\n", __func__, __LINE__);

    //2、分配空间
    pFormatCtx = avformat_alloc_context();
    //videoCodeCtx = avformat_alloc_context();
    //printf("%s -- %d\n", __func__, __LINE__);

    if (pFormatCtx) {
        //3、打开文件
        if (avformat_open_input(&pFormatCtx, filename, NULL, NULL) != 0) //error
        {
            printf("Couldn't open input stream.\n");
            return -1;
        }
    }

    //dump 视频信息
    av_dump_format(pFormatCtx, 0, filename, 0);

    //4、检索视频流信息
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        printf("Couldn't find stream information.\n");
        return -1;
    }

    //5、查找流位置
    audioindex = -1;
    videoindex = -1;

    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        printf("cur type :%d\n", pFormatCtx->streams[i]->codecpar->codec_type);
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioindex = i;
        }
        if ((pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) && (videoindex < 0)) {
            videoindex = i;
        }
    }
    if ((videoindex < 0) && (audioindex < 0)) {
        printf("not fine audio or video stream! \n");
        return -1;
    }

    printf("audioindex:%d, videoindex:%d\n", audioindex, videoindex);

    //6、解码上下文
//    audioCodeCtx = pFormatCtx->streams[audioindex]->codec;
//    videoCodeCtx = pFormatCtx->streams[videoindex]->codec;
    printf("%s -- %d\n", __func__, __LINE__);

    //7、、获取视频解码器
    audioCodec = avcodec_find_decoder(pFormatCtx->streams[audioindex]->codecpar->codec_id);
    audioCodeCtx = avcodec_alloc_context3(audioCodec);
    avcodec_parameters_to_context(audioCodeCtx, pFormatCtx->streams[audioindex]->codecpar);

    videoCodec = avcodec_find_decoder(pFormatCtx->streams[videoindex]->codecpar->codec_id);
    videoCodeCtx = avcodec_alloc_context3(videoCodec);
    avcodec_parameters_to_context(videoCodeCtx, pFormatCtx->streams[videoindex]->codecpar);


    if (audioCodec == NULL) {
        printf("Codec not found.\n");
        return -1;
    }

    //8、、打开解码器
    if (avcodec_open2(audioCodeCtx, audioCodec, NULL) < 0) {
        printf("Could not open audio codec.\n");
        return -1;
    }

    if (avcodec_open2(videoCodeCtx, videoCodec, NULL) < 0) {
        printf("Could not open video codec.\n");
        return -1;
    }
//======================固定的结构===========end====================


//=================SDL=============start===========

    int screen_w = 0, screen_h = 0;

    screen_w = videoCodeCtx->width;
    screen_h = videoCodeCtx->height;

//    printf("%s -- %d\n", __func__, __LINE__);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        printf("Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }

    //SDL 2.0 Support for multiple windows
    //创建窗口
    screen = SDL_CreateWindow("Simplest ffmpeg player's Window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              screen_w, screen_h,
                              SDL_WINDOW_OPENGL);

    if (!screen) {
        printf("SDL: could not create window - exiting:%s\n", SDL_GetError());
        return -1;
    }
    //创建一个渲染器
    sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
    //IYUV: Y + U + V  (3 planes)
    //YV12: Y + V + U  (3 planes)
    sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, screen_w,
                                   screen_h);

    sdlRect.x = 0;
    sdlRect.y = 0;
    sdlRect.w = screen_w;
    sdlRect.h = screen_h;

    //video statu init
//    is.pFormatCtx = avformat_alloc_context();
    is.pFormatCtx = pFormatCtx;
    is.audioFrame = av_frame_alloc();
    is.aCodecCtx = audioCodeCtx;
    is.vCodecCtx = videoCodeCtx;

    is.audio_st = pFormatCtx->streams[audioindex];
    is.video_st = pFormatCtx->streams[videoindex];

    is.audio_buf = (uint8_t *) malloc((AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2);
//    is.video_tid = SDL_CreateThread(video_play, "video_play", &is);

    is.audioindex = audioindex;
    is.videoindex = videoindex;

    //printf("%s -- %d\n", __func__, __LINE__);

    ///  打开SDL播放设备 - 结束
//=================SDL=============end=============
    //创建一个队列，并且初始化
    audio_stream_component_open(&is, audioindex);

    is.refresh_tid = SDL_CreateThread(sfp_refresh_thread, "play thread", &is); //创建一个线程定时刷新
    if (!is.refresh_tid) {
        printf("video play create thread fail : %s\n", SDL_GetError());
    }

    SDL_CreateThread(media_read, "read thread", &is);

    video_play(&is);

    wait_flag = 1;
    do {
//        printf("main thread loop\n");
        SDL_WaitEvent(&event);
        if (SFM_AUDIOEND_EVENT == event.type) {
            printf("main thread wait audio thread exit!\n");
            wait_flag = 0;
        }
    } while (wait_flag);

//    //等待ideo play线程退出
//    SDL_WaitThread(is.video_tid, NULL);

    av_free(FrameAudio);
    avcodec_close(audioCodeCtx);// Close the codec
    avformat_close_input(&pFormatCtx);// Close the video file

    SDL_Quit();

    return 0;
}


void audio_callback(void *userdata, Uint8 *stream, int len) {
    VideoState *is = (VideoState *) userdata;
    int len1, audio_data_size;
    int n = 0;

    if ((NULL != is) && (NULL != stream))  //参数检查
    {
        int audio_buf_size = (AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2;

        /*   len是由SDL传入的SDL缓冲区的大小，如果这个缓冲未满，我们就一直往里填充数据 */
        while (len > 0) {
            /*  audio_buf_index 和 audio_buf_size 标示我们自己用来放置解码出来的数据的缓冲区，*/
            /*   这些数据待copy到SDL缓冲区， 当audio_buf_index >= audio_buf_size的时候意味着我*/
            /*   们的缓冲为空，没有数据可供copy，这时候需要调用audio_decode_frame来解码出更
             /*   多的桢数据 */

            if (is->audio_buf_index >= is->audio_buf_size) {
                audio_data_size = audio_decode_frame(is, is->audio_buf, audio_buf_size/*sizeof(audio_buf)*/);
                /* audio_data_size < 0 标示没能解码出数据，我们默认播放静音 */
                if (audio_data_size < 0) {
                    /* silence */
                    is->audio_buf_size = 1024;
                    /* 清零，静音 */
                    memset(is->audio_buf, 0, is->audio_buf_size);
                } else {
                    is->audio_buf_size = audio_data_size;
                }
                is->audio_buf_index = 0;
            }
            /*  查看stream可用空间，决定一次copy多少数据，剩下的下次继续copy */
            len1 = is->audio_buf_size - is->audio_buf_index;
            if (len1 > len) {
                len1 = len;
            }

            //音频PTS暂时现这样写，坑了再改
            if (len1 && is->audio_st->codecpar) {
                //n = 2 * is->audio_st->codec->channels;
//                n = is->audioFrame->channels * is->audio_st->codecpar->channels;
                is->audio_clock += (double) len1 / (double) (8 * is->audio_st->codecpar->sample_rate);
            }

            memcpy(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, len1);
            len -= len1;
            stream += len1;
            is->audio_buf_index += len1;
        }
    }
}

//初始化队列
void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

//数据进入到队列中
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pkt1;
    if (!pkt) {
        return -1;
    }
    pkt1 = (AVPacketList *) av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    SDL_LockMutex(q->mutex);
    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

//从队列中取走数据
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            printf("pre wait %s %d ！\n", __func__, __LINE__);
            SDL_CondWait(q->cond, q->mutex);
            printf("after wait %s %d ！\n", __func__, __LINE__);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

//解码流数据到
int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size) {
    AVPacket *pkt;
    int _data_size;
    SDL_Event event;

    pkt = av_packet_alloc();

    for (;;) {
        //没有数据了，暂时认为播放完成了
        if (packet_queue_get(&is->audioq, pkt, 0) < 0) {
            event.type = SFM_AUDIOEND_EVENT;
            SDL_PushEvent(&event);

            return -1;
        }

        if (pkt->size <= 0) {
            event.type = SFM_AUDIOEND_EVENT;
            SDL_PushEvent(&event);
            return 0;
        }

        int audio_pkt_size = pkt->size;
//        std::cout << "audio_pkt_size：" << audio_pkt_size << std::endl;

        int ret = avcodec_send_packet(is->aCodecCtx, pkt);

        if (ret == AVERROR(EAGAIN)) {
            std::cout << "发送解码EAGAIN：" << std::endl;
        } else if (ret < 0) {
            char error[1024];
            av_strerror(ret, error, 1024);
            std::cout << "发送解码失败：" << error << std::endl;
            return _data_size;
        }
        while (true) {
            ret = avcodec_receive_frame(is->aCodecCtx, audioFrame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                std::cout << "音频解码失败：" << std::endl;
                return _data_size;
            }
            // 每个采样数据量的大小
            int data_size = av_get_bytes_per_sample(is->aCodecCtx->sample_fmt);

            if (av_sample_fmt_is_planar(is->aCodecCtx->sample_fmt)) {
//                std::cout << "pcm planar模式" << std::endl;
                for (int i = 0; i < audioFrame->nb_samples; i++) {
                    for (int ch = 0; ch < is->aCodecCtx->channels; ch++) {
                        // 需要储存为pack模式
//                            fwrite(aFrame->data[ch] + data_size * i, 1, data_size, audio_pcm);
                        memcpy(audio_buf, audioFrame->data[ch] + data_size * i, data_size);
                        audio_buf += data_size;
                    }
                }
            } else {
                std::cout << "pcm Pack模式" << std::endl;
//                    fwrite(frame->data[0], 1, frame->linesize[0], audio_pcm);
                memcpy(audio_buf, audioFrame->data[0], audioFrame->linesize[0]);
            }
            _data_size = audioFrame->nb_samples * 2 * 4;

            return _data_size;
        }
    }
}

///显示视频帧
void Video_Display_To_Window(
        VideoState *is,            //video play 状态机
        AVPacket *packet,        //包
        AVFrame *pFrame,        //流
        AVFrame *pFrameYUV,    //YUV流
        AVCodecContext *pCodecCtx,        //解码上下文
        SDL_Texture *bmp,            //显示句柄
        SDL_Renderer *renderer,        //渲染器句柄
        struct SwsContext *img_convert_ctx,//转码数据
        SDL_Rect rect            //显示区域
) {
    SDL_Rect DisplayRect = {0};
    int ret = 0;
    double video_pts = 0; //当前视频的pts
    double audio_pts = 0; //音频pts


    DisplayRect.x = rect.x;
    DisplayRect.y = rect.y;
    DisplayRect.w = rect.w;
    DisplayRect.h = rect.h;

    ret = avcodec_send_packet(pCodecCtx, packet);
    if (ret < 0) {
        std::cout << "视频发送解码失败:" << av_err2str(ret) << std::endl;
        return;
    }
    while (true) {
        ret = avcodec_receive_frame(pCodecCtx, pFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
//            std::cout << "avcodec_receive_frame：" << av_err2str(ret) << std::endl;
            break;
        } else if (ret < 0) {
            std::cout << "视频解码失败：" << std::endl;
            return;
        } else {
            if (packet->dts == AV_NOPTS_VALUE && pFrame->opaque && *(uint64_t *) pFrame->opaque != AV_NOPTS_VALUE) {
//                video_pts = *(uint64_t *) pFrame->opaque;
            } else if (packet->dts != AV_NOPTS_VALUE) {
//                video_pts = packet->dts;
            } else {
//                video_pts = 0;
            }

            video_pts = pFrame->pts;
#if 0
            printf("%s %d ！\n", __func__, __LINE__);
    printf("num:%d den:%d\n", is->video_st->time_base.num, is->video_st->time_base.den);
    printf("num:%d den:%d\n", is->aCodecCtx->time_base.num, is->aCodecCtx->time_base.den);

    video_pts *= av_q2d(is->video_st->time_base);
    printf("video_pts:%f ！\n", video_pts);
    video_pts = synchronize_video(is, pFrame, video_pts);
    printf("%s %d ！\n", __func__, __LINE__);
    printf("video_pts: %f, audio_pts:%f\n", video_pts, is->audio_clock);
#else
//            video_pts *= av_q2d(is->video_st->time_base);
//            video_pts = synchronize_video(is, pFrame, video_pts);

            video_pts = av_rescale_q(pFrame->pts, is->video_st->time_base, AV_TIME_BASE_Q);
            video_pts = synchronize_video(is, pFrame, video_pts);
#endif
            while (true) {
                //audio_clock时间再audio线程里播放，更新这个时间
                audio_pts = is->audio_clock * 1000000;
                if (video_pts <= audio_pts) break;

                int delayTime = (video_pts - audio_pts) * 1000;

                delayTime = delayTime > 5 ? 5 : delayTime;

                SDL_Delay(delayTime);
            }


            //printf("update texture to SDL widows!\n");
            sws_scale(
                    img_convert_ctx,
                    (uint8_t const *const *) pFrame->data,
                    pFrame->linesize,
                    0,
                    pCodecCtx->height,
                    pFrameYUV->data,
                    pFrameYUV->linesize
            );

//        iPitch 计算yuv一行数据占的字节数
            SDL_UpdateTexture(bmp, &DisplayRect, pFrameYUV->data[0], pFrameYUV->linesize[0]);
            //SDL_UpdateYUVTexture(bmp, &rect, pFrameYUV->data[0], pFrameYUV->linesize[0], pFrameYUV->data[1], pFrameYUV->linesize[1], pFrameYUV->data[2], pFrameYUV->linesize[2]);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, bmp, &DisplayRect, &DisplayRect);
            SDL_RenderPresent(renderer);

        }
    }



    //av_free_packet(packet);
}

//Thread
int sfp_refresh_thread(void *data) {
    SDL_Event event;
    auto *pVideoState = (VideoState *) data;
//    printf("%s %d refresh thread id:%d !\n", __func__, __LINE__, pVideoState->refresh_tid);
    //while (pVideoState->refresh_tid != 0)
    //先把消息发过去，播放线程在等消息
    do {
        event.type = SFM_REFRESH_EVENT;
        SDL_PushEvent(&event);
        //Wait x ms
        SDL_Delay(PLAY_REFRESH_TIME);
    } while (pVideoState->refresh_tid);
    return 0;
}

//调整同步时间
static double synchronize_video(VideoState *is, AVFrame *src_frame, double pts) {
    double frame_delay;

    if (pts != 0) {
        /* if we have pts, set video clock to it */
        is->video_clock = pts;
    } else {
        /* if we aren't given a pts, set it to the clock */
        pts = is->video_clock;
    }
    /* update the video clock */
//    frame_delay = av_q2d(is->vCodecCtx->time_base);
//    /* if we are repeating a frame, adjust clock accordingly */
//    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);

    frame_delay += src_frame->repeat_pict / (av_q2d(is->vCodecCtx->framerate) * 2);

    is->video_clock += frame_delay;
    return pts;
}

//音频处理
int audio_stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->pFormatCtx;
    AVCodecContext *codecCtx;
    AVCodec *codec;
    SDL_AudioSpec wanted_spec, spec;
    int64_t wanted_channel_layout = 0;
    int wanted_nb_channels;

    /*  SDL支持的声道数为 1, 2, 4, 6 */
    /*  后面我们会使用这个数组来纠正不支持的声道数目 */
    const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};

    if (stream_index < 0 || stream_index >= ic->nb_streams) {
        return -1;
    }

    printf("%s %d entry !\n", __func__, __LINE__);

    packet_queue_init(&is->audioq);  //初始化音频队列
#if 0
    wanted_spec.format = AUDIO_S16SYS; // 具体含义请查看“SDL宏定义”部分
    wanted_spec.silence = 0;            // 0指示静音
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;  // 自定义SDL缓冲区大小
    wanted_spec.callback = audio_callback;        // 音频解码的关键回调函数
    wanted_spec.userdata = is;                    // 传给上面回调函数的外带数据

    do{

        is->audioID = SDL_OpenAudioDevice(SDL_GetAudioDeviceName(0,0),0,&wanted_spec, &spec,0);

        printf("SDL_OpenAudio (%d channels): %s\n",wanted_spec.channels, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels)	//逐个匹配频道
        {
            printf("No more channel combinations to tyu, audio open failed\n");
//            return -1;
            break;
        }
    }while(is->audioID == 0);


    SDL_PauseAudioDevice(is->audioID,0);
#endif
#if 1
    ///  打开SDL播放设备 - 开始
    SDL_LockAudio();
    wanted_spec.freq = is->aCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_F32SYS;
    wanted_spec.channels = is->aCodecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = is;
    if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return -1;
    }
    SDL_UnlockAudio();
    SDL_PauseAudio(0); //开始播放
#endif
}

//视频播放线程
int video_play(void *arg) {
    //========SDL==========
    //SDL---------------------------
    int ret, got_picture;
    struct SwsContext *img_convert_ctx;
    SDL_Thread *refresh_tid;
    SDL_Event event;
    AVFrame *FrameVideo, *FrameYUV;
    int numBytes;
    uint8_t *out_buffer;
    AVPacket *packet;
    //=========SDL===end===


    VideoState *is = (VideoState *) arg;
    //初始化队列
    packet_queue_init(&is->videoq);
//    printf("%s init video queue :%p\n", __func__, &is->videoq);

    //=============video====================
    FrameVideo = av_frame_alloc();
    FrameYUV = av_frame_alloc();

//    printf("%s %d entry !\n", __func__, __LINE__);

    ///解码后数据转成YUV420P
    img_convert_ctx = sws_getContext(is->vCodecCtx->width, is->vCodecCtx->height, is->vCodecCtx->pix_fmt,
                                     is->vCodecCtx->width, is->vCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL,
                                     NULL, NULL);

    numBytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, is->vCodecCtx->width, is->vCodecCtx->height, 1);

    out_buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
    av_image_fill_arrays(FrameYUV->data, FrameYUV->linesize, out_buffer,
                         AV_PIX_FMT_YUV420P, is->vCodecCtx->width, is->vCodecCtx->height, 1);

    //int y_size = is->vCodecCtx->width * is->vCodecCtx->height;

    //packet = (AVPacket *) malloc(sizeof(AVPacket)); //分配一个packet
    //av_new_packet(packet, y_size); //分配packet的数据
    packet = av_packet_alloc();


    //================video===end===========

    //refresh_tid = SDL_CreateThread(sfp_refresh_thread, "play thread", NULL); //创建一个线程定时刷新
//    is->refresh_tid = SDL_CreateThread(sfp_refresh_thread, "play thread", is); //创建一个线程定时刷新
//    if (!is->refresh_tid) {
//        printf("video play create thread fail : %s\n", SDL_GetError());
//    }
    while (true) {
        //printf("%s %d loop\n", __func__, __LINE__);
        SDL_WaitEvent(&event);  //等待消息到来，再刷新
        //printf("%s %d\n", __func__, __LINE__);
        if (event.type == SFM_REFRESH_EVENT) {
            //第三个参数改成0，会有这样的情况：当队列中刚好为空，还没来得及往队列中存包，导致播放器意外停止
            //如果改成1的话，会导致一直阻塞在读ideo pack中：SDL_CondWait(q->cond, q->mutex);
            if (packet_queue_get(&is->videoq, packet, 0) <= 0) {
                printf("%s read pack end !\n", __func__);
                is->refresh_tid = 0;
                break;//队列里面没有数据了  读取完毕了
            }

            Video_Display_To_Window(
                    is, packet,
                    FrameVideo, FrameYUV,
                    is->vCodecCtx, sdlTexture,
                    sdlRenderer, img_convert_ctx,
                    sdlRect);
        }
    }
}

int media_read(void *arg) {
    AVPacket *packet, *VideoPacket;

    packet = av_packet_alloc();
    if (!packet) {
        printf("Could not malloc packet.\n");
        return -1;
    }

    // Debug -- Begin
//    printf("比特率 %3d\n", pFormatCtx->bit_rate);
//    printf("解码器名称 %s\n", audioCodeCtx->codec->long_name);
//    printf("time_base  %d \n", audioCodeCtx->time_base);
//    printf("声道数  %d \n", audioCodeCtx->channels);
//    printf("sample per second  %d \n", audioCodeCtx->sample_rate);
    // Debug -- End

    //主线程。读取流到对应的队列。
    while (true) {

        if ((is.audioq.size > MAX_AUDIO_SIZE) || (is.videoq.size > MAX_VIDEO_SIZE)) {
            SDL_Delay(10);  //队列限制以下大小，防止一下读取太多的包，导致内存吃完
            continue;
        }

        //流已经读取完了
        if (av_read_frame(is.pFormatCtx, packet) < 0) {
            //Exit Thread
            thread_exit = 1;
            break;
        }

        if (packet->stream_index == is.audioindex) {
            packet_queue_put(&is.audioq, packet);
            //这里我们将数据存入队列 因此不调用 av_free_packet 释放
        } else if (packet->stream_index == is.videoindex) {
            //display video stream
            packet_queue_put(&is.videoq, packet);
        } else {
            // Free the packet that was allocated by av_read_frame
            av_packet_free(&packet);
        }
    }

    printf("read finished!\n");
}


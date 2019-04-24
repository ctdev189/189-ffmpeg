/**
 * 实时加水印
 */
#include <stdio.h>

#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>

#include "../monitor.h"

/*监控执行情况*/
static t_dev189_monitor *monitor;
#define monitor_timer_LEN 7
const char *timers[monitor_timer_LEN] = {"decode", "filter", "encode", "send_frame", "receive_packet", "write_frame", "usleep"};

static AVFormatContext *pFmtCtxIn = NULL, *pFmtCtxOut = NULL;
static int iVideoStreamIndex = -1;
static AVStream *pStreamVideoIn;
static AVCodecContext *pCodecCtxIn;
static AVCodecContext *pCodecCtxOut;
static AVStream *pStreamVideoOut;

AVFilterContext *buffersink_ctx;
AVFilterContext *buffersrc_ctx;

static GAsyncQueue *queue_decoded_frames, *queue_filtered_frames;

static gboolean decode_done, filter_done, encode_done;

static void *input_to_decode_thread_handler(void *data);
static void *decoded_to_filter_thread_handler(void *data);
static void *filter_to_encode_thread_handler(void *data);
static int filter(AVFrame *pFrame);
static int encode(AVFrame *pFrame, AVPacket *pPacket, int *iFrameIndex, int64_t *iStartTime);

static int open_input(const char *filename)
{
    int ret = 0;

    pFmtCtxIn = avformat_alloc_context();
    pFmtCtxIn->iformat = av_find_input_format("sdp");
    av_opt_set(pFmtCtxIn, "protocol_whitelist", "file,udp,rtp", 0);
    av_opt_set(pFmtCtxIn, "max_analyze_duration", 0, 0);

    if ((ret = avformat_open_input(&pFmtCtxIn, filename, 0, 0)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Could not open input file.");
        return ret;
    }

    if ((ret = avformat_find_stream_info(pFmtCtxIn, 0)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Failed to retrieve input stream information.");
        return ret;
    }

    /* select the video stream */
    AVCodec *pCodecVideoIn;
    if ((ret = av_find_best_stream(pFmtCtxIn, AVMEDIA_TYPE_VIDEO, -1, -1, &pCodecVideoIn, 0)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
        return ret;
    }
    iVideoStreamIndex = ret;
    av_log(NULL, AV_LOG_INFO, "Get video stream index: %d.\n", iVideoStreamIndex);
    pStreamVideoIn = pFmtCtxIn->streams[iVideoStreamIndex];

    //Copy the settings of AVCodecContext
    pCodecCtxIn = avcodec_alloc_context3(pCodecVideoIn);
    if (pCodecCtxIn == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate AVCodecContext.\n");
        return ret;
    }
    avcodec_parameters_to_context(pCodecCtxIn, pStreamVideoIn->codecpar);

    /* init the video decoder */
    if ((ret = avcodec_open2(pCodecCtxIn, pCodecVideoIn, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
        return ret;
    }
    // Print
    av_dump_format(pFmtCtxIn, 0, filename, 0);

    return 0;
}

static int open_output(const char *filename)
{
    int ret = 0;

    avformat_alloc_output_context2(&pFmtCtxOut, NULL, "rtp", filename);
    av_opt_set_int(pFmtCtxOut->priv_data, "payload_type", 100, 0);

    if (avio_open(&pFmtCtxOut->pb, filename, AVIO_FLAG_WRITE) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Failed to open output file! \n");
        return ret;
    }
    pStreamVideoOut = avformat_new_stream(pFmtCtxOut, 0);
    if (!pStreamVideoOut)
    {
        av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream.\n");
        ret = AVERROR_UNKNOWN;
        return ret;
    }

    //Copy the settings of AVCodecContext
    AVCodec *pCodecOut = avcodec_find_encoder(pStreamVideoIn->codecpar->codec_id);
    pCodecCtxOut = avcodec_alloc_context3(pCodecOut);
    if (pCodecCtxOut == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate AVCodecContext\n");
        return ret;
    }
    avcodec_parameters_to_context(pCodecCtxOut, pStreamVideoIn->codecpar);
    pCodecCtxOut->bit_rate = 90000;
    pCodecCtxOut->width = 320;
    pCodecCtxOut->height = 240;
    pCodecCtxOut->time_base.num = 1;
    pCodecCtxOut->time_base.den = 25;
    pCodecCtxOut->gop_size = 25;
    pCodecCtxOut->max_b_frames = 1;
    pCodecCtxOut->pix_fmt = AV_PIX_FMT_YUV420P;
    pCodecCtxOut->codec_type = AVMEDIA_TYPE_VIDEO;
    //realtime|good|best
    av_opt_set(pCodecCtxOut->priv_data, "deadline", "realtime", 0);

    ret = avcodec_parameters_from_context(pStreamVideoOut->codecpar, pCodecCtxOut);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Failed to copy context from input to output stream codec context\n");
        return ret;
    }
    pStreamVideoOut->codecpar->codec_tag = 0;

    avcodec_open2(pCodecCtxOut, pCodecOut, NULL);

    //Initialize the muxer internals and write the file header.
    ret = avformat_write_header(pFmtCtxOut, NULL);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
        return ret;
    }

    //Dump Output Format
    av_dump_format(pFmtCtxOut, 0, filename, 1);

    return 0;
}

static int init_filters(const char *watermark)
{
    int ret;
    char args[512], filters_descr[256];
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    AVBufferSinkParams *buffersink_params;
    AVRational time_base = pFmtCtxIn->streams[iVideoStreamIndex]->time_base;

    AVFilterGraph *filter_graph = avfilter_graph_alloc();

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             pCodecCtxOut->width, pCodecCtxOut->height, pCodecCtxOut->pix_fmt,
             time_base.num, time_base.den,
             pCodecCtxOut->sample_aspect_ratio.num, pCodecCtxOut->sample_aspect_ratio.den);
    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
        return ret;
    }

    /* buffer video sink: to terminate the filter chain. */
    buffersink_params = av_buffersink_params_alloc();
    buffersink_params->pixel_fmts = pix_fmts;
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       NULL, buffersink_params, filter_graph);
    av_free(buffersink_params);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
        return ret;
    }

    /* Endpoints for the filter graph. */
    outputs->name = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    snprintf(filters_descr, sizeof(filters_descr), "movie=%s[wm];[in][wm]overlay=1:1[out]", watermark);

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr, &inputs, &outputs, NULL)) < 0)
        return ret;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        return ret;

    return 0;
}

/*从输入解码放到解码队列中*/
static int new_input_to_decode_thread()
{
    queue_decoded_frames = g_async_queue_new(); //解码后的frame队列

    GError *error = NULL;
    g_thread_try_new("input2decode", input_to_decode_thread_handler, NULL, &error);
    if (error != NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "Got error %d (%s) trying to launch the \'input2decode\' thread.\n",
               error->code, error->message ? error->message : "??");
        return -1;
    }

    return 0;
}

static void *input_to_decode_thread_handler(void *data)
{
    int ret;
    AVPacket packet;
    AVFrame *pFrame, *pFrameDec;

    pFrame = av_frame_alloc();
    /**
     * 从输入中解码（decode）frame，放入队列，等待后续处理
     */
    while (1)
    {
        dev189_monitor_timer_on(monitor, "decode");
        //Get an AVPacket
        if ((ret = av_read_frame(pFmtCtxIn, &packet)) < 0)
            break;
        //Only video stream
        if (packet.stream_index != iVideoStreamIndex)
            continue;
        //Decoding packet
        ret = avcodec_send_packet(pCodecCtxIn, &packet);
        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Error while sending a packet to the decoder\n");
            break;
        }
        while (1)
        {
            ret = avcodec_receive_frame(pCodecCtxIn, pFrame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                break;
            }
            else if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Error while receiving a frame from the decoder\n");
                break;
            }
            pFrame->pts = pFrame->best_effort_timestamp;

            //Deep copy frame for record
            pFrameDec = av_frame_alloc();
            pFrameDec->format = pFrame->format;
            pFrameDec->width = pFrame->width;
            pFrameDec->height = pFrame->height;
            pFrameDec->channels = pFrame->channels;
            pFrameDec->channel_layout = pFrame->channel_layout;
            pFrameDec->nb_samples = pFrame->nb_samples;
            av_frame_get_buffer(pFrameDec, 0);
            av_frame_copy(pFrameDec, pFrame);
            av_frame_copy_props(pFrameDec, pFrame);

            // end decode
            dev189_monitor_timer_off(monitor, "decode");

            g_async_queue_push(queue_decoded_frames, pFrameDec);

            av_frame_unref(pFrame);
        }

        av_packet_unref(&packet);
    }

    avcodec_free_context(&pCodecCtxIn);
    avformat_close_input(&pFmtCtxIn);

    av_log(NULL, AV_LOG_INFO, "Stop input_to_decode_thread_handler loop.\n");

    /*结束从输入中解码*/
    decode_done = TRUE;

    return NULL;
}

/*处理解码队列中的frame*/
static int new_decode_to_filter_thread()
{
    //queue_decoded_frames = g_async_queue_new(); //解码后的frame队列

    GError *error = NULL;
    g_thread_try_new("decode2filter", decoded_to_filter_thread_handler, NULL, &error);
    if (error != NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "Got error %d (%s) trying to launch the \'decode2filter\' thread.\n",
               error->code, error->message ? error->message : "??");
        return -1;
    }

    return 0;
}

static void *decoded_to_filter_thread_handler(void *data)
{
    AVFrame *pFrameDec;

    /*从解码队列中提取frame，加滤镜，并放入队列等待后续处理*/
    while (1)
    {
        pFrameDec = g_async_queue_try_pop(queue_decoded_frames);
        if (pFrameDec == NULL)
        {
            if (decode_done)
            {
                break;
            }
            else
            {
                //av_log(NULL, AV_LOG_INFO, "Wait other frames.\n");
                dev189_monitor_timer_on(monitor, "usleep");
                g_usleep(G_USEC_PER_SEC);
                dev189_monitor_timer_off(monitor, "usleep");
                continue;
            }
        }

        filter(pFrameDec);

        av_frame_unref(pFrameDec);
    }

    av_log(NULL, AV_LOG_INFO, "Stop decoded_to_filter_thread_handler loop.\n");

    filter_done = TRUE;

    return NULL;
}
/*给解码的frame加水印*/
static int filter(AVFrame *pFrameDec)
{
    int ret;
    AVFrame *pFrameNew;

    pFrameNew = av_frame_alloc();

    dev189_monitor_timer_on(monitor, "filter");
    /* push the decoded frame into the filtergraph */
    if ((ret = av_buffersrc_add_frame_flags(buffersrc_ctx, pFrameDec, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0)
    {
        dev189_monitor_timer_off(monitor, "filter");
        av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
        return ret;
    }

    /* pull filtered pictures from the filtergraph */
    while (1)
    {
        ret = av_buffersink_get_frame(buffersink_ctx, pFrameNew);
        dev189_monitor_timer_off(monitor, "filter");
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return ret;

        g_async_queue_push(queue_filtered_frames, pFrameNew);
    }

    return 0;
}
/*处理加滤镜后队列中的frame*/
static int new_filter_to_encode_thread()
{
    queue_filtered_frames = g_async_queue_new(); //加滤镜后的frame队列

    GError *error = NULL;
    g_thread_try_new("filter2encode", filter_to_encode_thread_handler, NULL, &error);
    if (error != NULL)
    {
        av_log(NULL, AV_LOG_DEBUG, "Got error %d (%s) trying to launch the new_filter_to_encode_thread...\n",
               error->code, error->message ? error->message : "??");
        return -1;
    }

    return 0;
}

static void *filter_to_encode_thread_handler(void *data)
{
    AVFrame *pFrameFil;
    AVPacket *pPacketNew = av_packet_alloc();
    int64_t iStartTime = av_gettime();
    int iFrameIndex = 0;

    while (1)
    {
        pFrameFil = g_async_queue_try_pop(queue_filtered_frames);
        if (pFrameFil == NULL)
        {
            if (filter_done)
            {
                break;
            }
            else
            {
                dev189_monitor_timer_on(monitor, "usleep");
                g_usleep(G_USEC_PER_SEC);
                dev189_monitor_timer_off(monitor, "usleep");
                continue;
            }
        }

        encode(pFrameFil, pPacketNew, &iFrameIndex, &iStartTime);

        av_frame_unref(pFrameFil);
    }

    av_packet_unref(pPacketNew);
    //Write file trailer
    av_write_trailer(pFmtCtxOut);

    av_log(NULL, AV_LOG_INFO, "Stop filter_to_encode_thread_handler loop.\n");

    encode_done = TRUE;

    return NULL;
}

/* 编码并输出 */
static int encode(AVFrame *pFrame, AVPacket *pPacket, int *iFrameIndex, int64_t *iStartTime)
{
    int ret;

    if (!pFrame)
        return -1;

    /* send the frame to the encoder */
    dev189_monitor_timer_on(monitor, "encode");

    dev189_monitor_timer_on(monitor, "send_frame");
    ret = avcodec_send_frame(pCodecCtxOut, pFrame);
    dev189_monitor_timer_off(monitor, "send_frame");

    if (ret < 0)
    {
        dev189_monitor_timer_off(monitor, "encode");
        av_log(NULL, AV_LOG_ERROR, "Error sending a frame for encoding\n");
        return ret;
    }

    while (1)
    {
        dev189_monitor_timer_on(monitor, "receive_packet");
        ret = avcodec_receive_packet(pCodecCtxOut, pPacket);
        dev189_monitor_timer_off(monitor, "receive_packet");

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        else if (ret < 0)
        {
            dev189_monitor_timer_off(monitor, "encode");
            av_log(NULL, AV_LOG_ERROR, "Error during encoding\n");
            return ret;
        }
        //Convert PTS/DTS
        if (pPacket->pts == AV_NOPTS_VALUE)
        {
            //Write PTS
            AVRational time_base1 = pStreamVideoIn->time_base;
            //Duration between 2 frames (us)
            int64_t calc_duration = (double)AV_TIME_BASE / av_q2d(pStreamVideoIn->r_frame_rate);
            //Parameters
            pPacket->pts = (double)(*iFrameIndex * calc_duration) / (double)(av_q2d(time_base1) * AV_TIME_BASE);
            pPacket->dts = pPacket->pts;
            pPacket->duration = (double)calc_duration / (double)(av_q2d(time_base1) * AV_TIME_BASE);
        }
        AVRational time_base = pStreamVideoIn->time_base;
        AVRational time_base_q = {1, AV_TIME_BASE};
        int64_t pts_time = av_rescale_q(pPacket->dts, time_base, time_base_q);
        int64_t now_time = av_gettime() - *iStartTime;
        if (pts_time > now_time)
            av_usleep(pts_time - now_time);

        pPacket->pts = av_rescale_q_rnd(pPacket->pts, pStreamVideoIn->time_base, pStreamVideoOut->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pPacket->dts = av_rescale_q_rnd(pPacket->dts, pStreamVideoIn->time_base, pStreamVideoOut->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pPacket->duration = av_rescale_q(pPacket->duration, pStreamVideoIn->time_base, pStreamVideoOut->time_base);
        pPacket->pos = -1;

        dev189_monitor_timer_on(monitor, "write_frame");
        av_write_frame(pFmtCtxOut, pPacket);
        dev189_monitor_timer_off(monitor, "write_frame");

        (*iFrameIndex)++;
        if (*iFrameIndex % 10 == 0)
            av_log(NULL, AV_LOG_INFO, "Output frames: %d\n", *iFrameIndex);

        av_packet_unref(pPacket);
    }

    dev189_monitor_timer_off(monitor, "encode");

    return 0;
}

/**
 * shell执行
 * ./rtwm.o input.sdp rtp://127.0.0.1:5034 watermark.png
*/
int main(int argc, char *argv[])
{
    const char *in_filename, *out_filename, *watermark_filename;

    if (argc <= 3)
    {
        av_log(NULL, AV_LOG_ERROR, "Usage: %s <input sdp file> <output name> <watermark name>\n", argv[0]);
        exit(0);
    }
    in_filename = argv[1];
    out_filename = argv[2];
    watermark_filename = argv[3];

    monitor = dev189_monitor_new();
    for (int i = 0; i < monitor_timer_LEN; i++)
        dev189_monitor_timer_new(monitor, timers[i]);

    /*Network*/
    avformat_network_init();

    /*Input*/
    if (open_input(in_filename) < 0)
        goto end;

    /*Output*/
    if (open_output(out_filename) < 0)
        goto end;

    /*Watermark*/
    if (init_filters(watermark_filename) < 0)
        goto end;

    /*接收输入并解码*/
    if (new_input_to_decode_thread() < 0)
        goto end;

    /*处理decoded队列*/
    if (new_decode_to_filter_thread() < 0)
        goto end;

    /*处理filtered队列*/
    if (new_filter_to_encode_thread() < 0)
        goto end;

end:
    // 等待处理完所有的frame
    while (!encode_done)
    {
        av_log(NULL, AV_LOG_INFO, "Wait 2 second, %d frames in decoded queue, %d frames in filtered queue.\n", g_async_queue_length_unlocked(queue_decoded_frames), g_async_queue_length_unlocked(queue_filtered_frames));
        g_usleep(2 * G_USEC_PER_SEC);
    }
    //Output monitor
    av_log(NULL, AV_LOG_INFO, "-----Monitor Info-----\n");
    for (int i = 0; i < monitor_timer_LEN; i++)
        av_log(NULL, AV_LOG_INFO, "\t%s\n", dev189_monitor_timer_str(monitor, timers[i]));
    dev189_monitor_free(monitor);

    return 0;
}
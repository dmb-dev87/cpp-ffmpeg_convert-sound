#include "pch.h"
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
}

#define AUDIO_INBUF_SIZE 20480
#define WRITE_U32(buf, x) *(buf)     = (unsigned char)((x)&0xff);\
                          *((buf)+1) = (unsigned char)(((x)>>8)&0xff);\
                          *((buf)+2) = (unsigned char)(((x)>>16)&0xff);\
                          *((buf)+3) = (unsigned char)(((x)>>24)&0xff);
#define WRITE_U16(buf, x) *(buf)     = (unsigned char)((x)&0xff);\
                          *((buf)+1) = (unsigned char)(((x)>>8)&0xff);

typedef struct WAV_HEADER {
    unsigned char chunkId[4];
    uint32_t chunkSize;
    unsigned char format[4];
    unsigned char subchunk1Id[4];
    uint32_t subchunk1Size;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    unsigned char subchunk2Id[4];
    uint32_t subchunk2Size;
} WavHeader;

static int DecodeAudio(AVCodecContext* dec_ctx, AVFrame* frame, AVPacket* pkt, SwrContext* swr_ctx, FILE* outfile)
{
    int ret, data_size;
    int dst_nb_samples = 0, dst_linesize;
    uint8_t** dst_data = NULL;

    ret = avcodec_send_packet(dec_ctx, pkt);

    if (ret < 0) {
        fprintf(stderr, "Error submitting the packet to the decoder %d \n", ret);
        return ret;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        else if (ret < 0) {
            fprintf(stderr, "Error duirng decoding \n");
            return ret;
        }
        data_size = av_get_bytes_per_sample(dec_ctx->sample_fmt);
        if (data_size < 0) {
            fprintf(stderr, "Failed to calculate data size\n");
            exit(1);
        }

        if (dst_nb_samples == 0) {
            dst_nb_samples = av_rescale_rnd(frame->nb_samples, 8000, 8000, AV_ROUND_UP);
            ret = av_samples_alloc_array_and_samples(&dst_data, &dst_linesize, 1, dst_nb_samples, AV_SAMPLE_FMT_S16, 0);
            ret = av_samples_alloc(dst_data, &dst_linesize, 1, dst_nb_samples, AV_SAMPLE_FMT_S16, 1);
        }
        ret = swr_convert(swr_ctx, dst_data, dst_nb_samples, (const uint8_t**)frame->data, frame->nb_samples);        

        int dst_bufsize = av_samples_get_buffer_size(&dst_linesize, 1, ret, AV_SAMPLE_FMT_S16, 1);
        int size = fwrite(dst_data[0], 1, dst_bufsize, outfile);
        return size;
    }

    return 0;
}

static int WritePrelimHeader(FILE* outfile, unsigned char* headbuf)
{
    int bytespersec = 8000 * 16 / 8;
    int align = 16 / 8;
    int samplesize = 16;
    unsigned int size = 0x7fffffff;

    memcpy(headbuf, "RIFF", 4);
    WRITE_U32(headbuf + 4, size - 8);
    memcpy(headbuf + 8, "WAVE", 4);
    memcpy(headbuf + 12, "fmt ", 4);
    WRITE_U32(headbuf + 16, 16);
    WRITE_U16(headbuf + 20, 1);  /* format */
    WRITE_U16(headbuf + 22, 1);
    WRITE_U32(headbuf + 24, 8000);
    WRITE_U32(headbuf + 28, bytespersec);
    WRITE_U16(headbuf + 32, align);
    WRITE_U16(headbuf + 34, samplesize);
    memcpy(headbuf + 36, "data", 4);
    WRITE_U32(headbuf + 40, size - 44);

    if (fwrite(headbuf, 1, 44, outfile) != 44)
    {
        fprintf(stderr, "ERROR: Failed to write wav header: \n");
        return -1;
    }

    return 0;
}

static int RewriteHeader(FILE* outfile, unsigned char* headbuf, unsigned int written)
{
    unsigned int length = written;

    length += 44;

    WRITE_U32(headbuf + 4, length - 8);
    WRITE_U32(headbuf + 40, length - 44);
    if (fseek(outfile, 0, SEEK_SET) != 0)
    {
        fprintf(stderr, "ERROR: Failed to seek on seekable file: \n");
        return 1;
    }

    if (fwrite(headbuf, 1, 44, outfile) != 44)
    {
        fprintf(stderr, "ERROR: Failed to write wav header: \n");
        return 1;
    }
    return 0;
}

EXPORT int ResampleWave(char* inputname, char* outputname)
{
    WavHeader wavHeader;
    int headerSize = sizeof(WavHeader);

    FILE* wavFile;
    fopen_s(&wavFile, inputname, "r");
    if (wavFile == nullptr)
    {
        fprintf(stderr, "Unable to open wave file: %s\n", inputname);
        return -1;
    }

    fread(&wavHeader, 1, headerSize, wavFile);
    fseek(wavFile, headerSize, SEEK_SET);

    uint8_t* wav_data = (uint8_t*)malloc(sizeof(uint8_t) * wavHeader.subchunk2Size);
    fread(wav_data, sizeof(uint8_t), wavHeader.subchunk2Size, wavFile); //read in our whole sound data chunk

    fclose(wavFile);

    uint8_t** src_data = NULL, ** dst_data = NULL;
    int src_rate = wavHeader.sampleRate, dst_rate = 8000;
    int src_nb_channels = wavHeader.numChannels, dst_nb_channels = 0;
    int src_linesize, dst_linesize;
    int64_t src_ch_layout = av_get_default_channel_layout(src_nb_channels), dst_ch_layout = AV_CH_LAYOUT_MONO;
    enum AVSampleFormat src_sample_fmt = AV_SAMPLE_FMT_S16, dst_sample_fmt = AV_SAMPLE_FMT_S16;
    int src_nb_samples, dst_nb_samples;
    static struct SwrContext* swr_ctx;
    int dst_bufsize;
    int ret;

    FILE* dstFile;

    fopen_s(&dstFile, outputname, "wb");

    if (!dstFile) {
        fprintf(stderr, "Could not open destination file %s\n", outputname);
        return -1;
    }

    uint16_t bytesPerSample = wavHeader.bitsPerSample / 8;
    src_nb_samples = wavHeader.subchunk2Size / bytesPerSample;
    src_ch_layout = av_get_default_channel_layout(src_nb_channels);

    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        fprintf(stderr, "Could not allocate resampler context\n");
        ret = AVERROR(ENOMEM);
        return -1;
    }

    av_opt_set_int(swr_ctx, "in_channel_layout", src_ch_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", src_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", src_sample_fmt, 0);

    av_opt_set_int(swr_ctx, "out_channel_layout", dst_ch_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", dst_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", dst_sample_fmt, 0);
    swr_init(swr_ctx);

    unsigned char headbuf[44];
    WritePrelimHeader(dstFile, headbuf);
    unsigned int sound_length = 0;

    ret = av_samples_alloc_array_and_samples(&src_data, &src_linesize, src_nb_channels, src_nb_samples, src_sample_fmt, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate source samples\n");
        return -1;
    }

    src_data[0] = wav_data;

    dst_nb_channels = av_get_channel_layout_nb_channels(dst_ch_layout);
    dst_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, src_rate) + src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);

    ret = av_samples_alloc_array_and_samples(&dst_data, &dst_linesize, dst_nb_channels, dst_nb_samples, dst_sample_fmt, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate destinate samples\n");
        return -1;
    }

    ret = swr_convert(swr_ctx, dst_data, dst_nb_samples, (const uint8_t**)src_data, src_nb_samples);
    if (ret < 0) {
        fprintf(stderr, "Error while converting\n");
        return -1;
    }

    dst_bufsize = av_samples_get_buffer_size(&dst_linesize, dst_nb_channels, ret, dst_sample_fmt, 1);
    if (dst_bufsize < 0) {
        fprintf(stderr, "Could not get sample buffer size\n");
        return -1;
    }

    sound_length += fwrite(dst_data[0], 1, dst_bufsize, dstFile);

    RewriteHeader(dstFile, headbuf, sound_length);

    fclose(dstFile);

    if (src_data)
        av_freep(&src_data);

    if (dst_data)
        av_freep(&dst_data);

    swr_free(&swr_ctx);

    return 1;
}

EXPORT int ConvertSound(char* inputname, char* outputname)
{
    const AVCodec* codec;
    AVCodecContext* c = NULL;
    int ret;
    FILE* infile, * outfile;
    uint8_t inbuf[AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    AVPacket* pkt;
    AVFrame* decoded_frame = NULL;
    static struct SwrContext* swr_ctx;

    pkt = av_packet_alloc();

    AVFormatContext* format = avformat_alloc_context();

    int res = avformat_open_input(&format, inputname, NULL, NULL);
    if (res != 0) {
        fprintf(stderr, "Could not open file '%s'\n", inputname);
        return -1;
    }
    if (avformat_find_stream_info(format, NULL) < 0) {
        fprintf(stderr, "Could not retrieve stream info from file '%s'\n", inputname);
        return -1;
    }

    int stream_index = -1;
    for (int i = 0; i < format->nb_streams; i++) {
        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            stream_index = i;
            break;
        }
    }
    if (stream_index == -1) {
        fprintf(stderr, "Could not retrieve audio stream from file '%s'\n", inputname);
        return -1;
    }

    AVStream* audio_stream = format->streams[stream_index];
    AVCodecParameters* params = audio_stream->codecpar;

    codec = avcodec_find_decoder(params->codec_id);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        return -1;
    }

    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate audio codec context\n");
        return -1;
    }

    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return -1;
    }

    swr_ctx = swr_alloc();

    if (!swr_ctx) {
        fprintf(stderr, "Could not allocate resampler context\n");
        return -1;
    }

    av_opt_set_int(swr_ctx, "in_channel_layout", params->channel_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", params->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", static_cast<AVSampleFormat>(params->format), 0);

    av_opt_set_int(swr_ctx, "out_channel_layout", AV_CH_LAYOUT_MONO, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", 8000, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    swr_init(swr_ctx);

    av_dump_format(format, 0, inputname, 0);

    decoded_frame = av_frame_alloc();
    if (decoded_frame == NULL) {
        fprintf(stderr, "Could not allocate frame\n");
        ret = AVERROR(ENOMEM);
        return -1;
    }

    av_init_packet(pkt);
    pkt->data = inbuf;
    pkt->size = AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE;
    pkt->stream_index = stream_index;

    int pkt_i = 0;

    fopen_s(&infile, inputname, "rb");
    if (!infile) {
        fprintf(stderr, "Could not open %s\n", inputname);
        return -1;
    }

    fopen_s(&outfile, outputname, "wb");
    if (!outfile) {
        av_free(c);
        return -1;
    }

    unsigned char headbuf[44];
    WritePrelimHeader(outfile, headbuf);
    unsigned int sound_length = 0;

    while (av_read_frame(format, pkt) >= 0) {
        if (pkt->stream_index == stream_index) {
            ret = DecodeAudio(c, decoded_frame, pkt, swr_ctx, outfile);
            if (ret > 0)
            {
                sound_length += ret;
            }
        }
        pkt_i++;

        av_packet_unref(pkt);
    }

    RewriteHeader(outfile, headbuf, sound_length);

    fclose(infile);
    fclose(outfile);    
    avcodec_free_context(&c);
    av_frame_free(&decoded_frame);
    av_packet_free(&pkt);
    avformat_close_input(&format);

    return 1;
}
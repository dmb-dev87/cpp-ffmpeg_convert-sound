// ResampleWave.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <string>
#include <fstream>
#include <cstdint>

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

static int write_prelim_header(FILE* outfile, unsigned char* headbuf)
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

static int rewrite_header(FILE* outfile, unsigned char* headbuf, unsigned int written)
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

static int resample_wave(const char* filename)
{
    WavHeader wavHeader;
    int headerSize = sizeof(WavHeader);

    FILE* wavFile;
    fopen_s(&wavFile, filename, "r");
    if (wavFile == nullptr)
    {
        fprintf(stderr, "Unable to open wave file: %s\n", filename);
        return -1;
    }

    fread(&wavHeader, 1, headerSize, wavFile);
    fseek(wavFile, headerSize, SEEK_SET);

    uint8_t* wav_data = (uint8_t*)malloc(sizeof(uint8_t) * wavHeader.subchunk2Size);
    fread(wav_data, sizeof(uint8_t), wavHeader.subchunk2Size, wavFile); //read in our whole sound data chunk

    fclose(wavFile);

    uint8_t **src_data = NULL, **dst_data = NULL;
    int src_rate = wavHeader.sampleRate, dst_rate = 8000;
    int src_nb_channels = wavHeader.numChannels, dst_nb_channels = 0;
    int src_linesize, dst_linesize;
    int64_t src_ch_layout = av_get_default_channel_layout(src_nb_channels), dst_ch_layout = AV_CH_LAYOUT_MONO;
    enum AVSampleFormat src_sample_fmt = AV_SAMPLE_FMT_S16, dst_sample_fmt = AV_SAMPLE_FMT_S16;
    int src_nb_samples, dst_nb_samples;
    static struct SwrContext* swr_ctx;
    int dst_bufsize;
    int ret;

    const char* dst_filename = "result.wav";
    FILE* dstFile;

    fopen_s(&dstFile, dst_filename, "wb");

    if (!dstFile) {
        fprintf(stderr, "Could not open destination file %s\n", dst_filename);
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
    write_prelim_header(dstFile, headbuf);
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
    
    rewrite_header(dstFile, headbuf, sound_length);

    fclose(dstFile);

    if (src_data)
        av_freep(&src_data);    

    if (dst_data)
        av_freep(&dst_data);

    swr_free(&swr_ctx);

    return 0;
}

int main(int argc, char** argv)
{
    const char* filename;

    if (argc <= 1)
    {
        fprintf(stderr, "Usage: %s <input file>\n", argv[0]);
        exit(0);
    }

    filename = argv[1];

    int ret = resample_wave(filename);

    return ret;
}

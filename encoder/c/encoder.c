/**
  * User: jon, peter
  * Date: 4/18/16
  * Time: 8:48 AM
  * Copyright English Central Inc. 2016
  */
  /*
FFMpeg Primer

    Files are Containers
    Containers are described with a Format Context
    Containers are read/written to files with IO
    Custom IO is used to read/write from memory 
    Containers have one or more Streams
    Muxers separate/combine multiple Streams in a Container
    Streams are a sequence of Packets
    Packets contain one or more Frames of some Format
    Fifos buffer data so it can be read in Frame size chunks
    Codecs input Frames of one Format, and output Packets of different Format
    Codecs may not produce immediate output
    Resamplers convert on sample rate to another, created with a Resampler Context

Notes
    In general a Foo is created with a FooContext

    Memory should be allocated/freed with av_malloc and av_free
    Free buffers first, then free the Context

This API

    Input is read from memory buffers
    A Fifo is used to compact buffers into Frames

    All entry points return an integer error code
    0 = no error, otherwise get_error_text returns a friendly message

    In this application the input is buffers of PCM Doubles
    The output is a buffer containing an MP4 file with an AAC stream

    To write the output Container to memory instead of a file
    we implement Custom IO for the output FormatContext
    http://www.codeproject.com/Tips/489450/Creating-Custom-FFmpeg-IO-Context
    http://miphol.com/muse/2014/03/custom-io-with-ffmpeg.html
  */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>

#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavcodec/avcodec.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"
#include "libswresample/swresample.h"

#ifdef __FLASHPLAYER__
#include "AS3/AS3.h"
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// max of 30 seconds at 32k bits/sec
const int max_input_length = (32000 / 8) * 30;

// max of 30 seconds at 44100khz
const int max_output_length = 44100 * 4 * 30;

AVAudioFifo *input_fifo;

//int input_sample_rate = 44100;
enum AVSampleFormat input_sample_fmt = AV_SAMPLE_FMT_FLTP;

int output_bit_rate = 100;
int output_channels = 1;
char *output_format = NULL;
int output_length = 0;

AVCodec *output_codec;
AVCodecContext *output_codec_context;   // args specifying Codec
AVFormatContext *output_format_context; // args specifying Output Container

SwrContext *resample_context;

int input_sample_rate;
int output_sample_rate;

int input_frame_size;
uint8_t *input_frame_buffer;

AVFrame *input_frame;
AVFrame *output_frame;

AVIOContext *output_io_context = NULL;
AVStream *output_stream        = NULL;

#define INTERNAL_ERROR 1
#define NO_ERROR 0
#define ERROR_CODE int
#define LOG(x) fprintf(stdout,"%s\n",x)
#define LOG1(x,y) fprintf(stdout,"%s = %d\n",x,y)
#define CHK_NULL(x) { fprintf(stdout,"%s\n",#x); if(!(x)) { fprintf(stderr,"%s FAILED",#x); return; }}
#define CHK_ERROR(x) { fprintf(stdout,"%s\n",#x); int err=(x); if(err<0) { fprintf(stderr,"%s FAILED code=%d %s\n",#x,err,get_error_text(err)); return; }}
#define CHK_GE(x,y) { fprintf(stdout,"%s\n",#x); int err=(x); if(err<y) { fprintf(stderr,"%s FAILED %d < %d\n",#x,err,y); return; }}
#define CHK_VOID(x) { fprintf(stdout,"%s\n",#x); x; }

int main(int argc, char **argv) {
    fprintf(stdout,"%s\n", "main");

    #ifdef __EMSCRIPTEN__
    emscripten_exit_with_live_runtime();
    #endif

    #ifdef __FLASHPLAYER__
    AS3_GoAsync();
    #endif
}

/**
 * Convert an error code into a text message.
 * @param error Error code to be converted
 * @return Corresponding error text (not thread-safe)
 */
static const char *get_error_text(const ERROR_CODE error)
{
    static char error_buffer[255];
    av_strerror(error, error_buffer, sizeof(error_buffer));
    return error_buffer;
}

void init_codec() {
    /** Create a new format context for the output container format. */
    CHK_NULL(output_format_context = avformat_alloc_context());

    /** Get the AAC encoder */
    CHK_NULL(output_codec = avcodec_find_encoder(AV_CODEC_ID_AAC));

    /** Create a new codec context. */
    CHK_NULL(output_codec_context = avcodec_alloc_context3(output_codec));

    /** Check that codec can handle the input */
    CHK_NULL(check_sample_fmt(output_codec, input_sample_fmt));
    CHK_NULL(check_sample_rate(output_codec, input_sample_rate));

    /** Parameters for AAC */
    output_codec_context->sample_fmt     = input_sample_fmt;
    output_codec_context->sample_rate    = input_sample_rate;
    output_codec_context->channels       = output_channels;
    output_codec_context->channel_layout = av_get_default_channel_layout(1);
    //output_codec_context->sample_fmt     = output_codec->sample_fmts[0];
    //output_codec_context->bit_rate       = output_bit_rate;

    /** Open the Codec */
    CHK_ERROR(avcodec_open2(output_codec_context, output_codec, NULL));

    /* Use the encoder's desired frame size for processing. */
    input_frame_size = output_codec_context->frame_size;
    LOG1("input_frame_size",input_frame_size);

    CHK_NULL(input_frame = av_frame_alloc());

    input_frame->nb_samples     = output_codec_context->frame_size;
    input_frame->format         = output_codec_context->sample_fmt;
    input_frame->channel_layout = output_codec_context->channel_layout;

        /* the codec gives us the frame size, in samples,
         * we calculate the size of the samples buffer in bytes */
    CHK_ERROR(input_frame_size = av_samples_get_buffer_size(NULL,
        output_codec_context->channels,
        output_codec_context->frame_size,
        output_codec_context->sample_fmt, 0));

    CHK_NULL(input_frame_buffer = av_malloc(input_frame_size));

        /* setup the data pointers in the AVFrame */
    CHK_ERROR( avcodec_fill_audio_frame(input_frame,
        output_codec_context->channels,
        output_codec_context->sample_fmt,
        (const uint8_t*)input_frame_buffer,
        input_frame_size, 0));

    /** Create the input FIFO buffer based on the Codec input format */
    CHK_NULL(input_fifo = av_audio_fifo_alloc(
        output_codec_context->sample_fmt,
        output_codec_context->channels, 1));
}

void init_container() {

    /** Open the output file to write to it. */
    CHK_ERROR(avio_open_dyn_buf(&output_io_context));

    /** Associate the output file (pointer) with the container format context. */
    output_format_context->pb = output_io_context;

    /** Set the container format to MP4 */
    CHK_NULL(output_format_context->oformat = av_guess_format("mp4", NULL, NULL));

    /** Create a new audio stream in the output file container. */
    CHK_NULL(output_stream = avformat_new_stream(output_format_context, output_codec));
    
    CHK_ERROR(avformat_write_header(output_format_context, NULL));
}


/**
    Set up input to accept samples PCM float 44.1k for now

    Create MP4 AAC output Container to be returned by flush()
*/
void init(const char *i_format, int i_sample_rate, const char *o_format, int o_sample_rate, int o_bit_rate) {

    input_sample_rate = i_sample_rate;
    output_sample_rate = o_sample_rate;

    fprintf(stdout,"init(%s,%d,%s,%d,%d)\n",i_format,input_sample_rate,o_format,output_sample_rate,o_bit_rate);

    /** Register all codecs and formats so that they can be used. */
    av_register_all();

    init_codec();

    init_container();

    /* Write the Header to the output Container */
}

/* check that a given sample format is supported by the encoder */
int check_sample_fmt(AVCodec *codec, enum AVSampleFormat sample_fmt)
{
    fprintf(stdout,"check_sample_fmt(%s)\n",av_get_sample_fmt_name(sample_fmt));
    const enum AVSampleFormat *p = codec->sample_fmts;
    while (*p != AV_SAMPLE_FMT_NONE) {
        fprintf(stdout," available %u %s\n",*p,av_get_sample_fmt_name(*p));

        if (*p == sample_fmt)
            return 1;
        p++;
    }
    return 0;
}

/* check that a given sample format is supported by the encoder */
int check_sample_rate(AVCodec *codec, int sample_rate)
{
    fprintf(stdout,"check_sample_rate(%d)\n",sample_rate);
    const int *p = codec->supported_samplerates;
    while (*p != 0) {
        fprintf(stdout," available %d\n",*p);

        if (*p == sample_rate)
            return 1;
        p++;
    }
    return 0;
}

/**
    Load some more input samples

    We are using the FDK AAC Codec
    http://wiki.hydrogenaud.io/index.php?title=Fraunhofer_FDK_AAC#Sample_Format
    The FDK library is based on fixed-point math and only supports 16-bit integer PCM input.

    These input buffers are varying sizes (not Frame size)
    The samples are converted to 16 bit Integer are put into a Fifo

    When at least a Frame's worth of data is in the Fifo,
    a Frame is read

    Before the first Frame is read,

    A Codec converts the input PCM Frame to a AAC Frame

    The AAC Frame is written to the AAC Stream of the output
*/
void load(uint8_t *i_data, int i_length) {
    LOG1("load i_length", i_length);

    int input_samples_size = i_length / sizeof(float);
    int frame_samples_size = input_frame_size / sizeof(float);

  /**
    * Make the FIFO as large as it needs to be to hold both,
    * the old and the new samples.
    */
    LOG1("  before fifo space = %d\n",av_audio_fifo_space(input_fifo));
    LOG1("    input_samples_size = %d\n",input_samples_size);
    CHK_ERROR(av_audio_fifo_realloc(input_fifo, av_audio_fifo_size(input_fifo) + input_samples_size));
    LOG1("  after fifo space = %d\n",av_audio_fifo_space(input_fifo));

    /** Store the new samples in the FIFO buffer. */
    LOG1("  before fifo size = %d\n",av_audio_fifo_size(input_fifo));
    CHK_GE(av_audio_fifo_write(input_fifo, (void **)&i_data, input_samples_size), input_samples_size);
    LOG1("  after fifo size = %d\n",av_audio_fifo_size(input_fifo));

    int finished               = 0;
    int amount_read            = 0;
    AVPacket *output_packet;
    CHK_NULL(output_packet=av_packet_alloc());
    /**
     * While there is at least one Frame's worth of data in the Fifo,
     * encode the Frame and write it to the output Container
     */
    while (av_audio_fifo_size(input_fifo) >= frame_samples_size) {
        LOG1("  before fifo size",av_audio_fifo_size(input_fifo));
        CHK_ERROR( amount_read = av_audio_fifo_read(input_fifo,(void**)&input_frame_buffer,frame_samples_size));
        LOG1("  read from fifo",amount_read);
        LOG1("  after fifo size",av_audio_fifo_size(input_fifo));

        int got_output = 0;
        CHK_ERROR(avcodec_encode_audio2(output_codec_context, output_packet, input_frame, &got_output));
        LOG1("  got_output",got_output);
        if(got_output) {
            LOG1("    packet size",output_packet->size);
            CHK_VOID(av_packet_unref(output_packet));
        }
    }
}

uint8_t *flush() {
    LOG("flush");

    /** Get all the delayed frames */
    AVPacket *output_packet;
    CHK_NULL(output_packet=av_packet_alloc());
    int got_output = 1;
    while (got_output) {
        CHK_ERROR(avcodec_encode_audio2(output_codec_context, output_packet, NULL, &got_output));
        LOG1("  got_output",got_output);
        if(got_output) {
            LOG1("    packet size",output_packet->size);
            CHK_VOID(av_packet_unref(output_packet));
        }
    }

    CHK_ERROR(av_write_trailer(output_format_context));
}

/**
 * Finish the output Container, and return the contents buffer and length
 */
void dispose(int status) {

    LOG("dispose\n");

    /* If there is a partial Frame left over in the Fifo, process it */

    /* Write the tail to the Container and close it */

    /* Get the Container contents */

    #ifdef __EMSCRIPTEN__
    emscripten_force_exit(status);
    #endif

    exit(status);
}

/**
 * Open an output file and the required encoder.
 * Also set some basic encoder parameters.
 * Some of these parameters are based on the input file's parameters.
 */
static int open_output_file(const char *filename,
                            AVCodecContext *input_codec_context,
                            AVFormatContext **output_format_context,
                            AVCodecContext **output_codec_context)
{
    AVIOContext *output_io_context = NULL;
    AVStream *stream               = NULL;
    AVCodec *output_codec          = NULL;
    int error;
    /** Open the output file to write to it. */
    if ((error = avio_open(&output_io_context, filename,
                           AVIO_FLAG_WRITE)) < 0) {
        fprintf(stderr, "Could not open output file '%s' (error '%s')\n",
                filename, get_error_text(error));
        return error;
    }
    /** Create a new format context for the output container format. */
    if (!(*output_format_context = avformat_alloc_context())) {
        fprintf(stderr, "Could not allocate output format context\n");
        return AVERROR(ENOMEM);
    }
    /** Associate the output file (pointer) with the container format context. */
    (*output_format_context)->pb = output_io_context;
    /** Guess the desired container format based on the file extension. */
    if (!((*output_format_context)->oformat = av_guess_format(NULL, filename,
                                                              NULL))) {
        fprintf(stderr, "Could not find output file format\n");
        goto cleanup;
    }
    av_strlcpy((*output_format_context)->filename, filename,
               sizeof((*output_format_context)->filename));
    /** Find the encoder to be used by its name. */
    if (!(output_codec = avcodec_find_encoder(AV_CODEC_ID_AAC))) {
        fprintf(stderr, "Could not find an AAC encoder.\n");
        goto cleanup;
    }
    /** Create a new audio stream in the output file container. */
    if (!(stream = avformat_new_stream(*output_format_context, output_codec))) {
        fprintf(stderr, "Could not create new stream\n");
        error = AVERROR(ENOMEM);
        goto cleanup;
    }
    /** Save the encoder context for easier access later. */
    *output_codec_context = stream->codec;
    /**
     * Set the basic encoder parameters.
     * The input file's sample rate is used to avoid a sample rate conversion.
     */
    (*output_codec_context)->channels       = output_channels;
    (*output_codec_context)->channel_layout = av_get_default_channel_layout(output_channels);
    (*output_codec_context)->sample_rate    = input_codec_context->sample_rate;
    (*output_codec_context)->sample_fmt     = output_codec->sample_fmts[0];
    (*output_codec_context)->bit_rate       = output_bit_rate;
    /** Allow the use of the experimental AAC encoder */
    (*output_codec_context)->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    /** Set the sample rate for the container. */
    stream->time_base.den = input_codec_context->sample_rate;
    stream->time_base.num = 1;
    /**
     * Some container formats (like MP4) require global headers to be present
     * Mark the encoder so that it behaves accordingly.
     */
    if ((*output_format_context)->oformat->flags & AVFMT_GLOBALHEADER)
        (*output_codec_context)->flags |= CODEC_FLAG_GLOBAL_HEADER;
    /** Open the encoder for the audio stream to use it later. */
    if ((error = avcodec_open2(*output_codec_context, output_codec, NULL)) < 0) {
        fprintf(stderr, "Could not open output codec (error '%s')\n",
                get_error_text(error));
        goto cleanup;
    }
    return 0;
cleanup:
    avio_closep(&(*output_format_context)->pb);
    avformat_free_context(*output_format_context);
    *output_format_context = NULL;
    return error < 0 ? error : AVERROR_EXIT;
}

int get_output_sample_rate() {
    //LOG("get_output_sample_rate (%u)\n", output_sample_rate);
    return 0; //output_sample_rate;
}

char *get_output_format() {
    fprintf(stdout,"get_output_format (%s)\n", output_format);
    return output_format;
}

int get_output_length() {
    LOG1("get_output_length", output_length);
    return output_length;
}












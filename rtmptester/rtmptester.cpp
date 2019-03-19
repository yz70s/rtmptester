// rtmptester.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "caratteri.h"
#define UINT64_C(val) val##ui64
#define INT64_C(val) val##i64
extern "C" {
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libavresample/avresample.h"
}

#define STREAM_FRAME_RATE 25 /* 25 images/s */
#define AUDIO_SAMPLE_RATE 44100

struct {
	char *filename;
	char *url;
	int minutes;
	bool realtime;
} parameters;

struct {
	AVFrame *generated;
	AVFrame *compress;
	SwsContext *context;
} pics;

struct {
	AVOutputFormat *fmt;
	AVFormatContext *oc;
	AVStream *audio_st;
	AVStream *video_st;
	long written_frames_count;
	uint8_t *video_outbuf;
	int video_outbuf_size;
	int audio_input_frame_size;
	int samples_used;
	int16_t *samples[AV_NUM_DATA_POINTERS];
	int samples_size[AV_NUM_DATA_POINTERS];
	uint8_t *samplescodec[AV_NUM_DATA_POINTERS];
	int samplescodec_size[AV_NUM_DATA_POINTERS];
	AVAudioResampleContext *acontext;
	int64_t audio_duration;
	AVFormatContext *oc_rtmp;
} videofile;

void set_default_parameters()
{
	parameters.filename = new char[11];
	strcpy_s(parameters.filename, 11, "output.mp4");
	parameters.url = new char[1];
	parameters.url[0] = 0; // rtmp://...
	parameters.minutes = 1;
	parameters.realtime = false;
}

void debug_list_codecs()
{
	/* Enumerate the codecs*/
	AVCodec * codec = av_codec_next(NULL);
	while (codec != NULL)
	{
		printf("%d %s\n\r", codec->id, codec->long_name);
		codec = av_codec_next(codec);
	}
}

void debug_list_formats()
{
	/* Enumerate the output formats */
	AVOutputFormat * outputf = av_oformat_next(NULL);
	while (outputf != NULL)
	{
		printf("%s\n\r", outputf->long_name);
		outputf = av_oformat_next(outputf);
	}
}

/* Notes to convert from older versions to version 12 of libav
	PixelFormat -> AVPixelFormat
	avcodec_alloc_frame() -> av_frame_alloc()
	PIX_FMT_BGRA -> AV_PIX_FMT_BGRA
	PIX_FMT_YUVJ420P -> AV_PIX_FMT_YUV420P
	CODEC_ID_AAC -> AV_CODEC_ID_AAC
	avcodec_encode_video -> avcodec_encode_video2 -> avcodec_send_frame
	avcodec_encode_audio2 -> avcodec_send_frame
	av_free_packet -> av_packet_unref
*/

int size_picture(AVPixelFormat pix_fmt, int width, int height, int linesize[AV_NUM_DATA_POINTERS])
{
	if (pix_fmt == AV_PIX_FMT_YUYV422)
	{
		linesize[0] = (width * 4) / 2;
		return 1;
	}
	if (pix_fmt == AV_PIX_FMT_YUV420P)
	{
		linesize[0] = width;
		linesize[1] = width / 2;
		linesize[2] = width / 2;
		return 3;
	}
	if (pix_fmt == AV_PIX_FMT_BGRA)
	{
		linesize[0] = width * 4;
		return 1;
	}
	if (pix_fmt == AV_PIX_FMT_RGB24)
	{
		linesize[0] = width * 3;
		return 1;
	}
	if (pix_fmt == AV_PIX_FMT_GRAY8)
	{
		linesize[0] = width;
		return 1;
	}
	return 0;
}

AVFrame *alloc_picture(AVPixelFormat pix_fmt, int width, int height)
{
	AVFrame *picture;

	picture = av_frame_alloc();
	if (!picture)
		return NULL;
	picture->width = width;
	picture->height = height;
	picture->format = pix_fmt;
	if (av_frame_get_buffer(picture, 1) < 0)
		return NULL;
	return picture;
}

AVFrame *alloc_picture_empty(AVPixelFormat pix_fmt, int width, int height)
{
	AVFrame *picture;

	picture = av_frame_alloc();
	if (!picture)
		return NULL;
	picture->width = width;
	picture->height = height;
	picture->format = pix_fmt;
	return picture;
}

void clear_picture(AVFrame *picture)
{
	if (picture->format != AV_PIX_FMT_BGRA)
		return;
	for (int n = 0; n < 8; n++)
	{
		if ((picture->data[n] != NULL) && (picture->linesize[n] != 0))
			memset(picture->data[n], 64, picture->linesize[n] * picture->height);
		else
			break;
	}
}

int packet_rtmp(AVStream *st, AVPacket *pkt)
{
	AVPacket *pktr = av_packet_clone(pkt);
	AVStream *ost;
	int ret;

	ost = videofile.oc_rtmp->streams[st->index];
	pktr->pts = av_rescale_q_rnd(pkt->pts, st->time_base, ost->time_base, AV_ROUND_NEAR_INF);
	pktr->dts = av_rescale_q_rnd(pkt->dts, st->time_base, ost->time_base, AV_ROUND_NEAR_INF);
	pktr->duration = av_rescale_q(pkt->duration, st->time_base, ost->time_base);
	ret = av_interleaved_write_frame(videofile.oc_rtmp, pktr);
	av_packet_unref(pktr);
	return ret;
}

// avconv -re -i file.mp4 -vcodec copy -acodec copy -f flv rtmp://website.com/live2/secet_key
void initialize_rtmp(char *url)
{
	AVOutputFormat *oformat;
	AVFormatContext *ofmt_ctx = avformat_alloc_context();

	oformat = av_guess_format("flv", NULL, NULL);
	if (!oformat)
	{
		printf("Output format flv is not a suitable output format\n\r");
		exit(1);
	}
	ofmt_ctx->oformat = oformat;
	if (url)
		strncpy_s(ofmt_ctx->filename, sizeof(ofmt_ctx->filename), url, sizeof(ofmt_ctx->filename));

	AVFormatContext *ifmt_ctx = videofile.oc;

	for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
		//Create output AVStream according to input AVStream
		AVStream *in_stream = ifmt_ctx->streams[i];
		AVStream *out_stream = avformat_new_stream(ofmt_ctx, in_stream->codec->codec);
		if (!out_stream) {
			printf("Failed allocating output stream\n\r");
			exit(1);
		}
		//Copy the settings of AVCodecContext
		if (avcodec_copy_context(out_stream->codec, in_stream->codec) < 0) {
			printf("Failed to copy context from input to output stream codec context\n");
			exit(1);
		}
		out_stream->time_base.den = out_stream->codec->time_base.den;
		out_stream->time_base.num = out_stream->codec->time_base.num;
		out_stream->codec->codec_tag = 0;
		if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
			out_stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
	}

	//Dump Format
	av_dump_format(ofmt_ctx, 0, url, 1);
	//Open output URL
	if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&ofmt_ctx->pb, url, AVIO_FLAG_WRITE) < 0) {
			printf("Could not open output URL '%s'", url);
			exit(1);
		}
	}
	if (avformat_write_header(ofmt_ctx, NULL) < 0)
	{
		printf("Error occurred when opening output URL\n");
		exit(1);
	}
	videofile.oc_rtmp = ofmt_ctx;
}

void deinitialize_rtmp()
{
	if (videofile.oc_rtmp)
	{
		av_write_trailer(videofile.oc_rtmp);

		for (int i = 0; i < videofile.oc_rtmp->nb_streams; i++) {
			av_freep(&videofile.oc_rtmp->streams[i]);
		}

		if (!(videofile.oc_rtmp->oformat->flags & AVFMT_NOFILE)) {
			avio_close(videofile.oc_rtmp->pb);
		}
		av_free(videofile.oc);
	}
}

void write_audio_frame(AVFormatContext *oc, AVStream *st, void *asamples, int count)
{
	AVCodecContext *c;
	AVPacket pkt = { 0 }; // data and size must be 0;
	AVFrame *frame = av_frame_alloc();
	uint16_t *audsamples;
	int num;
	int ret;

	audsamples = (uint16_t *)asamples;
	av_init_packet(&pkt);
	c = st->codec;

	while (count > 0) {
		// fill samples buffer until it has audio_input_frame_size samples
		while ((videofile.samples_used < videofile.audio_input_frame_size) && (count > 0)) {
			videofile.samples[0][videofile.samples_used] = *audsamples;
			videofile.samples_used++;
			audsamples++;
			count--;
		}
		//get_audio_frame(samples, audio_input_frame_size, c->channels);
		if (videofile.samples_used == videofile.audio_input_frame_size) {
			// convert the samples
			num = avresample_convert(videofile.acontext, videofile.samplescodec, 0, videofile.audio_input_frame_size, (uint8_t **)videofile.samples, 0, videofile.audio_input_frame_size);

			videofile.samples_used = 0;
			frame->nb_samples = videofile.audio_input_frame_size;
			avcodec_fill_audio_frame(frame, c->channels, c->sample_fmt,
				videofile.samplescodec[0],
				videofile.audio_input_frame_size *
				av_get_bytes_per_sample(c->sample_fmt) *
				c->channels, 1);

			ret = avcodec_send_frame(c, frame);
			if (ret >= 0)
			{
				ret = avcodec_receive_packet(c, &pkt);
				if (ret == 0)
				{
					pkt.stream_index = st->index;

					if (pkt.pts != AV_NOPTS_VALUE)
						pkt.pts = av_rescale_q(pkt.pts, st->codec->time_base, st->time_base);
					else
						pkt.pts = av_rescale_q(videofile.audio_duration, st->codec->time_base, st->time_base);
					pkt.dts = pkt.pts;
					if (pkt.duration > 0)
					{
						videofile.audio_duration += pkt.duration;
						pkt.duration = (int)av_rescale_q(pkt.duration, st->codec->time_base, st->time_base);
					}

					if (videofile.oc_rtmp)
						packet_rtmp(st, &pkt);
					/* Write the compressed frame to the media file. */
					if (av_interleaved_write_frame(oc, &pkt) != 0) {
						fprintf(stderr, "Error while writing audio frame\n");
						exit(1);
					}
					av_packet_unref(&pkt);
				}
			}
		}
	}
}

void write_video_frame(AVFormatContext *oc, AVStream *st)
{
	int ret;
	AVCodecContext *c;

	c = st->codec;

	sws_scale(pics.context, pics.generated->data, pics.generated->linesize,
		0, c->height, pics.compress->data, pics.compress->linesize);

	if (oc->oformat->flags & AVFMT_RAWPICTURE) {
		/* Raw video case - the API will change slightly in the near
		* future for that. */
		AVPacket pkt;
		av_init_packet(&pkt);

		pkt.flags |= AV_PKT_FLAG_KEY;
		pkt.stream_index = st->index;
		pkt.data = (uint8_t *)pics.compress;
		pkt.size = sizeof(AVPicture);

		ret = av_interleaved_write_frame(oc, &pkt);
	}
	else {
		AVPacket pkt = { 0 };

		// in a second there are fps frames and dsss tics
		// pts=frame_number*tics/fps -> tics=1/rate
		pics.compress->pts = videofile.written_frames_count * c->time_base.den / STREAM_FRAME_RATE;
		/* encode the image */
		ret = avcodec_send_frame(c, pics.compress);
		if (ret >= 0)
		{
			ret = avcodec_receive_packet(c, &pkt);
			if (ret == 0)
			{
				pkt.stream_index = st->index;

				if (videofile.oc_rtmp)
					packet_rtmp(st, &pkt);
				/* Write the compressed frame to the media file. */
				ret = av_interleaved_write_frame(oc, &pkt);
				av_packet_unref(&pkt);
			}
			else if (ret == AVERROR(EAGAIN))
				ret = 0;
		}
	}
	if (ret != 0) {
		fprintf(stderr, "Error while writing video frame\n");
		exit(1);
	}
	videofile.written_frames_count++;
}

void open_audio(AVFormatContext *oc, AVStream *st)
{
	AVCodecContext *c;
	int err;

	c = st->codec;

	/* open it */
	if (avcodec_open2(c, NULL, NULL) < 0) {
		fprintf(stderr, "could not open codec\n");
		exit(1);
	}

	st->time_base.den = c->time_base.den;
	st->time_base.num = c->time_base.num;
	if (c->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE)
		videofile.audio_input_frame_size = 10000;
	else
		videofile.audio_input_frame_size = c->frame_size;
	av_samples_alloc((uint8_t **)videofile.samples, videofile.samples_size, c->channels, videofile.audio_input_frame_size, AV_SAMPLE_FMT_S16, 0);
	av_samples_alloc(videofile.samplescodec, videofile.samplescodec_size, c->channels, videofile.audio_input_frame_size, c->sample_fmt, 0);
	videofile.samples_used = 0;
	videofile.audio_duration = 0;

	/* copy the stream parameters to the muxer */
	err = avcodec_parameters_from_context(st->codecpar, c);
	if (err < 0) {
		fprintf(stderr, "Could not copy the stream parameters\n");
		exit(1);
	}
}

static void open_video(AVFormatContext *oc, AVStream *st)
{
	AVCodecContext *c;
	int err;

	c = st->codec;

	/* open the codec */
	if ((err = avcodec_open2(c, NULL, NULL)) < 0) {
		fprintf(stderr, "could not open codec\n");
		exit(1);
	}

	videofile.video_outbuf = NULL;
	if (!(oc->oformat->flags & AVFMT_RAWPICTURE)) {
		/* Allocate output buffer. */
		/* XXX: API change will be done. */
		/* Buffers passed into lav* can be allocated any way you prefer,
		* as long as they're aligned enough for the architecture, and
		* they're freed appropriately (such as using av_free for buffers
		* allocated with av_malloc). */
		videofile.video_outbuf_size = 200000;
		videofile.video_outbuf = (uint8_t *)av_malloc(videofile.video_outbuf_size);
	}

	/* copy the stream parameters to the muxer */
	err = avcodec_parameters_from_context(st->codecpar, c);
	if (err < 0) {
		fprintf(stderr, "Could not copy the stream parameters\n");
		exit(1);
	}
}

AVStream *add_audio_stream(AVFormatContext *oc, AVCodecID codec_id)
{
	AVCodecContext *c;
	AVStream *st;
	AVCodec *codec;

	/* find the audio encoder */
	codec = avcodec_find_encoder(codec_id);
	if (!codec) {
		fprintf(stderr, "codec not found\n");
		exit(1);
	}

	st = avformat_new_stream(oc, codec);
	if (!st) {
		fprintf(stderr, "Could not alloc stream\n");
		exit(1);
	}

	c = st->codec;

	/* put sample parameters */
	/* AAC supports only AV_SAMPLE_FMT_FLTP */
	c->sample_fmt = codec->sample_fmts[0];
	for (int n = 0; codec->sample_fmts[n] != -1; n++)
	{
		if (codec->sample_fmts[n] == AV_SAMPLE_FMT_S16)
		{
			c->sample_fmt = AV_SAMPLE_FMT_S16;
			break;
		}
	}
	c->bit_rate = 64000;
	c->sample_rate = AUDIO_SAMPLE_RATE;
	c->channels = 1;
	c->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

	// some formats want stream headers to be separate
	if (oc->oformat->flags & AVFMT_GLOBALHEADER)
		c->flags |= CODEC_FLAG_GLOBAL_HEADER;

	videofile.acontext = avresample_alloc_context();

	if (videofile.acontext)
	{
		av_opt_set_int(videofile.acontext, "in_channel_layout", AV_CH_LAYOUT_MONO, 0);
		av_opt_set_int(videofile.acontext, "out_channel_layout", AV_CH_LAYOUT_MONO, 0);
		av_opt_set_int(videofile.acontext, "in_sample_rate", AUDIO_SAMPLE_RATE, 0);
		av_opt_set_int(videofile.acontext, "out_sample_rate", AUDIO_SAMPLE_RATE, 0);
		av_opt_set_int(videofile.acontext, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
		av_opt_set_int(videofile.acontext, "out_sample_fmt", c->sample_fmt, 0);

		if (avresample_open(videofile.acontext) < 0)
		{
			avresample_free(&videofile.acontext);
			videofile.acontext = nullptr;
		}
	}

	return st;
}

AVStream *add_video_stream(AVFormatContext *oc, AVCodecID codec_id, int width, int height)
{
	AVCodecContext *c;
	AVStream *st;
	AVCodec *codec;

	/* find the video encoder */
	codec = avcodec_find_encoder(codec_id);
	if (!codec) {
		fprintf(stderr, "codec not found\n");
		exit(1);
	}

	st = avformat_new_stream(oc, codec);
	if (!st) {
		fprintf(stderr, "Could not alloc stream\n");
		exit(1);
	}

	c = st->codec;
	c->codec_id = codec->id;

	/* Put sample parameters. */
	c->bit_rate = 1000000;
	/* Resolution must be a multiple of two. */
	c->width = width;
	c->height = height;
	/* timebase: This is the fundamental unit of time (in seconds) in terms
	* of which frame timestamps are represented. For fixed-fps content,
	* timebase should be 1/framerate and timestamp increments should be
	* identical to 1. */
	c->time_base.den = STREAM_FRAME_RATE;
	c->time_base.num = 1;
	st->time_base.den = c->time_base.den;
	st->time_base.num = c->time_base.num;
	c->gop_size = 12; /* emit one intra frame every twelve frames at most */
	c->pix_fmt = AV_PIX_FMT_YUV420P;
	/* just for testing, disable B frames */
	c->max_b_frames = 0;
	//if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
		/* Needed to avoid using macroblocks in which some coeffs overflow.
		* This does not happen with normal video, it just happens here as
		* the motion of the chroma plane does not match the luma plane. */
	//	c->mb_decision = 2;
	//}
	/* Some formats want stream headers to be separate. */
	if (oc->oformat->flags & AVFMT_GLOBALHEADER)
		c->flags |= CODEC_FLAG_GLOBAL_HEADER;
	c->qmax = 50;
	c->qmin = 1;
	return st;
}

void flush_audio_codec(AVFormatContext *oc, AVStream *st)
{
	AVCodecContext *c;
	int ret;

	c = st->codec;

	ret = avcodec_send_frame(c, nullptr);
	while (true)
	{
		AVPacket pkt = { 0 };

		av_init_packet(&pkt);
		ret = avcodec_receive_packet(c, &pkt);
		if (ret == 0)
		{
			pkt.stream_index = st->index;
			if (pkt.pts != AV_NOPTS_VALUE)
				pkt.pts = av_rescale_q(pkt.pts, st->codec->time_base, st->time_base);
			else
				pkt.pts = av_rescale_q(videofile.audio_duration, st->codec->time_base, st->time_base);
			pkt.dts = pkt.pts;
			if (pkt.duration > 0)
			{
				videofile.audio_duration += pkt.duration;
				pkt.duration = (int)av_rescale_q(pkt.duration, st->codec->time_base, st->time_base);
			}
			ret = av_interleaved_write_frame(oc, &pkt);
			av_packet_unref(&pkt);
			if (ret != 0)
				break;
		}
		else
			break;
	}
}

void flush_video_codec(AVFormatContext *oc, AVStream *st)
{
	int ret;
	AVCodecContext *c;

	c = st->codec;

	ret = avcodec_send_frame(c, nullptr);
	while (true)
	{
		AVPacket pkt = { 0 };

		ret = avcodec_receive_packet(c, &pkt);
		if (ret == 0)
		{
			pkt.stream_index = st->index;
			ret = av_interleaved_write_frame(oc, &pkt);
			av_packet_unref(&pkt);
			if (ret != 0)
				break;
		}
		else 
			break;
	}
}

void close_audio(AVFormatContext *oc, AVStream *st)
{
	if (videofile.acontext)
	{
		avresample_close(videofile.acontext);
		avresample_free(&videofile.acontext);
	}
	avcodec_close(st->codec);
	av_freep(videofile.samples);
	av_freep(videofile.samplescodec);
}

void close_video(AVFormatContext *oc, AVStream *st)
{
	avcodec_close(st->codec);
	av_free(videofile.video_outbuf);
}

int open_video_file(char *filename, int width, int height)
{

	/* Autodetect the output format from the name. default is MPEG. */
	videofile.fmt = av_guess_format(NULL, filename, NULL);
	if (!videofile.fmt) {
		videofile.fmt = av_guess_format("mpeg", NULL, NULL);
	}
	if (!videofile.fmt) {
		fprintf(stderr, "Could not find suitable output format\n");
		return 1;
	}

	/* Allocate the output media context. */
	videofile.oc = avformat_alloc_context();
	if (!videofile.oc) {
		//fprintf(stderr, "Memory error\n");
		return 1;
	}
	videofile.oc->oformat = videofile.fmt;
	//sprintf(videofile.oc->filename, "%s", filename);
	strncpy_s(videofile.oc->filename, sizeof(videofile.oc->filename), filename, sizeof(videofile.oc->filename));

	/* Add the audio and video streams using the default format codecs
	* and initialize the codecs. */
	videofile.video_st = NULL;
	videofile.audio_st = NULL;
	if (videofile.fmt->video_codec != AV_CODEC_ID_NONE) {
		videofile.video_st = add_video_stream(videofile.oc, videofile.fmt->video_codec, width, height);
	}
	if (videofile.fmt->audio_codec != AV_CODEC_ID_NONE) {
		videofile.audio_st = add_audio_stream(videofile.oc, videofile.fmt->audio_codec);
	}

	/* Now that all the parameters are set, we can open the audio and
	* video codecs and allocate the necessary encode buffers. */
	if (videofile.video_st)
		open_video(videofile.oc, videofile.video_st);
	if (videofile.audio_st)
		open_audio(videofile.oc, videofile.audio_st);
	videofile.written_frames_count = 0;

	av_dump_format(videofile.oc, 0, filename, 1);

	/* open the output file, if needed */
	if (!(videofile.fmt->flags & AVFMT_NOFILE)) {
		if (avio_open(&videofile.oc->pb, filename, AVIO_FLAG_WRITE) < 0) {
			return 1;
		}
	}

	/* Write the stream header, if any. */
	avformat_write_header(videofile.oc, NULL);
	return 0;
}

int close_video_file()
{
	unsigned int i;

	/* Flush any codec data not written to file */
	if (videofile.video_st)
	{
		flush_video_codec(videofile.oc, videofile.video_st);
	}
	if (videofile.audio_st)
	{
		flush_audio_codec(videofile.oc, videofile.audio_st);
	}

	/* Write the trailer, if any. The trailer must be written before you
	* close the CodecContexts open when you wrote the header; otherwise
	* av_write_trailer() may try to use memory that was freed on
	* av_codec_close(). */
	av_write_trailer(videofile.oc);

	/* Close each codec. */
	if (videofile.video_st)
		close_video(videofile.oc, videofile.video_st);
	if (videofile.audio_st)
		close_audio(videofile.oc, videofile.audio_st);

	/* Free the streams. */
	for (i = 0; i < videofile.oc->nb_streams; i++) {
		av_freep(&videofile.oc->streams[i]->codec);
		av_freep(&videofile.oc->streams[i]);
	}

	if (!(videofile.fmt->flags & AVFMT_NOFILE))
		/* Close the output file. */
		avio_close(videofile.oc->pb);

	/* free the stream */
	av_free(videofile.oc);
	return 0;
}

int main(int argc, char *argv[])
{
	LARGE_INTEGER performance_frequency;
	LARGE_INTEGER start_time;
	LARGE_INTEGER end_time;
	int timems;
	int n;
	uint16_t *audio_silent;

	set_default_parameters();
	if (argc > 0)
	{
		for (int n = 1; n < argc; n++)
		{
			if (strcmp(argv[n], "-h") == 0)
			{
				printf("Usage:\n\r");
				printf("%s [OPTIONS...]\n\r", argv[0]);
				printf("Options:\n\r");
				printf("\t-h\t\t this help\n\r");
				printf("\t-f filename\t set mp4 output file name\n\r");
				printf("\t-s url\t\t set rtmp server url\n\r");
				printf("\t-m minutes\t set output duration in minutes\n\r");
				printf("\t-r\t\t work in real time\n\r");
				exit(0);
			}
			else if (strcmp(argv[n], "-f") == 0)
			{
				n++;
				if (n < argc)
				{
					parameters.filename = argv[n];
				}
			}
			else if (strcmp(argv[n], "-s") == 0)
			{
				n++;
				if (n < argc)
				{
					int l = strlen(argv[n]);

					if (l > 0)
					{
						if (argv[n][l - 1] == '/')
						{
							int l2;
							char *buf;
							char *tmp = new char[128];

							printf("Please enter secret key: ");
							gets_s(tmp, 127);
							l2 = strlen(tmp);
							if (l2 > 0)
							{
								buf = new char[l+l2+1];
								strcpy(buf, argv[n]);
								strcat(buf, tmp);
								parameters.url = buf;
								delete[] tmp;
							}
						}
						else
							parameters.url = argv[n];
					}
				}
			}
			else if (strcmp(argv[n], "-m") == 0)
			{
				n++;
				if (n < argc)
				{
					parameters.minutes = atoi(argv[n]);
				}
			}
			else if (strcmp(argv[n], "-r") == 0)
			{
				parameters.realtime = true;
			}
			else
			{
				printf("Unknown option %s\n\r", argv[n]);
				exit(0);
			}
		}
	}
	QueryPerformanceFrequency(&performance_frequency);
	/* Initialize libavcodec, and register all codecs and formats. */
	av_register_all();
	avformat_network_init();
#ifdef _DEBUG
	debug_list_codecs();
	debug_list_formats();
#endif
	pics.generated = alloc_picture(AV_PIX_FMT_BGRA, 1280, 720);
	pics.compress = alloc_picture(AV_PIX_FMT_YUV420P, 1280, 720);
	pics.context = sws_getContext(pics.generated->width, pics.generated->height, AV_PIX_FMT_BGRA, pics.compress->width, pics.compress->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
	if (open_video_file(parameters.filename, 1280, 720) == 0)
	{
		if (parameters.url[0] != 0)
			initialize_rtmp(parameters.url);
		audio_silent = new uint16_t[AUDIO_SAMPLE_RATE / STREAM_FRAME_RATE];
		memset(audio_silent, 0, 2 * AUDIO_SAMPLE_RATE / STREAM_FRAME_RATE);
		QueryPerformanceCounter(&start_time);
		for (n = 0; n < parameters.minutes * 60 * STREAM_FRAME_RATE; n++)
		{
			LARGE_INTEGER current_time;
			int x;

			clear_picture(pics.generated);
			x = (1280 - value_width(n)) / 2;
			value_draw_argb(pics.generated->data[0], x, 253, pics.generated->linesize[0], n);
			write_video_frame(videofile.oc, videofile.video_st);
			write_audio_frame(videofile.oc, videofile.audio_st, audio_silent, AUDIO_SAMPLE_RATE / STREAM_FRAME_RATE);
			if (parameters.realtime)
			{
				QueryPerformanceCounter(&current_time);
				timems = (int)(((current_time.QuadPart - start_time.QuadPart) * (LONGLONG)1000) / performance_frequency.QuadPart);
				timems = ((n + 1) * 1000) / STREAM_FRAME_RATE - timems;
				if (timems > 0)
					Sleep(timems);
			}
		}
		QueryPerformanceCounter(&end_time);
		close_video_file();
		deinitialize_rtmp();
		timems = (int)(((end_time.QuadPart - start_time.QuadPart) * (LONGLONG)1000) / performance_frequency.QuadPart);
		printf("Generating %d frames took %d msec (%d sec)\n\r", n, timems, timems / 1000);
	}
	return 0;
}

// rtmptester.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
//#include "Strmif.h"
//#include "Codecapi.h"
#define UINT64_C(val) val##ui64
#define INT64_C(val) val##i64
extern "C" {
#include "libavutil/mathematics.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
//#include "libswscale/swscale.h"
}

struct {
	char *filename;
	char *url;
	int minutes;
} parameters;

struct {
	AVFrame *generated;
	AVFrame *compress;
} pics;

void set_default_parameters()
{
	parameters.filename = new char[11];
	strcpy_s(parameters.filename, 11, "output.mp4");
	parameters.url = new char[8];
	strcpy_s(parameters.url, 8, "rtmp://");
	parameters.minutes = 1;
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

/* Notes to convert from older versions to version 12
	PixelFormat -> AVPixelFormat
	avcodec_alloc_frame() -> av_frame_alloc()
	PIX_FMT_BGRA -> AV_PIX_FMT_BGRA
	PIX_FMT_YUVJ420P -> AV_PIX_FMT_YUVJ420P
*/

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
	for (int n = 0; n < 8; n++)
	{
		if ((picture->data[n] != NULL) && (picture->linesize[n] != 0))
			memset(picture->data[n], 0, picture->linesize[n] * picture->height);
		else
			break;
	}
}

int main(int argc, char *argv[])
{
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
					parameters.url = argv[n];
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
			else
			{
				printf("Unknown option %s\n\r", argv[n]);
				exit(0);
			}
		}
	}
	/* Initialize libavcodec, and register all codecs and formats. */
	av_register_all();
#ifdef _DEBUG
	debug_list_codecs();
	debug_list_formats();
#endif
	pics.generated = alloc_picture(AV_PIX_FMT_BGRA, 1280, 720);
	pics.compress = alloc_picture(AV_PIX_FMT_YUVJ420P, 1280, 720);
	return 0;
}

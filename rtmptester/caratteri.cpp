#include "stdafx.h"
#include "caratteri_120.h"

int string_width(char *data)
{
	int size;

	size = 0;
	while (*data != 0)
	{
		int c = *data - '0';

		if ((c < 0) || (c > 9))
			return size;
		size = size + digit_xy[c * 2];
		data++;
	}
	return size;
}

int value_width(unsigned int value)
{
	char buffer[32];

	_itoa_s(value, buffer, 31, 10);
	return string_width(buffer);
}

void string_draw_argb(void *buffer, int position_x, int position_y, int pitch, char *data)
{
	unsigned char *buff = (unsigned char *)buffer;

	buff = buff + position_y * pitch;
	buff = buff + position_x * 4;
	while (*data != 0)
	{
		int c = *data - '0';

		if ((c < 0) || (c > 9))
			return;

		unsigned char *pw = buff;
		unsigned char *pr = digits[c];
		int dx = digit_xy[c * 2];
		int dy = digit_xy[c * 2 + 1];

		for (int y = 0; y < dy; y++)
		{
			for (int x = 0; x < dx; x++)
			{
				unsigned int pix;

				pix = *pr;
				pix = pix << 8;
				pr++;
				pix = pix + *pr;
				pix = pix << 8;
				pr++;
				pix = pix + *pr;
				pr++;
				*(unsigned int *)pw = pix;
				pw = pw + 4;
			}
			pw = pw + pitch - dx * 4;
		}
		buff = buff + dx * 4;
		data++;
	}
}

void value_draw_argb(void *buffer, int position_x, int position_y, int pitch, unsigned int value)
{
	char data[32];

	_itoa_s(value, data, 31, 10);
	string_draw_argb(buffer, position_x, position_y, pitch, data);
}

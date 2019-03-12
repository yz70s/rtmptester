#pragma once

int string_width(char *data);
int value_width(unsigned int value);
void string_draw_argb(void *buffer, int position_x, int position_y, int pitch, char *data);
void value_draw_argb(void *buffer, int position_x, int position_y, int pitch, unsigned int value);

#pragma once

#include <efi/fbc.h>

void console_init(const FrameBufferConfig &fbc);
void console_write(const char *str);
/* Hand the scanout to a user-space console host: kernel log output continues
   on serial, but stops drawing into the framebuffer. */
void console_suspend_fb();

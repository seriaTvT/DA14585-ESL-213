/*
 * render.c - run a DSL script through the real firmware renderer, on the host.
 *
 *   make render
 *   printf "ROTATE(3)\nCLEAR(1)\nFONT(4,4,0,0,0,1,2,'HI')\n" \
 *       | ./render 852109500 > fb.bin
 *
 * Takes the script on stdin and the clock (seconds since 2000-01-01) as argv[1],
 * and writes the resulting EPD_BUF_SIZE-byte framebuffer to stdout - the same
 * bytes the tag would hold, and the same bytes the image characteristic takes.
 *
 * The point is to have a third opinion. When the panel disagrees with the web
 * preview there are three candidates - the firmware, the JS port in webui, and
 * the test rig reading the tag over SWD - and comparing two of them cannot say
 * which. This runs the actual epd_cmdparser.c and epd_gfx.c with no hardware in
 * the way, so a hardware dump that matches this is proof the firmware is fine
 * and the rig is not. (That is not hypothetical: a stale symbol address from a
 * previous build once made a correct render look like a rendering bug.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "epd_cmdparser.h"
#include "epd_gfx.h"
#include "epd_time.h"

int main(int argc, char **argv)
{
    static char script[4096];

    if (argc < 2) {
        fprintf(stderr, "usage: %s <seconds-since-2000> < script > fb.bin\n",
                argv[0]);
        return 2;
    }

    size_t n = fread(script, 1, sizeof script - 1, stdin);
    script[n] = '\0';

    epd_time_set((uint32_t)strtoul(argv[1], NULL, 10));

    /* Same path a BLE client's bytes take: begin a batch, feed the script in,
     * then run it - so quoting, line splitting and {} expansion are all
     * exercised exactly as they are on the tag. */
    epd_cmd_begin_batch();
    epd_cmd_feed((const uint8_t *)script, (uint16_t)n);
    epd_cmd_run();

    if (fwrite(epd_framebuffer, 1, EPD_BUF_SIZE, stdout) != EPD_BUF_SIZE) {
        perror("write");
        return 1;
    }
    return 0;
}

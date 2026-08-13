/*
 * render.c - run a DSL script through the real firmware renderer, on the host.
 *
 *   make render
 *   printf "ROTATE(3)\nCLEAR(1)\nFONT(4,4,0,0,0,1,2,'HI')\n" \
 *       | ./render 852109500 > fb.bin
 *   ./render 852109500 --panel low < face.txt > fb.bin    # the 104x212 part
 *
 * Takes the script on stdin and the clock (seconds since 2000-01-01) as argv[1],
 * and writes the resulting framebuffer to stdout - the same bytes the tag would
 * hold, and the same bytes the image characteristic takes. Its size follows the
 * panel: 4000 bytes at 122x250, 2756 at 104x212.
 *
 * ONE BINARY FOR BOTH PANELS, chosen with --panel, because that is what the
 * firmware does. There used to be a second executable, `render-low`, built with
 * -DEPD_PANEL_LOW_RES; that macro went when one image started reading its
 * geometry off the tag at boot, and this tool was left not compiling at all for
 * a while afterwards. Geometry here is what it is in the firmware now: four
 * ordinary variables, set once before anything draws.
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
#include "epd_gfx.h"      /* pulls in epd_ssd1680.h and <stdint.h> */
#include "epd_time.h"

/* The geometry lives in epd_ssd1680.c, which this tool does not link - it wants
 * the parser and the drawing code, not a panel driver. Defined here instead,
 * the same way test_gfx.c does it, and set from --panel below before anything
 * draws. Nothing reads them until the first command runs, so plain assignment
 * in main() is enough; the firmware's own epd_geometry_init() does the same job
 * from the board record. */
uint16_t epd_width;
uint16_t epd_height;
uint16_t epd_width_bytes;
uint16_t epd_buf_size;

static void set_panel(uint16_t w, uint16_t h)
{
    epd_width       = w;
    epd_height      = h;
    epd_width_bytes = (uint16_t)((w + 7) / 8);
    epd_buf_size    = (uint16_t)(((w + 7) / 8) * h);
}

int main(int argc, char **argv)
{
    static char script[4096];
    int want_status = (argc > 2 && strcmp(argv[2], "--status") == 0);
    int want_every  = (argc > 2 && strcmp(argv[2], "--every") == 0);

    /* --temp <c> supplies what the panel's sensor would have reported, so a
     * face using {T} can be rendered here and compared against the JS
     * preview. Without it {T} stays an unknown name and renders literally,
     * which is what the firmware does before anything has called
     * epd_cmd_set_temp() - and what the JS does with no temperature. The two
     * only agree if both are given the same value or neither is. */
    /* --batt <pct> <mv> does the same for {BAT} and {VCC}. One option taking
     * both because one call supplies both - see epd_cmd_set_batt(). */
    /* --panel <high|low> picks the geometry, defaulting to the 122x250 part.
     * The names match webui/epd.js's PANELS keys so the byte-identity test can
     * pass its own key straight through. Applied before anything draws. */
    set_panel(122, 250);

    int i;
    for (i = 2; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--temp") == 0) {
            epd_cmd_set_temp((int8_t)strtol(argv[i + 1], NULL, 10));
        } else if (strcmp(argv[i], "--batt") == 0 && i + 2 < argc) {
            epd_cmd_set_batt((uint8_t)strtoul(argv[i + 1], NULL, 10),
                             (uint16_t)strtoul(argv[i + 2], NULL, 10));
        } else if (strcmp(argv[i], "--panel") == 0) {
            if (strcmp(argv[i + 1], "high") == 0) {
                set_panel(122, 250);
            } else if (strcmp(argv[i + 1], "low") == 0) {
                set_panel(104, 212);
            } else {
                fprintf(stderr, "render: --panel wants high or low, got '%s'\n",
                        argv[i + 1]);
                return 2;
            }
        }
    }

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <seconds-since-2000> [--status|--every]"
                " [--panel <high|low>]"
                " [--temp <celsius>] [--batt <pct> <mv>] < script > fb.bin\n",
                argv[0]);
        return 2;
    }

    size_t n = fread(script, 1, sizeof script - 1, stdin);
    script[n] = '\0';

    epd_time_set((uint32_t)strtoul(argv[1], NULL, 10));

    /* A line of exactly "%%" separates scripts to be run one after another in
     * this same process, which is the only way to test that nothing leaks
     * between runs. The tag runs script after script without restarting - a
     * setting left standing from a previous face is invisible on a rig that
     * renders once and exits, and EVERY() is exactly such a setting.
     *
     * Ordinary single-script input contains no such line and is unaffected. */
    char *part = script;
    for (;;) {
        char *sep = strstr(part, "\n%%\n");
        size_t len = sep ? (size_t)(sep - part) + 1 : strlen(part);

        /* Same path a BLE client's bytes take: begin a batch, feed the script
         * in, then run it - so quoting, line splitting and {} expansion are
         * all exercised exactly as they are on the tag. */
        epd_cmd_begin_batch();
        epd_cmd_feed((const uint8_t *)part, (uint16_t)len);
        epd_cmd_run();

        if (!sep) {
            break;
        }
        part = sep + 4;
    }

    /* --status prints the render report instead of the framebuffer, so the
     * error codes the tag serves over the status characteristic can be checked
     * without a tag. */
    if (want_status) {
        uint8_t st[EPD_STATUS_LEN];
        epd_cmd_status(st);
        for (int i = 0; i < EPD_STATUS_LEN; i++) {
            printf("%s%u", i ? " " : "", st[i]);
        }
        printf("\n");
        return 0;
    }

    /* --every prints the repaint interval the script asked for. Not part of
     * the status report: that describes what went wrong with a render, and
     * this is a setting that worked. */
    if (want_every) {
        printf("%u\n", epd_cmd_every_min());
        return 0;
    }

    /* epd_buf_size, not a compile-time constant: the framebuffer is as big as
     * the selected panel needs, exactly as on the tag. This line said
     * EPD_BUF_SIZE, which stopped existing when the geometry became runtime,
     * and the tool quietly failed to build for a while - see the Makefile. */
    if (fwrite(epd_framebuffer, 1, epd_buf_size, stdout) != epd_buf_size) {
        perror("write");
        return 1;
    }
    return 0;
}

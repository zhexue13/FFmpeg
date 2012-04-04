/*
 * Copyright (C) 2012 Michael Niedermayer <michaelni@gmx.at>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/cpu.h"
#include "libavutil/common.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"

#undef NDEBUG
#include <assert.h>

typedef struct {
    float interlace_threshold;
    float progressive_threshold;

    AVFilterBufferRef *cur;
    AVFilterBufferRef *next;
    AVFilterBufferRef *prev;
    AVFilterBufferRef *out;
    int (*filter_line)(uint8_t *prev, uint8_t *cur, uint8_t *next, int w);

    const AVPixFmtDescriptor *csp;
} IDETContext;


static int filter_line_c(const uint8_t *a, const uint8_t *b, const uint8_t *c, int w)
{
    int x;
    int ret=0;

    for(x=0; x<w; x++){
        ret += FFABS((*a++ + *c++) - 2 * *b++);
    }

    return ret;
}

static int filter_line_c_16bit(const uint16_t *a, const uint16_t *b, const uint16_t *c, int w)
{
    int x;
    int ret=0;

    for(x=0; x<w; x++){
        ret += FFABS((*a++ + *c++) - 2 * *b++);
    }

    return ret;
}

static void filter(AVFilterContext *ctx)
{
    IDETContext *idet = ctx->priv;
    int y, i;
    int64_t alpha[2]={0};
    int64_t delta=0;
    static int p=0, t=0, b=0, u=0;

    for (i = 0; i < idet->csp->nb_components; i++) {
        int w = idet->cur->video->w;
        int h = idet->cur->video->h;
        int refs = idet->cur->linesize[i];
        int df = (idet->csp->comp[i].depth_minus1 + 8) / 8;

        if (i && i<3) {
            w >>= idet->csp->log2_chroma_w;
            h >>= idet->csp->log2_chroma_h;
        }

        for (y = 2; y < h - 2; y++) {
            uint8_t *prev = &idet->prev->data[i][y*refs];
            uint8_t *cur  = &idet->cur ->data[i][y*refs];
            uint8_t *next = &idet->next->data[i][y*refs];
            alpha[ y   &1] += idet->filter_line(cur-refs, prev, cur+refs, w);
            alpha[(y^1)&1] += idet->filter_line(cur-refs, next, cur+refs, w);
            delta          += idet->filter_line(cur-refs,  cur, cur+refs, w);
        }
    }
#if HAVE_MMX
    __asm__ volatile("emms \n\t" : : : "memory");
#endif

    if      (alpha[0] / (float)alpha[1] > idet->interlace_threshold){
        av_log(ctx, AV_LOG_INFO, "Interlaced, top field first\n");
        t++;
    }else if(alpha[1] / (float)alpha[0] > idet->interlace_threshold){
        av_log(ctx, AV_LOG_INFO, "Interlaced, bottom field first\n");
        b++;
    }else if(alpha[1] / (float)delta    > idet->progressive_threshold){
        av_log(ctx, AV_LOG_INFO, "Progressive\n");
        p++;
    }else{
        av_log(ctx, AV_LOG_INFO, "Undetermined\n");
        u++;
    }
//     av_log(ctx,0, "t%d b%d p%d u%d\n", t,b,p,u);
}

static void return_frame(AVFilterContext *ctx)
{
}

static void start_frame(AVFilterLink *link, AVFilterBufferRef *picref)
{
    AVFilterContext *ctx = link->dst;
    IDETContext *idet = ctx->priv;

    if (idet->prev)
        avfilter_unref_buffer(idet->prev);
    idet->prev = idet->cur;
    idet->cur  = idet->next;
    idet->next = picref;

    if (!idet->cur)
        return;

    if (!idet->prev)
        idet->prev = avfilter_ref_buffer(idet->cur, AV_PERM_READ);

    avfilter_start_frame(ctx->outputs[0], avfilter_ref_buffer(idet->cur, AV_PERM_READ));
}

static void end_frame(AVFilterLink *link)
{
    AVFilterContext *ctx = link->dst;
    IDETContext *idet = ctx->priv;

    if (!idet->cur)
        return;

    if (!idet->csp)
        idet->csp = &av_pix_fmt_descriptors[link->format];
    if (idet->csp->comp[0].depth_minus1 / 8 == 1)
        idet->filter_line = (void*)filter_line_c_16bit;

    filter(ctx);

    avfilter_draw_slice(ctx->outputs[0], 0, link->h, 1);
    avfilter_end_frame(ctx->outputs[0]);
}

static int request_frame(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    IDETContext *idet = ctx->priv;

    do {
        int ret;

        if ((ret = avfilter_request_frame(link->src->inputs[0])))
            return ret;
    } while (!idet->cur);

    return 0;
}

static int poll_frame(AVFilterLink *link)
{
    IDETContext *idet = link->src->priv;
    int ret, val;

    val = avfilter_poll_frame(link->src->inputs[0]);

    if (val >= 1 && !idet->next) { //FIXME change API to not requre this red tape
        if ((ret = avfilter_request_frame(link->src->inputs[0])) < 0)
            return ret;
        val = avfilter_poll_frame(link->src->inputs[0]);
    }
    assert(idet->next || !val);

    return val;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    IDETContext *idet = ctx->priv;

    if (idet->prev) avfilter_unref_buffer(idet->prev);
    if (idet->cur ) avfilter_unref_buffer(idet->cur );
    if (idet->next) avfilter_unref_buffer(idet->next);
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum PixelFormat pix_fmts[] = {
        PIX_FMT_YUV420P,
        PIX_FMT_YUV422P,
        PIX_FMT_YUV444P,
        PIX_FMT_YUV410P,
        PIX_FMT_YUV411P,
        PIX_FMT_GRAY8,
        PIX_FMT_YUVJ420P,
        PIX_FMT_YUVJ422P,
        PIX_FMT_YUVJ444P,
        AV_NE( PIX_FMT_GRAY16BE, PIX_FMT_GRAY16LE ),
        PIX_FMT_YUV440P,
        PIX_FMT_YUVJ440P,
        AV_NE( PIX_FMT_YUV420P10BE, PIX_FMT_YUV420P10LE ),
        AV_NE( PIX_FMT_YUV422P10BE, PIX_FMT_YUV422P10LE ),
        AV_NE( PIX_FMT_YUV444P10BE, PIX_FMT_YUV444P10LE ),
        AV_NE( PIX_FMT_YUV420P16BE, PIX_FMT_YUV420P16LE ),
        AV_NE( PIX_FMT_YUV422P16BE, PIX_FMT_YUV422P16LE ),
        AV_NE( PIX_FMT_YUV444P16BE, PIX_FMT_YUV444P16LE ),
        PIX_FMT_YUVA420P,
        PIX_FMT_NONE
    };

    avfilter_set_common_pixel_formats(ctx, avfilter_make_format_list(pix_fmts));

    return 0;
}

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    IDETContext *idet = ctx->priv;
    int cpu_flags = av_get_cpu_flags();

    idet->csp = NULL;

    idet->interlace_threshold   = 1.01;
    idet->progressive_threshold = 2.5;

    if (args) sscanf(args, "%f:%f", &idet->interlace_threshold, &idet->progressive_threshold);

    idet->filter_line = filter_line_c;

    return 0;
}

static void null_draw_slice(AVFilterLink *link, int y, int h, int slice_dir) { }

AVFilter avfilter_vf_idet = {
    .name          = "idet",
    .description   = NULL_IF_CONFIG_SMALL("Interlace detect Filter."),

    .priv_size     = sizeof(IDETContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,

    .inputs    = (const AVFilterPad[]) {{ .name       = "default",
                                    .type             = AVMEDIA_TYPE_VIDEO,
                                    .start_frame      = start_frame,
                                    .draw_slice       = null_draw_slice,
                                    .end_frame        = end_frame,
                                    .rej_perms        = AV_PERM_REUSE2, },
                                  { .name = NULL}},

    .outputs   = (const AVFilterPad[]) {{ .name       = "default",
                                    .type             = AVMEDIA_TYPE_VIDEO,
                                    .poll_frame       = poll_frame,
                                    .request_frame    = request_frame, },
                                  { .name = NULL}},
};

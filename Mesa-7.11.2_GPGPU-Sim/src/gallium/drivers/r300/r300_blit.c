/*
 * Copyright 2009 Marek Olšák <maraeo@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include "r300_context.h"
#include "r300_emit.h"
#include "r300_texture.h"

#include "util/u_format.h"
#include "util/u_pack_color.h"
#include "util/u_surface.h"

enum r300_blitter_op /* bitmask */
{
    R300_STOP_QUERY         = 1,
    R300_SAVE_TEXTURES      = 2,
    R300_SAVE_FRAMEBUFFER   = 4,
    R300_IGNORE_RENDER_COND = 8,

    R300_CLEAR         = R300_STOP_QUERY,

    R300_CLEAR_SURFACE = R300_STOP_QUERY | R300_SAVE_FRAMEBUFFER,

    R300_COPY          = R300_STOP_QUERY | R300_SAVE_FRAMEBUFFER |
                         R300_SAVE_TEXTURES | R300_IGNORE_RENDER_COND,

    R300_DECOMPRESS    = R300_STOP_QUERY | R300_IGNORE_RENDER_COND,
};

static void r300_blitter_begin(struct r300_context* r300, enum r300_blitter_op op)
{
    if ((op & R300_STOP_QUERY) && r300->query_current) {
        r300->blitter_saved_query = r300->query_current;
        r300_stop_query(r300);
    }

    /* Yeah we have to save all those states to ensure the blitter operation
     * is really transparent. The states will be restored by the blitter once
     * copying is done. */
    util_blitter_save_blend(r300->blitter, r300->blend_state.state);
    util_blitter_save_depth_stencil_alpha(r300->blitter, r300->dsa_state.state);
    util_blitter_save_stencil_ref(r300->blitter, &(r300->stencil_ref));
    util_blitter_save_rasterizer(r300->blitter, r300->rs_state.state);
    util_blitter_save_fragment_shader(r300->blitter, r300->fs.state);
    util_blitter_save_vertex_shader(r300->blitter, r300->vs_state.state);
    util_blitter_save_viewport(r300->blitter, &r300->viewport);
    util_blitter_save_clip(r300->blitter, (struct pipe_clip_state*)r300->clip_state.state);
    util_blitter_save_vertex_elements(r300->blitter, r300->velems);
    util_blitter_save_vertex_buffers(r300->blitter, r300->vbuf_mgr->nr_vertex_buffers,
                                     r300->vbuf_mgr->vertex_buffer);

    if (op & R300_SAVE_FRAMEBUFFER) {
        util_blitter_save_framebuffer(r300->blitter, r300->fb_state.state);
    }

    if (op & R300_SAVE_TEXTURES) {
        struct r300_textures_state* state =
            (struct r300_textures_state*)r300->textures_state.state;

        util_blitter_save_fragment_sampler_states(
            r300->blitter, state->sampler_state_count,
            (void**)state->sampler_states);

        util_blitter_save_fragment_sampler_views(
            r300->blitter, state->sampler_view_count,
            (struct pipe_sampler_view**)state->sampler_views);
    }

    if (op & R300_IGNORE_RENDER_COND) {
        /* Save the flag. */
        r300->blitter_saved_skip_rendering = r300->skip_rendering+1;
        r300->skip_rendering = FALSE;
    } else {
        r300->blitter_saved_skip_rendering = 0;
    }
}

static void r300_blitter_end(struct r300_context *r300)
{
    if (r300->blitter_saved_query) {
        r300_resume_query(r300, r300->blitter_saved_query);
        r300->blitter_saved_query = NULL;
    }

    if (r300->blitter_saved_skip_rendering) {
        /* Restore the flag. */
        r300->skip_rendering = r300->blitter_saved_skip_rendering-1;
    }
}

static uint32_t r300_depth_clear_cb_value(enum pipe_format format,
                                          const float* rgba)
{
    union util_color uc;
    util_pack_color(rgba, format, &uc);

    if (util_format_get_blocksizebits(format) == 32)
        return uc.ui;
    else
        return uc.us | (uc.us << 16);
}

static boolean r300_cbzb_clear_allowed(struct r300_context *r300,
                                       unsigned clear_buffers)
{
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;

    /* Only color clear allowed, and only one colorbuffer. */
    if (clear_buffers != PIPE_CLEAR_COLOR || fb->nr_cbufs != 1)
        return FALSE;

    return r300_surface(fb->cbufs[0])->cbzb_allowed;
}

static boolean r300_fast_zclear_allowed(struct r300_context *r300)
{
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;

    return r300_resource(fb->zsbuf->texture)->tex.zmask_dwords[fb->zsbuf->u.tex.level] != 0;
}

static boolean r300_hiz_clear_allowed(struct r300_context *r300)
{
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;

    return r300_resource(fb->zsbuf->texture)->tex.hiz_dwords[fb->zsbuf->u.tex.level] != 0;
}

static uint32_t r300_depth_clear_value(enum pipe_format format,
                                       double depth, unsigned stencil)
{
    switch (format) {
        case PIPE_FORMAT_Z16_UNORM:
        case PIPE_FORMAT_X8Z24_UNORM:
            return util_pack_z(format, depth);

        case PIPE_FORMAT_S8_USCALED_Z24_UNORM:
            return util_pack_z_stencil(format, depth, stencil);

        default:
            assert(0);
            return 0;
    }
}

static uint32_t r300_hiz_clear_value(double depth)
{
    uint32_t r = (uint32_t)(CLAMP(depth, 0, 1) * 255.5);
    assert(r <= 255);
    return r | (r << 8) | (r << 16) | (r << 24);
}

/* Clear currently bound buffers. */
static void r300_clear(struct pipe_context* pipe,
                       unsigned buffers,
                       const float* rgba,
                       double depth,
                       unsigned stencil)
{
    /* My notes about Zbuffer compression:
     *
     * 1) The zbuffer must be micro-tiled and whole microtiles must be
     *    written if compression is enabled. If microtiling is disabled,
     *    it locks up.
     *
     * 2) There is ZMASK RAM which contains a compressed zbuffer.
     *    Each dword of the Z Mask contains compression information
     *    for 16 4x4 pixel tiles, that is 2 bits for each tile.
     *    On chips with 2 Z pipes, every other dword maps to a different
     *    pipe. On newer chipsets, there is a new compression mode
     *    with 8x8 pixel tiles per 2 bits.
     *
     * 3) The FASTFILL bit has nothing to do with filling. It only tells hw
     *    it should look in the ZMASK RAM first before fetching from a real
     *    zbuffer.
     *
     * 4) If a pixel is in a cleared state, ZB_DEPTHCLEARVALUE is returned
     *    during zbuffer reads instead of the value that is actually stored
     *    in the zbuffer memory. A pixel is in a cleared state when its ZMASK
     *    is equal to 0. Therefore, if you clear ZMASK with zeros, you may
     *    leave the zbuffer memory uninitialized, but then you must enable
     *    compression, so that the ZMASK RAM is actually used.
     *
     * 5) Each 4x4 (or 8x8) tile is automatically decompressed and recompressed
     *    during zbuffer updates. A special decompressing operation should be
     *    used to fully decompress a zbuffer, which basically just stores all
     *    compressed tiles in ZMASK to the zbuffer memory.
     *
     * 6) For a 16-bit zbuffer, compression causes a hung with one or
     *    two samples and should not be used.
     *
     * 7) FORCE_COMPRESSED_STENCIL_VALUE should be enabled for stencil clears
     *    to avoid needless decompression.
     *
     * 8) Fastfill must not be used if reading of compressed Z data is disabled
     *    and writing of compressed Z data is enabled (RD/WR_COMP_ENABLE),
     *    i.e. it cannot be used to compress the zbuffer.
     *
     * 9) ZB_CB_CLEAR does not interact with zbuffer compression in any way.
     *
     * - Marek
     */

    struct r300_context* r300 = r300_context(pipe);
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;
    struct r300_hyperz_state *hyperz =
        (struct r300_hyperz_state*)r300->hyperz_state.state;
    uint32_t width = fb->width;
    uint32_t height = fb->height;
    uint32_t hyperz_dcv = hyperz->zb_depthclearvalue;

    /* Enable fast Z clear.
     * The zbuffer must be in micro-tiled mode, otherwise it locks up. */
    if (buffers & PIPE_CLEAR_DEPTHSTENCIL) {
        boolean zmask_clear, hiz_clear;

        zmask_clear = r300_fast_zclear_allowed(r300);
        hiz_clear = r300_hiz_clear_allowed(r300);

        /* If we need Hyper-Z. */
        if (zmask_clear || hiz_clear) {
            r300->num_z_clears++;

            /* Try to obtain the access to Hyper-Z buffers if we don't have one. */
            if (!r300->hyperz_enabled) {
                r300->hyperz_enabled =
                    r300->rws->cs_request_feature(r300->cs,
                                                RADEON_FID_HYPERZ_RAM_ACCESS,
                                                TRUE);
                if (r300->hyperz_enabled) {
                   /* Need to emit HyperZ buffer regs for the first time. */
                   r300_mark_fb_state_dirty(r300, R300_CHANGED_HYPERZ_FLAG);
                }
            }

            /* Setup Hyper-Z clears. */
            if (r300->hyperz_enabled) {
                DBG(r300, DBG_HYPERZ, "r300: Clear memory: %s%s\n",
                    zmask_clear ? "ZMASK " : "", hiz_clear ? "HIZ" : "");

                if (zmask_clear) {
                    hyperz_dcv = hyperz->zb_depthclearvalue =
                        r300_depth_clear_value(fb->zsbuf->format, depth, stencil);

                    r300_mark_atom_dirty(r300, &r300->zmask_clear);
                    buffers &= ~PIPE_CLEAR_DEPTHSTENCIL;
                }

                if (hiz_clear) {
                    r300->hiz_clear_value = r300_hiz_clear_value(depth);
                    r300_mark_atom_dirty(r300, &r300->hiz_clear);
                }
            }
        }
    }

    /* Enable CBZB clear. */
    if (r300_cbzb_clear_allowed(r300, buffers)) {
        struct r300_surface *surf = r300_surface(fb->cbufs[0]);

        hyperz->zb_depthclearvalue =
                r300_depth_clear_cb_value(surf->base.format, rgba);

        width = surf->cbzb_width;
        height = surf->cbzb_height;

        r300->cbzb_clear = TRUE;
        r300_mark_fb_state_dirty(r300, R300_CHANGED_HYPERZ_FLAG);
    }

    /* Clear. */
    if (buffers) {
        /* Clear using the blitter. */
        r300_blitter_begin(r300, R300_CLEAR);
        util_blitter_clear(r300->blitter,
                           width,
                           height,
                           fb->nr_cbufs,
                           buffers, rgba, depth, stencil);
        r300_blitter_end(r300);
    } else if (r300->zmask_clear.dirty || r300->hiz_clear.dirty) {
        /* Just clear zmask and hiz now, this does not use the standard draw
         * procedure. */
        /* Calculate zmask_clear and hiz_clear atom sizes. */
        unsigned dwords =
            (r300->zmask_clear.dirty ? r300->zmask_clear.size : 0) +
            (r300->hiz_clear.dirty ? r300->hiz_clear.size : 0) +
            r300_get_num_cs_end_dwords(r300);

        /* Reserve CS space. */
        if (dwords > (RADEON_MAX_CMDBUF_DWORDS - r300->cs->cdw)) {
            r300_flush(&r300->context, RADEON_FLUSH_ASYNC, NULL);
        }

        /* Emit clear packets. */
        if (r300->zmask_clear.dirty) {
            r300_emit_zmask_clear(r300, r300->zmask_clear.size,
                                  r300->zmask_clear.state);
            r300->zmask_clear.dirty = FALSE;
        }
        if (r300->hiz_clear.dirty) {
            r300_emit_hiz_clear(r300, r300->hiz_clear.size,
                                r300->hiz_clear.state);
            r300->hiz_clear.dirty = FALSE;
        }
    } else {
        assert(0);
    }

    /* Disable CBZB clear. */
    if (r300->cbzb_clear) {
        r300->cbzb_clear = FALSE;
        hyperz->zb_depthclearvalue = hyperz_dcv;
        r300_mark_fb_state_dirty(r300, R300_CHANGED_HYPERZ_FLAG);
    }

    /* Enable fastfill and/or hiz.
     *
     * If we cleared zmask/hiz, it's in use now. The Hyper-Z state update
     * looks if zmask/hiz is in use and programs hardware accordingly. */
    if (r300->zmask_in_use || r300->hiz_in_use) {
        r300_mark_atom_dirty(r300, &r300->hyperz_state);
    }
}

/* Clear a region of a color surface to a constant value. */
static void r300_clear_render_target(struct pipe_context *pipe,
                                     struct pipe_surface *dst,
                                     const float *rgba,
                                     unsigned dstx, unsigned dsty,
                                     unsigned width, unsigned height)
{
    struct r300_context *r300 = r300_context(pipe);

    r300_blitter_begin(r300, R300_CLEAR_SURFACE);
    util_blitter_clear_render_target(r300->blitter, dst, rgba,
                                     dstx, dsty, width, height);
    r300_blitter_end(r300);
}

/* Clear a region of a depth stencil surface. */
static void r300_clear_depth_stencil(struct pipe_context *pipe,
                                     struct pipe_surface *dst,
                                     unsigned clear_flags,
                                     double depth,
                                     unsigned stencil,
                                     unsigned dstx, unsigned dsty,
                                     unsigned width, unsigned height)
{
    struct r300_context *r300 = r300_context(pipe);
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;

    if (r300->zmask_in_use && !r300->locked_zbuffer) {
        if (fb->zsbuf->texture == dst->texture) {
            r300_decompress_zmask(r300);
        }
    }

    /* XXX Do not decompress ZMask of the currently-set zbuffer. */
    r300_blitter_begin(r300, R300_CLEAR_SURFACE);
    util_blitter_clear_depth_stencil(r300->blitter, dst, clear_flags, depth, stencil,
                                     dstx, dsty, width, height);
    r300_blitter_end(r300);
}

void r300_decompress_zmask(struct r300_context *r300)
{
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;

    if (!r300->zmask_in_use || r300->locked_zbuffer)
        return;

    r300->zmask_decompress = TRUE;
    r300_mark_atom_dirty(r300, &r300->hyperz_state);

    r300_blitter_begin(r300, R300_DECOMPRESS);
    util_blitter_clear_depth_custom(r300->blitter, fb->width, fb->height, 0,
                                    r300->dsa_decompress_zmask);
    r300_blitter_end(r300);

    r300->zmask_decompress = FALSE;
    r300->zmask_in_use = FALSE;
    r300_mark_atom_dirty(r300, &r300->hyperz_state);
}

void r300_decompress_zmask_locked_unsafe(struct r300_context *r300)
{
    struct pipe_framebuffer_state fb = {0};
    fb.width = r300->locked_zbuffer->width;
    fb.height = r300->locked_zbuffer->height;
    fb.nr_cbufs = 0;
    fb.zsbuf = r300->locked_zbuffer;

    r300->context.set_framebuffer_state(&r300->context, &fb);
    r300_decompress_zmask(r300);
}

void r300_decompress_zmask_locked(struct r300_context *r300)
{
    struct pipe_framebuffer_state saved_fb = {0};

    util_copy_framebuffer_state(&saved_fb, r300->fb_state.state);
    r300_decompress_zmask_locked_unsafe(r300);
    r300->context.set_framebuffer_state(&r300->context, &saved_fb);
    util_unreference_framebuffer_state(&saved_fb);

    pipe_surface_reference(&r300->locked_zbuffer, NULL);
}

/* Copy a block of pixels from one surface to another using HW. */
static void r300_hw_copy_region(struct pipe_context* pipe,
                                struct pipe_resource *dst,
                                unsigned dst_level,
                                unsigned dstx, unsigned dsty, unsigned dstz,
                                struct pipe_resource *src,
                                unsigned src_level,
                                const struct pipe_box *src_box)
{
    struct r300_context* r300 = r300_context(pipe);

    r300_blitter_begin(r300, R300_COPY);
    util_blitter_copy_region(r300->blitter, dst, dst_level, dstx, dsty, dstz,
                             src, src_level, src_box, TRUE);
    r300_blitter_end(r300);
}

/* Copy a block of pixels from one surface to another. */
static void r300_resource_copy_region(struct pipe_context *pipe,
                                      struct pipe_resource *dst,
                                      unsigned dst_level,
                                      unsigned dstx, unsigned dsty, unsigned dstz,
                                      struct pipe_resource *src,
                                      unsigned src_level,
                                      const struct pipe_box *src_box)
{
    struct r300_context *r300 = r300_context(pipe);
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;
    struct pipe_resource old_src = *src;
    struct pipe_resource old_dst = *dst;
    struct pipe_resource new_src = old_src;
    struct pipe_resource new_dst = old_dst;
    const struct util_format_description *desc =
            util_format_description(dst->format);
    struct pipe_box box;

    /* Fallback for buffers. */
    if (dst->target == PIPE_BUFFER && src->target == PIPE_BUFFER) {
        util_resource_copy_region(pipe, dst, dst_level, dstx, dsty, dstz,
                                  src, src_level, src_box);
        return;
    }

    if (r300->zmask_in_use && !r300->locked_zbuffer) {
        if (fb->zsbuf->texture == src ||
            fb->zsbuf->texture == dst) {
            r300_decompress_zmask(r300);
        }
    }

    /* Handle non-renderable plain formats. */
    if (desc->layout == UTIL_FORMAT_LAYOUT_PLAIN &&
        (desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB ||
         !pipe->screen->is_format_supported(pipe->screen,
                                            src->format, src->target,
                                            src->nr_samples,
                                            PIPE_BIND_SAMPLER_VIEW) ||
         !pipe->screen->is_format_supported(pipe->screen,
                                            dst->format, dst->target,
                                            dst->nr_samples,
                                            PIPE_BIND_RENDER_TARGET))) {
        switch (util_format_get_blocksize(old_dst.format)) {
            case 1:
                new_dst.format = PIPE_FORMAT_I8_UNORM;
                break;
            case 2:
                new_dst.format = PIPE_FORMAT_B4G4R4A4_UNORM;
                break;
            case 4:
                new_dst.format = PIPE_FORMAT_B8G8R8A8_UNORM;
                break;
            case 8:
                new_dst.format = PIPE_FORMAT_R16G16B16A16_UNORM;
                break;
            default:
                debug_printf("r300: surface_copy: Unhandled format: %s. Falling back to software.\n"
                             "r300: surface_copy: Software fallback doesn't work for tiled textures.\n",
                             util_format_short_name(dst->format));
        }
        new_src.format = new_dst.format;
    }

    /* Handle compressed formats. */
    if (desc->layout == UTIL_FORMAT_LAYOUT_S3TC ||
        desc->layout == UTIL_FORMAT_LAYOUT_RGTC) {
        switch (util_format_get_blocksize(old_dst.format)) {
        case 8:
            /* 1 pixel = 4 bits,
             * we set 1 pixel = 2 bytes ===> 4 times larger pixels. */
            new_dst.format = PIPE_FORMAT_B4G4R4A4_UNORM;
            break;
        case 16:
            /* 1 pixel = 8 bits,
             * we set 1 pixel = 4 bytes ===> 4 times larger pixels. */
            new_dst.format = PIPE_FORMAT_B8G8R8A8_UNORM;
            break;
        }

        /* Since the pixels are 4 times larger, we must decrease
         * the image size and the coordinates 4 times. */
        new_src.format = new_dst.format;
        new_dst.height0 = (new_dst.height0 + 3) / 4;
        new_src.height0 = (new_src.height0 + 3) / 4;
        dsty /= 4;
        box = *src_box;
        box.y /= 4;
        box.height = (box.height + 3) / 4;
        src_box = &box;
    }

    if (old_src.format != new_src.format)
        r300_resource_set_properties(pipe->screen, src, 0, &new_src);
    if (old_dst.format != new_dst.format)
        r300_resource_set_properties(pipe->screen, dst, 0, &new_dst);

    r300_hw_copy_region(pipe, dst, dst_level, dstx, dsty, dstz,
                        src, src_level, src_box);

    if (old_src.format != new_src.format)
        r300_resource_set_properties(pipe->screen, src, 0, &old_src);
    if (old_dst.format != new_dst.format)
        r300_resource_set_properties(pipe->screen, dst, 0, &old_dst);
}

void r300_init_blit_functions(struct r300_context *r300)
{
    r300->context.clear = r300_clear;
    r300->context.clear_render_target = r300_clear_render_target;
    r300->context.clear_depth_stencil = r300_clear_depth_stencil;
    r300->context.resource_copy_region = r300_resource_copy_region;
}
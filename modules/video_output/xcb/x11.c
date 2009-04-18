/**
 * @file x11.c
 * @brief X C Bindings video output module for VLC media player
 */
/*****************************************************************************
 * Copyright © 2009 Rémi Denis-Courmont
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2.0
 * of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 ****************************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/shm.h>

#include <xcb/xcb.h>
#include <xcb/shm.h>

#include <xcb/xcb_image.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout.h>
#include <vlc_window.h>

#include "xcb_vlc.h"

#define DISPLAY_TEXT N_("X11 display")
#define DISPLAY_LONGTEXT N_( \
    "X11 hardware display to use. By default VLC will " \
    "use the value of the DISPLAY environment variable.")

#define SHM_TEXT N_("Use shared memory")
#define SHM_LONGTEXT N_( \
    "Use shared memory to communicate between VLC and the X server.")

static int  Open (vlc_object_t *);
static void Close (vlc_object_t *);

/*
 * Module descriptor
 */
vlc_module_begin ()
    set_shortname (N_("XCB"))
    set_description (N_("(Experimental) XCB video output"))
    set_category (CAT_VIDEO)
    set_subcategory (SUBCAT_VIDEO_VOUT)
    set_capability ("video output", 0)
    set_callbacks (Open, Close)

    add_string ("x11-display", NULL, NULL,
                DISPLAY_TEXT, DISPLAY_LONGTEXT, true)
    add_bool ("x11-shm", true, NULL, SHM_TEXT, SHM_LONGTEXT, true)
vlc_module_end ()

struct vout_sys_t
{
    xcb_connection_t *conn;
    xcb_screen_t *screen;
    vout_window_t *embed; /* VLC window (when windowed) */

    xcb_visualid_t vid; /* selected visual */
    xcb_colormap_t cmap; /* colormap for selected visual */
    xcb_window_t window; /* drawable X window */
    xcb_gcontext_t gc; /* context to put images */
    bool shm; /* whether to use MIT-SHM */
    uint8_t bpp; /* bits per pixel */
    uint8_t pad; /* scanline pad */
    uint8_t byte_order; /* server byte order */
};

static int Init (vout_thread_t *);
static void Deinit (vout_thread_t *);
static void Display (vout_thread_t *, picture_t *);
static int Manage (vout_thread_t *);

int CheckError (vout_thread_t *vout, const char *str, xcb_void_cookie_t ck)
{
    xcb_generic_error_t *err;

    err = xcb_request_check (vout->p_sys->conn, ck);
    if (err)
    {
        msg_Err (vout, "%s: X11 error %d", str, err->error_code);
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

#define p_vout vout

/**
 * Probe the X server.
 */
static int Open (vlc_object_t *obj)
{
    vout_thread_t *vout = (vout_thread_t *)obj;
    vout_sys_t *p_sys = malloc (sizeof (*p_sys));
    if (p_sys == NULL)
        return VLC_ENOMEM;

    vout->p_sys = p_sys;
    p_sys->conn = NULL;
    p_sys->embed = NULL;

    /* Connect to X */
    char *display = var_CreateGetNonEmptyString (vout, "x11-display");
    p_sys->conn = xcb_connect (display, NULL);
    if (xcb_connection_has_error (p_sys->conn) /*== NULL*/)
    {
        msg_Err (vout, "cannot connect to X server %s",
                 display ? display : "");
        free (display);
        goto error;
    }
    free (display);

    /* Get window */
    p_sys->embed = vout_RequestXWindow (vout, &(int){ 0 }, &(int){ 0 },
                                        &(unsigned){ 0 }, &(unsigned){ 0 });
    if (p_sys->embed == NULL)
    {
        msg_Err (vout, "parent window not available");
        goto error;
    }
    xcb_window_t root;
    {
        xcb_get_geometry_reply_t *geo;
        xcb_get_geometry_cookie_t ck;

        ck = xcb_get_geometry (p_sys->conn, p_sys->embed->handle.xid);
        geo = xcb_get_geometry_reply (p_sys->conn, ck, NULL);
        if (geo == NULL)
        {
            msg_Err (vout, "parent window not valid");
            goto error;
        }
        root = geo->root;
        free (geo);

        /* Subscribe to parent window resize events */
        const uint32_t value = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes (p_sys->conn, p_sys->embed->handle.xid,
                                      XCB_CW_EVENT_MASK, &value);
    }

    /* Find the selected screen */
    const xcb_setup_t *setup = xcb_get_setup (p_sys->conn);
    p_sys->byte_order = setup->image_byte_order;

    xcb_screen_t *scr = NULL;
    for (xcb_screen_iterator_t i = xcb_setup_roots_iterator (setup);
         i.rem > 0 && scr == NULL; xcb_screen_next (&i))
    {
        if (i.data->root == root)
            scr = i.data;
    }

    if (scr == NULL)
    {
        msg_Err (vout, "parent window screen not found");
        goto error;
    }
    p_sys->screen = scr;
    msg_Dbg (vout, "using screen 0x%"PRIx32, scr->root);

    /* Determine our video format. Normally, this is done in pf_init(), but
     * this plugin always uses the same format for a given X11 screen. */
    uint8_t depth = 0;
    bool gray = true;
    for (const xcb_format_t *fmt = xcb_setup_pixmap_formats (setup),
             *end = fmt + xcb_setup_pixmap_formats_length (setup);
         fmt < end; fmt++)
    {
        vlc_fourcc_t chroma = 0;

        if (fmt->depth < depth)
            continue; /* We already found a better format! */

        /* Check that the pixmap format is supported by VLC. */
        switch (fmt->depth)
        {
          case 24:
            if (fmt->bits_per_pixel == 32)
                chroma = VLC_FOURCC ('R', 'V', '3', '2');
            else if (fmt->bits_per_pixel == 24)
                chroma = VLC_FOURCC ('R', 'V', '2', '4');
            else
                continue;
            break;
          case 16:
            if (fmt->bits_per_pixel != 16)
                continue;
            chroma = VLC_FOURCC ('R', 'V', '1', '6');
            break;
          case 15:
            if (fmt->bits_per_pixel != 16)
                continue;
            chroma = VLC_FOURCC ('R', 'V', '1', '5');
            break;
          case 8:
            if (fmt->bits_per_pixel != 8)
                continue;
            chroma = VLC_FOURCC ('R', 'G', 'B', '2');
            break;
          default:
            continue;
        }
        if ((fmt->bits_per_pixel << 4) % fmt->scanline_pad)
            continue; /* VLC pads lines to 16 pixels internally */

        /* Byte sex is a non-issue for 8-bits. It can be worked around with
         * RGB masks for 24-bits. Too bad for 15-bits and 16-bits. */
#ifdef WORDS_BIGENDIAN
# define ORDER XCB_IMAGE_ORDER_MSB_FIRST
#else
# define ORDER XCB_IMAGE_ORDER_LSB_FIRST
#endif
        if (fmt->bits_per_pixel == 16 && setup->image_byte_order != ORDER)
            continue;

        /* Check that the selected screen supports this depth */
        xcb_depth_iterator_t it = xcb_screen_allowed_depths_iterator (scr);
        while (it.rem > 0 && it.data->depth != fmt->depth)
             xcb_depth_next (&it);
        if (!it.rem)
            continue; /* Depth not supported on this screen */

        /* Find a visual type for the selected depth */
        const xcb_visualtype_t *vt = xcb_depth_visuals (it.data);
        xcb_visualid_t vid = 0;
        for (int i = xcb_depth_visuals_length (it.data); (i > 0) && !vid; i--)
        {
            if (vt->_class == XCB_VISUAL_CLASS_TRUE_COLOR)
            {
                msg_Dbg (vout,
                         "using TrueColor %"PRIu8"-bits visual ID 0x%0"PRIx32,
                         fmt->depth, vt->visual_id);
                vid = vt->visual_id;
                gray = false;
                break;
            }
            if (fmt->depth == 8 && vt->_class == XCB_VISUAL_CLASS_STATIC_GRAY)
            {
                if (!gray)
                    continue; /* Prefer color over gray scale */
                vid = vt->visual_id;
                chroma = VLC_FOURCC ('G', 'R', 'E', 'Y');
                msg_Dbg (vout,
                         "using static gray 8-bits visual ID 0x%x"PRIx32,
                         vt->visual_id);
                break;
            }
        }

        if (!vid)
            continue; /* The screen does not *really* support this depth */

        vout->fmt_out.i_chroma = vout->output.i_chroma = chroma;
        if (!gray)
        {
            vout->output.i_rmask = vt->red_mask;
            vout->output.i_gmask = vt->green_mask;
            vout->output.i_bmask = vt->blue_mask;
        }
        p_sys->vid = vid;
        p_sys->bpp = fmt->bits_per_pixel;
        p_sys->pad = fmt->scanline_pad;

        depth = fmt->depth;
    }

    if (depth == 0)
    {
        msg_Err (vout, "no supported pixmap formats");
        goto error;
    }

    msg_Dbg (vout, "using %"PRIu8" bits per pixels (line pad: %"PRIu8")",
             p_sys->bpp, p_sys->pad);

    /* Create colormap (needed to select non-default visual) */
    if (p_sys->vid != scr->root_visual)
    {
        p_sys->cmap = xcb_generate_id (p_sys->conn);
        xcb_create_colormap (p_sys->conn, XCB_COLORMAP_ALLOC_NONE,
                             p_sys->cmap, scr->root, p_sys->vid);
    }
    else
        p_sys->cmap = scr->default_colormap;

    /* Check shared memory support */
    p_sys->shm = var_CreateGetBool (vout, "x11-shm") > 0;
    if (p_sys->shm)
    {
        xcb_shm_query_version_cookie_t ck;
        xcb_shm_query_version_reply_t *r;

        ck = xcb_shm_query_version (p_sys->conn);
        r = xcb_shm_query_version_reply (p_sys->conn, ck, NULL);
        if (!r)
        {
            msg_Err (vout, "shared memory (MIT-SHM) not available");
            msg_Warn (vout, "display will be slow");
            p_sys->shm = false;
        }
        free (r);
    }

    vout->pf_init = Init;
    vout->pf_end = Deinit;
    vout->pf_display = Display;
    vout->pf_manage = Manage;
    return VLC_SUCCESS;

error:
    Close (obj);
    return VLC_EGENERIC;
}


/**
 * Disconnect from the X server.
 */
static void Close (vlc_object_t *obj)
{
    vout_thread_t *vout = (vout_thread_t *)obj;
    vout_sys_t *p_sys = vout->p_sys;

    if (p_sys->embed)
        vout_ReleaseWindow (p_sys->embed);
    /* colormap is garbage-ollected by X (?) */
    if (p_sys->conn)
        xcb_disconnect (p_sys->conn);
    free (p_sys);
}

struct picture_sys_t
{
    xcb_connection_t *conn; /* Shared connection to X server */
    xcb_image_t *image;  /* Picture buffer */
    xcb_shm_seg_t segment; /* Shared memory segment X ID */
};

#define SHM_ERR ((void *)(intptr_t)(-1))

static int PictureInit (vout_thread_t *vout, picture_t *pic)
{
    vout_sys_t *p_sys = vout->p_sys;
    picture_sys_t *priv = malloc (sizeof (*p_sys));

    if (priv == NULL)
        return VLC_ENOMEM;

    assert (pic->i_status == FREE_PICTURE);
    vout_InitPicture (vout, pic, vout->output.i_chroma,
                      vout->output.i_width, vout->output.i_height,
                      vout->output.i_aspect);

    void *shm = SHM_ERR;
    const size_t size = pic->p->i_pitch * pic->p->i_lines;

    if (p_sys->shm)
    {   /* Allocate shared memory segment */
        int id = shmget (IPC_PRIVATE, size, IPC_CREAT | 0700);

        if (id == -1)
        {
            msg_Err (vout, "shared memory allocation error: %m");
            goto error;
        }

        /* Attach the segment to VLC */
        shm = shmat (id, NULL, 0 /* read/write */);
        if (shm == SHM_ERR)
        {
            msg_Err (vout, "shared memory attachment error: %m");
            shmctl (id, IPC_RMID, 0);
            goto error;
        }

        /* Attach the segment to X */
        xcb_void_cookie_t ck;

        priv->segment = xcb_generate_id (p_sys->conn);
        ck = xcb_shm_attach_checked (p_sys->conn, priv->segment, id, 1);
        shmctl (id, IPC_RMID, 0);

        if (CheckError (vout, "shared memory server-side error", ck))
        {
            msg_Info (vout, "using buggy X (remote) server? SSH?");
            shmdt (shm);
            shm = SHM_ERR;
        }
    }
    else
        priv->segment = 0;

    const unsigned real_width = pic->p->i_pitch / (p_sys->bpp >> 3);
    /* FIXME: anyway to getthing more intuitive than that?? */

    xcb_image_t *img;
    img = xcb_image_create (real_width, pic->p->i_lines,
                            XCB_IMAGE_FORMAT_Z_PIXMAP, p_sys->pad,
                            p_sys->screen->root_depth, p_sys->bpp, p_sys->bpp,
                            p_sys->byte_order, XCB_IMAGE_ORDER_MSB_FIRST,
                            NULL,
                            (shm != SHM_ERR) ? size : 0,
                            (shm != SHM_ERR) ? shm : NULL);

    if (img == NULL)
    {
        if (shm != SHM_ERR)
            xcb_shm_detach (p_sys->conn, priv->segment);
        goto error;
    }

    priv->conn = p_sys->conn;
    priv->image = img;
    pic->p_sys = priv;
    pic->p->p_pixels = img->data;
    pic->i_status = DESTROYED_PICTURE;
    pic->i_type = DIRECT_PICTURE;
    return VLC_SUCCESS;

error:
    if (shm != SHM_ERR)
        shmdt (shm);
    free (priv);
    return VLC_EGENERIC;
}


/**
 * Release picture private data
 */
static void PictureDeinit (picture_t *pic)
{
    struct picture_sys_t *p_sys = pic->p_sys;

    if (p_sys->segment != 0)
    {
        xcb_shm_detach (p_sys->conn, p_sys->segment);
        shmdt (p_sys->image->data);
    }
    xcb_image_destroy (p_sys->image);
    free (p_sys);
}

static void get_window_size (xcb_connection_t *conn, xcb_window_t win,
                             unsigned *width, unsigned *height)
{
    xcb_get_geometry_cookie_t ck = xcb_get_geometry (conn, win);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply (conn, ck, NULL);

    if (geo)
    {
        *width = geo->width;
        *height = geo->height;
        free (geo);
    }
    else
        *width = *height = 0;
}

/**
 * Allocate drawable window and picture buffers.
 */
static int Init (vout_thread_t *vout)
{
    vout_sys_t *p_sys = vout->p_sys;
    const xcb_screen_t *screen = p_sys->screen;
    unsigned x, y, width, height;
    xcb_window_t parent = p_sys->embed->handle.xid;

    I_OUTPUTPICTURES = 0;

    get_window_size (p_sys->conn, parent, &width, &height);
    vout_PlacePicture (vout, width, height, &x, &y, &width, &height);

    /* FIXME: I don't get the subtlety between output and fmt_out here */
    vout->fmt_out.i_visible_width = width;
    vout->fmt_out.i_visible_height = height;
    vout->fmt_out.i_sar_num = vout->fmt_out.i_sar_den = 1;

    vout->output.i_width = vout->fmt_out.i_width =
        width * vout->fmt_in.i_width / vout->fmt_in.i_visible_width;
    vout->output.i_height = vout->fmt_out.i_height =
        height * vout->fmt_in.i_height / vout->fmt_in.i_visible_height;
    vout->fmt_out.i_x_offset =
        width * vout->fmt_in.i_x_offset / vout->fmt_in.i_visible_width;
    p_vout->fmt_out.i_y_offset =
        height * vout->fmt_in.i_y_offset / vout->fmt_in.i_visible_height;

    assert (height > 0);
    vout->output.i_aspect = vout->fmt_out.i_aspect =
        width * VOUT_ASPECT_FACTOR / height;

    /* Create window */
    const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK
                        | XCB_CW_COLORMAP;
    const uint32_t values[] = {
        /* XCB_CW_BACK_PIXEL */
        screen->black_pixel,
        /* XCB_CW_EVENT_MASK */
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION,
        /* XCB_CW_COLORMAP */
        p_sys->cmap,
    };
    xcb_void_cookie_t c;
    xcb_window_t window = xcb_generate_id (p_sys->conn);

    c = xcb_create_window_checked (p_sys->conn, screen->root_depth, window,
                                   parent, x, y, width, height, 0,
                                   XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                   p_sys->vid, mask, values);
    if (CheckError (vout, "cannot create X11 window", c))
        goto error;
    p_sys->window = window;
    msg_Dbg (vout, "using X11 window %08"PRIx32, p_sys->window);
    xcb_map_window (p_sys->conn, window);

    /* Create graphic context (I wonder why the heck do we need this) */
    p_sys->gc = xcb_generate_id (p_sys->conn);
    xcb_create_gc (p_sys->conn, p_sys->gc, p_sys->window, 0, NULL);
    msg_Dbg (vout, "using X11 graphic context %08"PRIx32, p_sys->gc);
    xcb_flush (p_sys->conn);

    /* Allocate picture buffers */
    I_OUTPUTPICTURES = 0;
    for (size_t index = 0; I_OUTPUTPICTURES < 2; index++)
    {
        picture_t *pic = vout->p_picture + index;

        if (index > sizeof (vout->p_picture) / sizeof (pic))
            break;
        if (pic->i_status != FREE_PICTURE)
            continue;
        if (PictureInit (vout, pic))
            break;
        PP_OUTPUTPICTURE[I_OUTPUTPICTURES++] = pic;
    }

    return VLC_SUCCESS;

error:
    Deinit (vout);
    return VLC_EGENERIC;
}

/**
 * Free picture buffers.
 */
static void Deinit (vout_thread_t *vout)
{
    vout_sys_t *p_sys = vout->p_sys;

    for (int i = 0; i < I_OUTPUTPICTURES; i++)
        PictureDeinit (PP_OUTPUTPICTURE[i]);

    xcb_unmap_window (p_sys->conn, p_sys->window);
    xcb_destroy_window (p_sys->conn, p_sys->window);
}

/**
 * Sends an image to the X server.
 */
static void Display (vout_thread_t *vout, picture_t *pic)
{
    vout_sys_t *p_sys = vout->p_sys;
    picture_sys_t *priv = pic->p_sys;
    xcb_image_t *img = priv->image;

    if (img->base == NULL)
    {
        xcb_shm_segment_info_t info = {
            .shmseg = priv->segment,
            .shmid = -1, /* lost track of it, unimportant */
            .shmaddr = img->data,
        };

        xcb_image_shm_put (p_sys->conn, p_sys->window, p_sys->gc, img, info,
                        0, 0, 0, 0, img->width, img->height, 0);
    }
    else
        xcb_image_put (p_sys->conn, p_sys->window, p_sys->gc, img, 0, 0, 0);
    xcb_flush (p_sys->conn);
}

/**
 * Process incoming X events.
 */
static int Manage (vout_thread_t *vout)
{
    vout_sys_t *p_sys = vout->p_sys;
    xcb_generic_event_t *ev;

    while ((ev = xcb_poll_for_event (p_sys->conn)) != NULL)
        ProcessEvent (vout, p_sys->conn, p_sys->window, ev);

    if (xcb_connection_has_error (p_sys->conn))
    {
        msg_Err (vout, "X server failure");
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}
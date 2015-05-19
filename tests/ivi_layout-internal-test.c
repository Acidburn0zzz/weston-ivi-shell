/*
 * Copyright © 2013 DENSO CORPORATION
 * Copyright © 2015 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>

#include "../src/compositor.h"
#include "../ivi-shell/ivi-layout-export.h"

struct test_context {
	struct weston_compositor *compositor;
	const struct ivi_controller_interface *controller_interface;
};

static void
iassert_fail(const char *cond, const char *file, int line,
	     const char *func, struct test_context *ctx)
{
	weston_log("Assert failure in %s:%d, %s: '%s'\n",
		   file, line, func, cond);
	weston_compositor_exit_with_code(ctx->compositor, EXIT_FAILURE);
}

#define iassert(cond) ({						\
	bool b_ = (cond);						\
	if (!b_)							\
		iassert_fail(#cond, __FILE__, __LINE__, __func__, ctx);	\
	b_;								\
})

/************************ tests begin ******************************/

/*
 * These are all internal ivi_layout API tests that do not require
 * any client objects.
 */

static void
test_surface_bad_visibility(struct test_context *ctx)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	bool visibility;

	iassert(ctl->surface_set_visibility(NULL, true) == IVI_FAILED);

	ctl->commit_changes();

	visibility = ctl->surface_get_visibility(NULL);
	iassert(visibility == false);
}

static void
test_surface_bad_destination_rectangle(struct test_context *ctx)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	iassert(ctl->surface_set_destination_rectangle(NULL, 20, 30, 200, 300) == IVI_FAILED);
}

static void
test_surface_bad_orientation(struct test_context *ctx)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	iassert(ctl->surface_set_orientation(NULL, WL_OUTPUT_TRANSFORM_90) == IVI_FAILED);

	iassert(ctl->surface_get_orientation(NULL) == WL_OUTPUT_TRANSFORM_NORMAL);
}

static void
test_surface_bad_dimension(struct test_context *ctx)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf = NULL;
	int32_t dest_width;
	int32_t dest_height;

	iassert(ctl->surface_set_dimension(NULL, 200, 300) == IVI_FAILED);

	ctl->commit_changes();

	iassert(ctl->surface_get_dimension(NULL, &dest_width, &dest_height) == IVI_FAILED);
	iassert(ctl->surface_get_dimension(ivisurf, NULL, &dest_height) == IVI_FAILED);
	iassert(ctl->surface_get_dimension(ivisurf, &dest_width, NULL) == IVI_FAILED);
}

static void
test_surface_bad_position(struct test_context *ctx)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf = NULL;
	int32_t dest_x;
	int32_t dest_y;

	iassert(ctl->surface_set_position(NULL, 20, 30) == IVI_FAILED);

	ctl->commit_changes();

	iassert(ctl->surface_get_position(NULL, &dest_x, &dest_y) == IVI_FAILED);
	iassert(ctl->surface_get_position(ivisurf, NULL, &dest_y) == IVI_FAILED);
	iassert(ctl->surface_get_position(ivisurf, &dest_x, NULL) == IVI_FAILED);
}

static void
test_surface_bad_source_rectangle(struct test_context *ctx)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	iassert(ctl->surface_set_source_rectangle(NULL, 20, 30, 200, 300) == IVI_FAILED);
}

static void
test_surface_bad_properties(struct test_context *ctx)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	iassert(ctl->get_properties_of_surface(NULL) == NULL);
}

/************************ tests end ********************************/

static void
run_internal_tests(void *data)
{
	struct test_context *ctx = data;

	test_surface_bad_visibility(ctx);
	surface_set_source_rectangle_bad_address(ctx);
	surface_set_destination_rectangle_bad_address(ctx);
	test_surface_bad_destination_rectangle(ctx);
	test_surface_bad_orientation(ctx);
	test_surface_bad_dimension(ctx);
	test_surface_bad_position(ctx);
	test_surface_bad_source_rectangle(ctx);
	test_surface_bad_properties(ctx);

	weston_compositor_exit_with_code(ctx->compositor, EXIT_SUCCESS);
	free(ctx);
}

int
controller_module_init(struct weston_compositor *compositor,
		       int *argc, char *argv[],
		       const struct ivi_controller_interface *iface,
		       size_t iface_version);

WL_EXPORT int
controller_module_init(struct weston_compositor *compositor,
		       int *argc, char *argv[],
		       const struct ivi_controller_interface *iface,
		       size_t iface_version)
{
	struct wl_event_loop *loop;
	struct test_context *ctx;

	/* strict check, since this is an internal test module */
	if (iface_version != sizeof(*iface)) {
		weston_log("fatal: controller interface mismatch\n");
		return -1;
	}

	ctx = zalloc(sizeof(*ctx));
	if (!ctx)
		return -1;

	ctx->compositor = compositor;
	ctx->controller_interface = iface;

	loop = wl_display_get_event_loop(compositor->wl_display);
	wl_event_loop_add_idle(loop, run_internal_tests, ctx);

	return 0;
}

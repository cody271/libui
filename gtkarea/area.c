// 4 september 2015
#include "area.h"

struct areaPrivate {
	uiArea *a;
	uiAreaHandler *ah;

	GtkAdjustment *ha;
	GtkAdjustment *va;
	// TODO get rid of the need for these
	int clientWidth;
	int clientHeight;
	// needed for GtkScrollable
	GtkScrollablePolicy hpolicy, vpolicy;
	clickCounter cc;
};

static void areaWidget_scrollable_init(GtkScrollable *);

G_DEFINE_TYPE_WITH_CODE(areaWidget, areaWidget, GTK_TYPE_DRAWING_AREA,
	G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, areaWidget_scrollable_init))

/*
lower and upper are the bounds of the adjusment, in units
step_increment is the number of units scrolled when using the arrow keys or the buttons on an old-style scrollbar
page_incremenet is the number of page_size units scrolled with the Page Up/Down keys
according to baedert, the other condition is that upper >= page_size, and the effect is that the largest possible value is upper - page_size

unfortunately, everything in GTK+ assumes 1 unit = 1 pixel
let's do the same :/
*/
static void updateScroll(areaWidget *a)
{
	struct areaPrivate *ap = a->priv;
	uintmax_t count;

	// don't call if too early
	if (ap->ha == NULL || ap->va == NULL)
		return;

	count = (*(ap->ah->HScrollMax))(ap->ah, ap->a);
	gtk_adjustment_configure(ap->ha,
		gtk_adjustment_get_value(ap->ha),
		0,
		count,
		1,
		ap->clientWidth,
		MIN(count, ap->clientWidth));

	count = (*(ap->ah->VScrollMax))(ap->ah, ap->a);
	gtk_adjustment_configure(ap->va,
		gtk_adjustment_get_value(ap->va),
		0,
		count,
		1,
		ap->clientHeight,
		MIN(count, ap->clientHeight));

	// TODO notify adjustment changes?
//	g_object_notify(G_OBJECT(a), "hadjustment");
//	g_object_notify(G_OBJECT(a), "vadjustment");
}

static void areaWidget_init(areaWidget *a)
{
	a->priv = G_TYPE_INSTANCE_GET_PRIVATE(a, areaWidgetType, struct areaPrivate);

	// for events
	gtk_widget_add_events(GTK_WIDGET(a),
		GDK_POINTER_MOTION_MASK |
		GDK_BUTTON_MOTION_MASK |
		GDK_BUTTON_PRESS_MASK |
		GDK_BUTTON_RELEASE_MASK |
		GDK_KEY_PRESS_MASK |
		GDK_KEY_RELEASE_MASK);

	// for scrolling
	// TODO do we need GDK_TOUCH_MASK?
	gtk_widget_add_events(GTK_WIDGET(a),
		GDK_SCROLL_MASK |
		GDK_TOUCH_MASK |
		GDK_SMOOTH_SCROLL_MASK);

	gtk_widget_set_can_focus(GTK_WIDGET(a), TRUE);

	clickCounterReset(&(a->priv->cc));
}

static void areaWidget_dispose(GObject *obj)
{
	struct areaPrivate *ap = areaWidget(obj)->priv;

	if (ap->ha != NULL) {
		g_object_unref(ap->ha);
		ap->ha = NULL;
	}
	if (ap->va != NULL) {
		g_object_unref(ap->va);
		ap->va = NULL;
	}
	G_OBJECT_CLASS(areaWidget_parent_class)->dispose(obj);
}

static void areaWidget_finalize(GObject *obj)
{
	G_OBJECT_CLASS(areaWidget_parent_class)->finalize(obj);
}

static void areaWidget_size_allocate(GtkWidget *w, GtkAllocation *allocation)
{
	struct areaPrivate *ap = areaWidget(w)->priv;

	// GtkDrawingArea has a size_allocate() implementation; we need to call it
	// this will call gtk_widget_set_allocation() for us
	GTK_WIDGET_CLASS(areaWidget_parent_class)->size_allocate(w, allocation);
	ap->clientWidth = allocation->width;
	ap->clientHeight = allocation->height;
	updateScroll(areaWidget(w));
	if ((*(ap->ah->RedrawOnResize))(ap->ah, ap->a))
		gtk_widget_queue_resize(w);
}

static gboolean areaWidget_draw(GtkWidget *w, cairo_t *cr)
{
	areaWidget *a = areaWidget(w);
	struct areaPrivate *ap = a->priv;
	uiAreaDrawParams dp;
	double clipX0, clipY0, clipX1, clipY1;

	dp.Context = newContext(cr);

	dp.ClientWidth = ap->clientWidth;
	dp.ClientHeight = ap->clientHeight;

	cairo_clip_extents(cr, &clipX0, &clipY0, &clipX1, &clipY1);
	dp.ClipX = clipX0;
	dp.ClipY = clipY0;
	dp.ClipWidth = clipX1 - clipX0;
	dp.ClipHeight = clipY1 - clipY0;

	// on GTK+ you're not supposed to care about high-DPI scaling
	// instead, pango handles scaled text rendering for us
	// this doesn't handle non-text cases, but neither do other GTK+ programs, so :/
	// wayland and mir GDK are hardcoded to 96dpi; X11 uses this as a fallback
	// thanks to hergertme in irc.gimp.net/#gtk+ for clarifying things
	dp.DPIX = 96;
	dp.DPIY = 96;

	dp.HScrollPos = gtk_adjustment_get_value(ap->ha);
	dp.VScrollPos = gtk_adjustment_get_value(ap->va);

	(*(ap->ah->Draw))(ap->ah, ap->a, &dp);

	g_free(dp.Context);
	return FALSE;
}

// TODO preferred height/width

// TODO merge with toModifiers?
static guint translateModifiers(guint state, GdkWindow *window)
{
	GdkModifierType statetype;

	// GDK doesn't initialize the modifier flags fully; we have to explicitly tell it to (thanks to Daniel_S and daniels (two different people) in irc.gimp.net/#gtk+)
	statetype = state;
	gdk_keymap_add_virtual_modifiers(
		gdk_keymap_get_for_display(gdk_window_get_display(window)),
		&statetype);
	return statetype;
}

static uiModifiers toModifiers(guint state)
{
	uiModifiers m;

	m = 0;
	if ((state & GDK_CONTROL_MASK) != 0)
		m |= uiModifierCtrl;
	if ((state & GDK_META_MASK) != 0)
		m |= uiModifierAlt;
	if ((state & GDK_MOD1_MASK) != 0)		// GTK+ itself requires this to be Alt (just read through gtkaccelgroup.c)
		m |= uiModifierAlt;
	if ((state & GDK_SHIFT_MASK) != 0)
		m |= uiModifierShift;
	if ((state & GDK_SUPER_MASK) != 0)
		m |= uiModifierSuper;
	return m;
}

// capture on drag is done automatically on GTK+
static void finishMouseEvent(struct areaPrivate *ap, uiAreaMouseEvent *me, guint mb, gdouble x, gdouble y, guint state, GdkWindow *window)
{
	// on GTK+, mouse buttons 4-7 are for scrolling; if we got here, that's a mistake
	if (mb >= 4 && mb <= 7)
		return;
	// if the button ID >= 8, continue counting from 4, as in the MouseEvent spec
	if (me->Down >= 8)
		me->Down -= 4;
	if (me->Up >= 8)
		me->Up -= 4;

	state = translateModifiers(state, window);
	me->Modifiers = toModifiers(state);

	// the mb != # checks exclude the Up/Down button from Held
	me->Held1To64 = 0;
	if (mb != 1 && (state & GDK_BUTTON1_MASK) != 0)
		me->Held1To64 |= 1 << 0;
	if (mb != 2 && (state & GDK_BUTTON2_MASK) != 0)
		me->Held1To64 |= 1 << 1;
	if (mb != 3 && (state & GDK_BUTTON3_MASK) != 0)
		me->Held1To64 |= 1 << 2;
	// don't check GDK_BUTTON4_MASK or GDK_BUTTON5_MASK because those are for the scrolling buttons mentioned above
	// GDK expressly does not support any more buttons in the GdkModifierType; see https://git.gnome.org/browse/gtk+/tree/gdk/x11/gdkdevice-xi2.c#n763 (thanks mclasen in irc.gimp.net/#gtk+)

	me->X = x;
	me->Y = y;

	me->ClientWidth = ap->clientWidth;
	me->ClientHeight = ap->clientHeight;
	me->HScrollPos = gtk_adjustment_get_value(ap->ha);
	me->VScrollPos = gtk_adjustment_get_value(ap->va);

	(*(ap->ah->MouseEvent))(ap->ah, ap->a, me);
}

static gboolean areaWidget_button_press_event(GtkWidget *w, GdkEventButton *e)
{
	struct areaPrivate *ap = areaWidget(w)->priv;
	gint maxTime, maxDistance;
	GtkSettings *settings;
	uiAreaMouseEvent me;

	// clicking doesn't automatically transfer keyboard focus; we must do so manually (thanks tristan in irc.gimp.net/#gtk+)
	gtk_widget_grab_focus(w);

	// we handle multiple clicks ourselves here, in the same way as we do on Windows
	if (e->type != GDK_BUTTON_PRESS)
		// ignore GDK's generated double-clicks and beyond
		return GDK_EVENT_PROPAGATE;
	settings = gtk_widget_get_settings(w);
	g_object_get(settings,
		"gtk-double-click-time", &maxTime,
		"gtk-double-click-distance", &maxDistance,
		NULL);
	// TODO unref settings?
	me.Count = clickCounterClick(&(ap->cc), me.Down,
		e->x, e->y,
		e->time, maxTime,
		maxDistance, maxDistance);

	me.Down = e->button;
	me.Up = 0;
	finishMouseEvent(ap, &me, e->button, e->x, e->y, e->state, e->window);
	return GDK_EVENT_PROPAGATE;
}

static gboolean areaWidget_button_release_event(GtkWidget *w, GdkEventButton *e)
{
	struct areaPrivate *ap = areaWidget(w)->priv;
	uiAreaMouseEvent me;

	me.Down = 0;
	me.Up = e->button;
	me.Count = 0;
	finishMouseEvent(ap, &me, e->button, e->x, e->y, e->state, e->window);
	return GDK_EVENT_PROPAGATE;
}

static gboolean areaWidget_motion_notify_event(GtkWidget *w, GdkEventMotion *e)
{
	struct areaPrivate *ap = areaWidget(w)->priv;
	uiAreaMouseEvent me;

	me.Down = 0;
	me.Up = 0;
	me.Count = 0;
	finishMouseEvent(ap, &me, 0, e->x, e->y, e->state, e->window);
	return GDK_EVENT_PROPAGATE;
}

// we want switching away from the control to reset the double-click counter, like with WM_ACTIVATE on Windows
// according to tristan in irc.gimp.net/#gtk+, doing this on enter-notify-event and leave-notify-event is correct (and it seems to be true in my own tests; plus the events DO get sent when switching programs with the keyboard (just pointing that out))
// differentiating between enter-notify-event and leave-notify-event is unimportant
gboolean areaWidget_enterleave_notify_event(GtkWidget *w, GdkEventCrossing *e)
{
	struct areaPrivate *ap = areaWidget(w)->priv;

	clickCounterReset(&(ap->cc));
	return GDK_EVENT_PROPAGATE;
}

// note: there is no equivalent to WM_CAPTURECHANGED on GTK+; there literally is no way to break a grab like that (at least not on X11 and Wayland)
// even if I invoke the task switcher and switch processes, the mouse grab will still be held until I let go of all buttons

// TODO key events

enum {
	// normal properties must come before override properties
	// thanks gregier in irc.gimp.net/#gtk+
	pAreaHandler = 1,
	pHAdjustment,
	pVAdjustment,
	pHScrollPolicy,
	pVScrollPolicy,
	nProps,
};

static GParamSpec *pspecAreaHandler;

static void onValueChanged(GtkAdjustment *a, gpointer data)
{
	// there's no way to scroll the contents of a widget, so we have to redraw the entire thing
	gtk_widget_queue_draw(GTK_WIDGET(data));
}

static void replaceAdjustment(areaWidget *a, GtkAdjustment **adj, const GValue *value)
{
	if (*adj != NULL) {
		g_signal_handlers_disconnect_by_func(*adj, G_CALLBACK(onValueChanged), a);
		g_object_unref(*adj);
	}
	*adj = GTK_ADJUSTMENT(g_value_get_object(value));
	if (*adj != NULL)
		g_object_ref_sink(*adj);
	else
		*adj = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
	g_signal_connect(*adj, "value-changed", G_CALLBACK(onValueChanged), a);
	updateScroll(a);
}

static void areaWidget_set_property(GObject *obj, guint prop, const GValue *value, GParamSpec *pspec)
{
	areaWidget *a = areaWidget(obj);
	struct areaPrivate *ap = a->priv;

	switch (prop) {
	case pHAdjustment:
		replaceAdjustment(a, &(ap->ha), value);
		return;
	case pVAdjustment:
		replaceAdjustment(a, &(ap->va), value);
		return;
	case pHScrollPolicy:
		ap->hpolicy = g_value_get_enum(value);
		return;
	case pVScrollPolicy:
		ap->vpolicy = g_value_get_enum(value);
		return;
	case pAreaHandler:
		ap->ah = (uiAreaHandler *) g_value_get_pointer(value);
		return;
	}
	G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop, pspec);
}

static void areaWidget_get_property(GObject *obj, guint prop, GValue *value, GParamSpec *pspec)
{
	areaWidget *a = areaWidget(obj);
	struct areaPrivate *ap = a->priv;

	switch (prop) {
	case pHAdjustment:
		g_value_set_object(value, ap->ha);
		return;
	case pVAdjustment:
		g_value_set_object(value, ap->va);
		return;
	case pHScrollPolicy:
		g_value_set_enum(value, ap->hpolicy);
		return;
	case pVScrollPolicy:
		g_value_set_enum(value, ap->vpolicy);
		return;
	}
	G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop, pspec);
}

static void areaWidget_class_init(areaWidgetClass *class)
{
	G_OBJECT_CLASS(class)->dispose = areaWidget_dispose;
	G_OBJECT_CLASS(class)->finalize = areaWidget_finalize;
	G_OBJECT_CLASS(class)->set_property = areaWidget_set_property;
	G_OBJECT_CLASS(class)->get_property = areaWidget_get_property;

	GTK_WIDGET_CLASS(class)->size_allocate = areaWidget_size_allocate;
	GTK_WIDGET_CLASS(class)->draw = areaWidget_draw;
//	GTK_WIDGET_CLASS(class)->get_preferred_height = areaWidget_get_preferred_height;
//	GTK_WIDGET_CLASS(class)->get_preferred_width = areaWidget_get_preferred_width;
	GTK_WIDGET_CLASS(class)->button_press_event = areaWidget_button_press_event;
	GTK_WIDGET_CLASS(class)->button_release_event = areaWidget_button_release_event;
	GTK_WIDGET_CLASS(class)->motion_notify_event = areaWidget_motion_notify_event;
	GTK_WIDGET_CLASS(class)->enter_notify_event = areaWidget_enterleave_notify_event;
	GTK_WIDGET_CLASS(class)->leave_notify_event = areaWidget_enterleave_notify_event;
//	GTK_WIDGET_CLASS(class)->key_press_event = areaWidget_key_press_event;
//	GTK_WIDGET_CLASS(class)->key_release_event = areaWidget_key_release_event;

	g_type_class_add_private(G_OBJECT_CLASS(class), sizeof (struct areaPrivate));

	pspecAreaHandler = g_param_spec_pointer("area-handler",
		"area-handler",
		"Area handler.",
		G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	g_object_class_install_property(G_OBJECT_CLASS(class), pAreaHandler, pspecAreaHandler);

	// this is the actual interface implementation
	g_object_class_override_property(G_OBJECT_CLASS(class), pHAdjustment, "hadjustment");
	g_object_class_override_property(G_OBJECT_CLASS(class), pVAdjustment, "vadjustment");
	g_object_class_override_property(G_OBJECT_CLASS(class), pHScrollPolicy, "hscroll-policy");
	g_object_class_override_property(G_OBJECT_CLASS(class), pVScrollPolicy, "vscroll-policy");
}

static void areaWidget_scrollable_init(GtkScrollable *iface)
{
	// no need to do anything; the interface only has properties
}

GtkWidget *newArea(uiAreaHandler *ah)
{
	return GTK_WIDGET(g_object_new(areaWidgetType,
		"area-handler", ah,
		NULL));
}

void areaUpdateScroll(GtkWidget *area)
{
	updateScroll(areaWidget(area));
}

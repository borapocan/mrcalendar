/*
 * mrcalendar.c — Mr.Calendar for MrRobotOS
 * GTK4, single file, fully self-contained
 * Build: gcc `pkg-config --cflags gtk4 gdk-pixbuf-2.0` -g -std=gnu99 -o mrcalendar mrcalendar.c `pkg-config --libs gtk4 gdk-pixbuf-2.0` -lX11
 */

#include <gtk/gtk.h>
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_EVENTS 1024
#define EVENTS_FILE ".config/mrrobotos/mrcalendar/events.dat"
#define ICON_PATH "/usr/share/icons/mrrobotos/scalable/apps/mrcalendar.png"

typedef struct {
	int    year, month, day;
	int    hour, minute;
	char   title[128];
	char   note[512];
	int    color;
} CalEvent;

typedef struct {
	CalEvent events[MAX_EVENTS];
	int      n_events;
	int      cur_year, cur_month, cur_day;
	int      today_year, today_month, today_day;
	GtkWidget *header_label;
	GtkWidget *day_grid;
	GtkWidget *event_panel;
	GtkWidget *event_list;
	GtkWidget *event_day_label;
	GtkWidget *window;
} CalApp;

static CalApp *g_cal = NULL;

static const char *MONTHS[] = {
	"January","February","March","April","May","June",
	"July","August","September","October","November","December"
};
static const char *DAYS_SHORT[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *EVENT_COLOR_NAMES[] = {
	"Blue","Red","Green","Yellow","Purple","Orange"
};

/* ================================================================== */
/* _NET_WM_ICON — set icon directly via Xlib                           */
/* ================================================================== */

static void set_net_wm_icon(GtkWindow *win, const char *path) {
	GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(win));
	if (!surface) return;

	GError *err = NULL;
	GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(path, 48, 48, &err);
	if (!pb) {
		if (err) g_error_free(err);
		return;
	}

	int w = gdk_pixbuf_get_width(pb);
	int h = gdk_pixbuf_get_height(pb);
	guchar *pixels = gdk_pixbuf_get_pixels(pb);
	int rowstride = gdk_pixbuf_get_rowstride(pb);
	int has_alpha = gdk_pixbuf_get_has_alpha(pb);
	int n = 2 + w * h;

	unsigned long *data = g_new(unsigned long, n);
	data[0] = w;
	data[1] = h;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			guchar *p = pixels + y * rowstride + x * (has_alpha ? 4 : 3);
			unsigned long a = has_alpha ? p[3] : 255;
			unsigned long r = p[0], g = p[1], b = p[2];
			data[2 + y * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	Display *dpy = gdk_x11_display_get_xdisplay(gdk_display_get_default());
	Window xwin = gdk_x11_surface_get_xid(surface);
#pragma GCC diagnostic pop

	Atom net_wm_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
	XChangeProperty(dpy, xwin, net_wm_icon, XA_CARDINAL, 32,
			PropModeReplace, (unsigned char *)data, n);
	XFlush(dpy);

	g_free(data);
	g_object_unref(pb);
}

static void on_window_realize(GtkWidget *widget, gpointer ud) {
	set_net_wm_icon(GTK_WINDOW(widget), ICON_PATH);
}

/* ================================================================== */
/* Persistence                                                          */
/* ================================================================== */

static char *get_events_path(void) {
	const char *home = g_get_home_dir();
	return g_strdup_printf("%s/%s", home, EVENTS_FILE);
}

static void save_events(CalApp *ca) {
	char *path = get_events_path();
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0755);
	g_free(dir);
	FILE *f = fopen(path, "w");
	if (!f) { g_free(path); return; }
	for (int i = 0; i < ca->n_events; i++) {
		CalEvent *e = &ca->events[i];
		fprintf(f, "%d|%d|%d|%d|%d|%d|%s|%s\n",
			e->year, e->month, e->day, e->hour, e->minute,
			e->color, e->title, e->note);
	}
	fclose(f);
	g_free(path);
}

static void load_events(CalApp *ca) {
	char *path = get_events_path();
	FILE *f = fopen(path, "r");
	g_free(path);
	if (!f) return;
	char line[1024];
	while (fgets(line, sizeof(line), f) && ca->n_events < MAX_EVENTS) {
		CalEvent *e = &ca->events[ca->n_events];
		char title[128]="", note[512]="";
		if (sscanf(line, "%d|%d|%d|%d|%d|%d|%127[^|]|%511[^\n]",
			   &e->year,&e->month,&e->day,&e->hour,&e->minute,
			   &e->color,title,note) >= 7) {
			strncpy(e->title, title, 127);
			strncpy(e->note,  note,  511);
			ca->n_events++;
		}
	}
	fclose(f);
}

/* ================================================================== */
/* Date helpers                                                         */
/* ================================================================== */

static int days_in_month(int year, int month) {
	static const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
	if (month==2 && ((year%4==0&&year%100!=0)||year%400==0)) return 29;
	return days[month];
}

static int day_of_week(int year, int month, int day) {
	struct tm t = {0};
	t.tm_year=year-1900; t.tm_mon=month-1; t.tm_mday=day;
	mktime(&t);
	return t.tm_wday;
}

static int events_on_day(CalApp *ca, int y, int m, int d) {
	int n=0;
	for (int i=0;i<ca->n_events;i++)
		if (ca->events[i].year==y&&ca->events[i].month==m&&ca->events[i].day==d) n++;
	return n;
}

/* ================================================================== */
/* Forward declarations                                                 */
/* ================================================================== */
static void rebuild_calendar(CalApp *ca);
static void show_day_events(CalApp *ca, int y, int m, int d);
static void open_add_event(CalApp *ca, int y, int m, int d);

/* ================================================================== */
/* Event panel                                                          */
/* ================================================================== */

static void on_delete_event(GtkWidget *btn, gpointer ud) {
	int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "event-idx"));
	CalApp *ca = g_cal;
	if (idx<0||idx>=ca->n_events) return;
	for (int i=idx;i<ca->n_events-1;i++) ca->events[i]=ca->events[i+1];
	ca->n_events--;
	save_events(ca);
	show_day_events(ca, ca->cur_year, ca->cur_month, ca->cur_day);
	rebuild_calendar(ca);
}

static void show_day_events(CalApp *ca, int y, int m, int d) {
	ca->cur_day = d;
	char label[64];
	snprintf(label, sizeof(label), "%s %d, %d", MONTHS[m-1], d, y);
	gtk_label_set_text(GTK_LABEL(ca->event_day_label), label);

	GtkWidget *c;
	while ((c=gtk_widget_get_first_child(ca->event_list)))
		gtk_widget_unparent(c);

	int found = 0;
	for (int i=0;i<ca->n_events;i++) {
		CalEvent *e = &ca->events[i];
		if (e->year!=y||e->month!=m||e->day!=d) continue;
		found++;

		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
		gtk_widget_add_css_class(row, "event-row");
		gtk_widget_set_margin_bottom(row, 6);

		GtkWidget *stripe = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		gtk_widget_set_size_request(stripe, 4, -1);
		char stripe_class[32];
		snprintf(stripe_class, sizeof(stripe_class), "stripe-color-%d", e->color % 6);
		gtk_widget_add_css_class(stripe, "event-stripe");
		gtk_widget_add_css_class(stripe, stripe_class);
		gtk_box_append(GTK_BOX(row), stripe);

		GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		gtk_widget_set_hexpand(info, TRUE);

		char time_str[32];
		if (e->hour>=0&&e->minute>=0)
			snprintf(time_str,sizeof(time_str),"%02d:%02d",e->hour,e->minute);
		else strncpy(time_str,"All day",sizeof(time_str));

		GtkWidget *tl = gtk_label_new(time_str);
		gtk_widget_add_css_class(tl, "event-time");
		gtk_widget_set_halign(tl, GTK_ALIGN_START);
		gtk_box_append(GTK_BOX(info), tl);

		GtkWidget *nl = gtk_label_new(e->title);
		gtk_widget_add_css_class(nl, "event-title");
		gtk_widget_set_halign(nl, GTK_ALIGN_START);
		gtk_label_set_ellipsize(GTK_LABEL(nl), PANGO_ELLIPSIZE_END);
		gtk_box_append(GTK_BOX(info), nl);

		if (e->note[0]) {
			GtkWidget *note = gtk_label_new(e->note);
			gtk_widget_add_css_class(note, "event-note");
			gtk_widget_set_halign(note, GTK_ALIGN_START);
			gtk_label_set_ellipsize(GTK_LABEL(note), PANGO_ELLIPSIZE_END);
			gtk_box_append(GTK_BOX(info), note);
		}
		gtk_box_append(GTK_BOX(row), info);

		GtkWidget *del = gtk_button_new_from_icon_name("user-trash-symbolic");
		gtk_widget_add_css_class(del, "flat");
		gtk_widget_add_css_class(del, "del-btn");
		gtk_widget_set_valign(del, GTK_ALIGN_CENTER);
		g_object_set_data(G_OBJECT(del), "event-idx", GINT_TO_POINTER(i));
		g_signal_connect(del, "clicked", G_CALLBACK(on_delete_event), NULL);
		gtk_box_append(GTK_BOX(row), del);

		gtk_box_append(GTK_BOX(ca->event_list), row);
	}

	if (!found) {
		GtkWidget *empty = gtk_label_new("No events");
		gtk_widget_add_css_class(empty, "dim-label");
		gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
		gtk_widget_set_margin_top(empty, 20);
		gtk_box_append(GTK_BOX(ca->event_list), empty);
	}
}

/* ================================================================== */
/* Add event dialog                                                     */
/* ================================================================== */

typedef struct {
	CalApp    *ca;
	int        y, m, d;
	GtkWidget *title_entry;
	GtkWidget *note_entry;
	GtkWidget *hour_spin;
	GtkWidget *min_spin;
	GtkWidget *allday_sw;
	GtkWidget *color_dd;
	GtkWidget *dialog;
} AddEventData;

static void on_add_confirm(GtkWidget *btn, gpointer ud) {
	AddEventData *ad = ud;
	CalApp *ca = ad->ca;
	if (ca->n_events >= MAX_EVENTS) return;

	const char *title = gtk_editable_get_text(GTK_EDITABLE(ad->title_entry));
	if (!title||!title[0]) return;

	CalEvent *e = &ca->events[ca->n_events++];
	e->year=ad->y; e->month=ad->m; e->day=ad->d;
	strncpy(e->title, title, 127);
	const char *note = gtk_editable_get_text(GTK_EDITABLE(ad->note_entry));
	if (note) strncpy(e->note, note, 511);

	if (gtk_switch_get_active(GTK_SWITCH(ad->allday_sw))) {
		e->hour=-1; e->minute=-1;
	} else {
		e->hour   = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(ad->hour_spin));
		e->minute = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(ad->min_spin));
	}
	e->color = gtk_drop_down_get_selected(GTK_DROP_DOWN(ad->color_dd));

	save_events(ca);
	show_day_events(ca, ad->y, ad->m, ad->d);
	rebuild_calendar(ca);
	gtk_window_destroy(GTK_WINDOW(ad->dialog));
	g_free(ad);
}

static void on_add_cancel(GtkWidget *btn, gpointer ud) {
	AddEventData *ad = ud;
	gtk_window_destroy(GTK_WINDOW(ad->dialog));
	g_free(ad);
}

static void on_allday_toggled(GtkSwitch *sw, GParamSpec *ps, gpointer ud) {
	AddEventData *ad = ud;
	gboolean allday = gtk_switch_get_active(sw);
	gtk_widget_set_sensitive(ad->hour_spin, !allday);
	gtk_widget_set_sensitive(ad->min_spin,  !allday);
}

static void open_add_event(CalApp *ca, int y, int m, int d) {
	AddEventData *ad = g_new0(AddEventData, 1);
	ad->ca=ca; ad->y=y; ad->m=m; ad->d=d;

	GtkWidget *dlg = gtk_window_new();
	ad->dialog = dlg;
	char title[64];
	snprintf(title,sizeof(title),"New Event — %s %d",MONTHS[m-1],d);
	gtk_window_set_title(GTK_WINDOW(dlg), title);
	gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(ca->window));
	gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
	gtk_window_set_default_size(GTK_WINDOW(dlg), 380, -1);
	gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);

	GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
	gtk_widget_set_margin_start(vb,24); gtk_widget_set_margin_end(vb,24);
	gtk_widget_set_margin_top(vb,20);   gtk_widget_set_margin_bottom(vb,20);
	gtk_window_set_child(GTK_WINDOW(dlg), vb);

	GtkWidget *tl = gtk_label_new("Title");
	gtk_widget_set_halign(tl, GTK_ALIGN_START);
	gtk_widget_add_css_class(tl, "caption");
	gtk_box_append(GTK_BOX(vb), tl);
	ad->title_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(ad->title_entry), "Event title");
	gtk_box_append(GTK_BOX(vb), ad->title_entry);

	GtkWidget *nl = gtk_label_new("Note (optional)");
	gtk_widget_set_halign(nl, GTK_ALIGN_START);
	gtk_widget_add_css_class(nl, "caption");
	gtk_box_append(GTK_BOX(vb), nl);
	ad->note_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(ad->note_entry), "Optional note");
	gtk_box_append(GTK_BOX(vb), ad->note_entry);

	GtkWidget *adr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *adl = gtk_label_new("All day");
	gtk_widget_set_hexpand(adl, TRUE);
	gtk_widget_set_halign(adl, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(adr), adl);
	ad->allday_sw = gtk_switch_new();
	gtk_switch_set_active(GTK_SWITCH(ad->allday_sw), TRUE);
	gtk_widget_set_valign(ad->allday_sw, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(adr), ad->allday_sw);
	gtk_box_append(GTK_BOX(vb), adr);

	GtkWidget *tr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *hrl = gtk_label_new("Time");
	gtk_widget_set_halign(hrl, GTK_ALIGN_START);
	gtk_widget_set_hexpand(hrl, TRUE);
	gtk_box_append(GTK_BOX(tr), hrl);
	ad->hour_spin = gtk_spin_button_new_with_range(0,23,1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(ad->hour_spin), 9);
	gtk_widget_set_sensitive(ad->hour_spin, FALSE);
	gtk_box_append(GTK_BOX(tr), ad->hour_spin);
	gtk_box_append(GTK_BOX(tr), gtk_label_new(":"));
	ad->min_spin = gtk_spin_button_new_with_range(0,59,5);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(ad->min_spin), 0);
	gtk_widget_set_sensitive(ad->min_spin, FALSE);
	gtk_box_append(GTK_BOX(tr), ad->min_spin);
	gtk_box_append(GTK_BOX(vb), tr);
	g_signal_connect(ad->allday_sw,"notify::active",G_CALLBACK(on_allday_toggled),ad);

	GtkWidget *cl = gtk_label_new("Color");
	gtk_widget_set_halign(cl, GTK_ALIGN_START);
	gtk_widget_add_css_class(cl, "caption");
	gtk_box_append(GTK_BOX(vb), cl);
	GtkStringList *color_list = gtk_string_list_new(NULL);
	for (int i=0;i<6;i++) gtk_string_list_append(color_list, EVENT_COLOR_NAMES[i]);
	ad->color_dd = gtk_drop_down_new(G_LIST_MODEL(color_list), NULL);
	gtk_box_append(GTK_BOX(vb), ad->color_dd);

	GtkWidget *br = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(br, GTK_ALIGN_END);
	GtkWidget *cancel = gtk_button_new_with_label("Cancel");
	g_signal_connect(cancel,"clicked",G_CALLBACK(on_add_cancel),ad);
	gtk_box_append(GTK_BOX(br), cancel);
	GtkWidget *add = gtk_button_new_with_label("Add Event");
	gtk_widget_add_css_class(add, "suggested-action");
	g_signal_connect(add,"clicked",G_CALLBACK(on_add_confirm),ad);
	gtk_box_append(GTK_BOX(br), add);
	gtk_box_append(GTK_BOX(vb), br);

	gtk_window_set_default_widget(GTK_WINDOW(dlg), add);
	gtk_window_present(GTK_WINDOW(dlg));
}

/* ================================================================== */
/* Calendar grid                                                        */
/* ================================================================== */

typedef struct { CalApp *ca; int y,m,d; } DayData;

static void on_day_clicked(GtkWidget *btn, gpointer ud) {
	DayData *dd = ud;
	CalApp *ca = dd->ca;
	ca->cur_day = dd->d;
	show_day_events(ca, dd->y, dd->m, dd->d);
	rebuild_calendar(ca);
}

static void on_day_add(GtkWidget *btn, gpointer ud) {
	DayData *dd = ud;
	open_add_event(dd->ca, dd->y, dd->m, dd->d);
}

static void rebuild_calendar(CalApp *ca) {
	GtkWidget *c;
	while ((c=gtk_widget_get_first_child(ca->day_grid)))
		gtk_widget_unparent(c);

	for (int d=0;d<7;d++) {
		GtkWidget *lbl = gtk_label_new(DAYS_SHORT[d]);
		gtk_widget_add_css_class(lbl, "day-header");
		gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
		gtk_grid_attach(GTK_GRID(ca->day_grid), lbl, d, 0, 1, 1);
	}

	int first_dow = day_of_week(ca->cur_year, ca->cur_month, 1);
	int n_days    = days_in_month(ca->cur_year, ca->cur_month);
	int row = 1, col = first_dow;

	for (int day=1; day<=n_days; day++) {
		gboolean is_today = (day==ca->today_day &&
				     ca->cur_month==ca->today_month &&
				     ca->cur_year==ca->today_year);
		gboolean is_sel = (day==ca->cur_day);

		GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		gtk_widget_add_css_class(cell, "day-cell");
		if (is_today) gtk_widget_add_css_class(cell, "day-today");
		if (is_sel)   gtk_widget_add_css_class(cell, "day-selected");

		char ds[4]; snprintf(ds,sizeof(ds),"%d",day);
		GtkWidget *day_btn = gtk_button_new_with_label(ds);
		gtk_widget_add_css_class(day_btn, "day-num-btn");
		if (is_today) gtk_widget_add_css_class(day_btn, "day-num-today");
		if (is_sel)   gtk_widget_add_css_class(day_btn, "day-num-sel");

		DayData *dd = g_new0(DayData,1);
		dd->ca=ca; dd->y=ca->cur_year; dd->m=ca->cur_month; dd->d=day;
		g_signal_connect_data(day_btn,"clicked",G_CALLBACK(on_day_clicked),
				      dd,(GClosureNotify)g_free,0);
		gtk_box_append(GTK_BOX(cell), day_btn);

		int n_ev = events_on_day(ca, ca->cur_year, ca->cur_month, day);
		if (n_ev > 0) {
			GtkWidget *dots = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
			gtk_widget_set_halign(dots, GTK_ALIGN_CENTER);
			int show = n_ev > 3 ? 3 : n_ev;
			for (int i=0;i<ca->n_events&&show>0;i++) {
				CalEvent *e = &ca->events[i];
				if (e->year!=ca->cur_year||e->month!=ca->cur_month||e->day!=day) continue;
				GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
				gtk_widget_set_size_request(dot, 6, 6);
				char dot_class[32];
				snprintf(dot_class, sizeof(dot_class), "dot-color-%d", e->color % 6);
				gtk_widget_add_css_class(dot, "event-dot");
				gtk_widget_add_css_class(dot, dot_class);
				gtk_box_append(GTK_BOX(dots), dot);
				show--;
			}
			if (n_ev>3) {
				char more[8]; snprintf(more,sizeof(more),"+%d",n_ev-3);
				GtkWidget *ml = gtk_label_new(more);
				gtk_widget_add_css_class(ml,"dot-more");
				gtk_box_append(GTK_BOX(dots),ml);
			}
			gtk_box_append(GTK_BOX(cell), dots);
		}

		GtkWidget *add_btn = gtk_button_new_from_icon_name("list-add-symbolic");
		gtk_widget_add_css_class(add_btn, "day-add-btn");
		gtk_widget_add_css_class(add_btn, "flat");
		DayData *dd2 = g_new0(DayData,1);
		dd2->ca=ca; dd2->y=ca->cur_year; dd2->m=ca->cur_month; dd2->d=day;
		g_signal_connect_data(add_btn,"clicked",G_CALLBACK(on_day_add),
				      dd2,(GClosureNotify)g_free,0);
		gtk_box_append(GTK_BOX(cell), add_btn);

		gtk_grid_attach(GTK_GRID(ca->day_grid), cell, col, row, 1, 1);
		col++;
		if (col>6) { col=0; row++; }
	}

	char hdr[64];
	snprintf(hdr,sizeof(hdr),"%s  %d", MONTHS[ca->cur_month-1], ca->cur_year);
	gtk_label_set_text(GTK_LABEL(ca->header_label), hdr);

	show_day_events(ca, ca->cur_year, ca->cur_month, ca->cur_day);
}

/* ================================================================== */
/* Navigation callbacks                                                 */
/* ================================================================== */

static void on_prev(GtkWidget *btn, gpointer ud) {
	CalApp *ca = g_cal;
	ca->cur_month--;
	if (ca->cur_month<1) { ca->cur_month=12; ca->cur_year--; }
	ca->cur_day = 1;
	rebuild_calendar(ca);
}

static void on_next(GtkWidget *btn, gpointer ud) {
	CalApp *ca = g_cal;
	ca->cur_month++;
	if (ca->cur_month>12) { ca->cur_month=1; ca->cur_year++; }
	ca->cur_day = 1;
	rebuild_calendar(ca);
}

static void on_today(GtkWidget *btn, gpointer ud) {
	CalApp *ca = g_cal;
	ca->cur_year=ca->today_year;
	ca->cur_month=ca->today_month;
	ca->cur_day=ca->today_day;
	rebuild_calendar(ca);
}

static void on_add_today(GtkWidget *btn, gpointer ud) {
	CalApp *ca = g_cal;
	open_add_event(ca, ca->cur_year, ca->cur_month, ca->cur_day);
}

/* ================================================================== */
/* Activate                                                             */
/* ================================================================== */

static void activate(GtkApplication *app, gpointer ud) {
	CalApp *ca = g_new0(CalApp, 1);
	g_cal = ca;

	time_t now = time(NULL);
	struct tm *t = localtime(&now);
	ca->today_year  = ca->cur_year  = t->tm_year+1900;
	ca->today_month = ca->cur_month = t->tm_mon+1;
	ca->today_day   = ca->cur_day   = t->tm_mday;

	load_events(ca);

	ca->window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(ca->window), "Mr.Calendar");
	gtk_window_set_default_size(GTK_WINDOW(ca->window), 980, 660);
	/* header bar with title and window controls */
	GtkWidget *title_bar = gtk_header_bar_new();
	gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(title_bar), TRUE);
	gtk_header_bar_set_title_widget(GTK_HEADER_BAR(title_bar), gtk_label_new("Mr.Calendar"));
	gtk_window_set_titlebar(GTK_WINDOW(ca->window), title_bar);

	/* set icon after window is realized */
	g_signal_connect(ca->window, "realize",
			 G_CALLBACK(on_window_realize), NULL);

	/* CSS */
	GtkCssProvider *css = gtk_css_provider_new();
	gtk_css_provider_load_from_string(css,
		"window, dialog { background: #f5f5f5; }"
		"* { font-family: 'JetBrains Mono','Fira Mono',monospace; color: #1a1a1a; }"
		"headerbar { background: #ebebeb; border-bottom: 1px solid #d0d0d0; border-radius: 0; box-shadow: none; min-height: 36px; padding: 0 8px; }"
		"headerbar * { color: #1a1a1a; }"
		"headerbar label { font-size: 14px; font-weight: bold; color: #1a1a1a; }"
		"headerbar windowcontrols { background: transparent; }"
		"headerbar windowcontrols button { background: transparent; border: none; border-radius: 0; box-shadow: none; outline: none; padding: 4px 6px; margin: 0; color: #1a1a1a; -gtk-icon-size: 14px; }"
		"headerbar windowcontrols button:hover { background: transparent; border: none; box-shadow: none; }"
		"headerbar windowcontrols button:active { background: transparent; }"
		"entry { background: #fff; color: #1a1a1a; border: 1px solid #ccc; border-radius: 6px; }"
		"entry text { color: #1a1a1a; }"
		"entry placeholder { color: #aaa; }"
		"spinbutton { background: #fff; color: #1a1a1a; border: 1px solid #ccc; }"
		"spinbutton text { color: #1a1a1a; }"
		"label { color: #1a1a1a; }"
		"popover, popover contents { background: #fff; color: #1a1a1a; }"
		"popover label { color: #1a1a1a; }"
		"listview row { background: #fff; color: #1a1a1a; }"
		"listview row:hover { background: #f0f0f0; }"
		"listview row label { color: #1a1a1a; }"
		"dropdownbutton, dropdown { background: #fff; color: #1a1a1a; border: 1px solid #ccc; border-radius: 6px; }"
		"button { background: #e8e8e8; color: #1a1a1a; border: 1px solid #ccc; }"
		"button:hover { background: #ddd; }"
		"button.suggested-action { background: #cc2222; color: #fff; border: none; }"
		"button.suggested-action:hover { background: #ee3333; }"
		".cal-header { background: #ececec; border-bottom: 1px solid #ddd; padding: 10px 18px; }"
		".cal-title { font-size: 1.5em; font-weight: bold; color: #cc2222; }"
		".nav-btn { border-radius: 8px; min-width:36px; min-height:36px; background: #e0e0e0; border: 1px solid #ccc; color: #444; }"
		".nav-btn:hover { background: #d0d0d0; color: #000; }"
		".today-btn { background: #fde8e8; color: #cc2222; border-radius:8px; padding: 4px 14px; border: 1px solid #cc2222; font-size:0.85em; }"
		".today-btn:hover { background: #fcd0d0; }"
		".add-btn-main { background: #cc2222; color: #fff; border-radius:8px; padding: 4px 16px; font-weight:bold; font-size:0.85em; border:none; }"
		".add-btn-main:hover { background: #ee3333; }"
		".day-header { font-size:0.75em; color:#999; padding:8px 0 4px 0; font-weight:bold; letter-spacing:0.06em; }"
		".day-cell { border-radius:10px; padding:4px 2px; margin:2px; background: #fff; min-width:80px; min-height:72px; }"
		".day-cell:hover { background: #f0f0f0; }"
		".day-today { background: #fde8e8; border: 1px solid #cc2222; }"
		".day-selected { background: #e8eef8; border: 1px solid #3a6aaa; }"
		".day-num-btn { background: transparent; border: none; border-radius:6px; font-size:0.9em; color:#333; min-width:28px; min-height:28px; padding:0; }"
		".day-num-btn:hover { background: #e0e0e0; color:#000; }"
		".day-num-today { color: #cc2222; font-weight:bold; }"
		".day-num-sel { color: #3a6aaa; font-weight:bold; }"
		".day-add-btn { min-width:18px; min-height:18px; padding:0; opacity:0; }"
		".day-cell:hover .day-add-btn { opacity:0.5; }"
		".day-cell:hover .day-add-btn:hover { opacity:1.0; }"
		".dot-more { font-size:0.6em; color:#888; }"
		".event-stripe { border-radius: 2px; }"
		".stripe-color-0 { background: #3584e4; }"
		".stripe-color-1 { background: #e04343; }"
		".stripe-color-2 { background: #2ec27e; }"
		".stripe-color-3 { background: #e5a50a; }"
		".stripe-color-4 { background: #9141ac; }"
		".stripe-color-5 { background: #c64600; }"
		".event-dot { border-radius: 3px; }"
		".dot-color-0 { background: #3584e4; }"
		".dot-color-1 { background: #e04343; }"
		".dot-color-2 { background: #2ec27e; }"
		".dot-color-3 { background: #e5a50a; }"
		".dot-color-4 { background: #9141ac; }"
		".dot-color-5 { background: #c64600; }"
		".event-panel { background: #ececec; border-left: 1px solid #ddd; }"
		".event-panel-title { font-size:1.05em; font-weight:bold; color:#cc2222; padding:14px 16px 4px 16px; }"
		".event-day-label { font-size:0.8em; color:#888; padding: 0 16px 10px 16px; }"
		".event-row { background:#fff; border-radius:10px; padding:10px; margin: 0 10px 6px 10px; }"
		".event-time { font-size:0.72em; color:#888; }"
		".event-title { font-size:0.92em; color:#1a1a1a; font-weight:bold; }"
		".event-note { font-size:0.78em; color:#666; }"
		".del-btn { color:#cc2222; opacity:0.4; min-width:28px; min-height:28px; }"
		".del-btn:hover { opacity:1.0; }"
		".add-event-btn { background:#fde8e8; color:#cc2222; border-radius:8px; margin:8px 10px 12px 10px; border:1px solid #cc2222; }"
		".add-event-btn:hover { background:#fcd0d0; }"
	);
	gtk_style_context_add_provider_for_display(
						   gdk_display_get_default(),
						   GTK_STYLE_PROVIDER(css),
						   GTK_STYLE_PROVIDER_PRIORITY_USER);
	g_object_unref(css);

	GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_window_set_child(GTK_WINDOW(ca->window), root);

	GtkWidget *hbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_widget_add_css_class(hbar, "cal-header");

	GtkWidget *prev = gtk_button_new_from_icon_name("go-previous-symbolic");
	gtk_widget_add_css_class(prev, "nav-btn");
	g_signal_connect(prev,"clicked",G_CALLBACK(on_prev),NULL);
	gtk_box_append(GTK_BOX(hbar), prev);

	GtkWidget *next = gtk_button_new_from_icon_name("go-next-symbolic");
	gtk_widget_add_css_class(next, "nav-btn");
	g_signal_connect(next,"clicked",G_CALLBACK(on_next),NULL);
	gtk_box_append(GTK_BOX(hbar), next);

	ca->header_label = gtk_label_new("");
	gtk_widget_add_css_class(ca->header_label, "cal-title");
	gtk_widget_set_hexpand(ca->header_label, TRUE);
	gtk_widget_set_halign(ca->header_label, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(hbar), ca->header_label);

	GtkWidget *today_btn = gtk_button_new_with_label("Today");
	gtk_widget_add_css_class(today_btn, "today-btn");
	g_signal_connect(today_btn,"clicked",G_CALLBACK(on_today),NULL);
	gtk_box_append(GTK_BOX(hbar), today_btn);

	GtkWidget *add_main = gtk_button_new_with_label("+ Event");
	gtk_widget_add_css_class(add_main, "add-btn-main");
	g_signal_connect(add_main,"clicked",G_CALLBACK(on_add_today),NULL);
	gtk_box_append(GTK_BOX(hbar), add_main);

	gtk_box_append(GTK_BOX(root), hbar);

	GtkWidget *main_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_vexpand(main_row, TRUE);
	gtk_box_append(GTK_BOX(root), main_row);

	GtkWidget *cal_pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_hexpand(cal_pane, TRUE);
	gtk_widget_set_margin_start(cal_pane, 8);
	gtk_widget_set_margin_end(cal_pane, 4);
	gtk_widget_set_margin_top(cal_pane, 8);
	gtk_widget_set_margin_bottom(cal_pane, 8);

	ca->day_grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(ca->day_grid), 2);
	gtk_grid_set_column_spacing(GTK_GRID(ca->day_grid), 2);
	gtk_grid_set_column_homogeneous(GTK_GRID(ca->day_grid), TRUE);
	gtk_grid_set_row_homogeneous(GTK_GRID(ca->day_grid), FALSE);
	gtk_widget_set_hexpand(ca->day_grid, TRUE);
	gtk_widget_set_vexpand(ca->day_grid, TRUE);
	gtk_box_append(GTK_BOX(cal_pane), ca->day_grid);
	gtk_box_append(GTK_BOX(main_row), cal_pane);

	GtkWidget *ep = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(ep, "event-panel");
	gtk_widget_set_size_request(ep, 270, -1);
	ca->event_panel = ep;

	GtkWidget *ep_title = gtk_label_new("Events");
	gtk_widget_add_css_class(ep_title, "event-panel-title");
	gtk_widget_set_halign(ep_title, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(ep), ep_title);

	ca->event_day_label = gtk_label_new("");
	gtk_widget_add_css_class(ca->event_day_label, "event-day-label");
	gtk_widget_set_halign(ca->event_day_label, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(ep), ca->event_day_label);

	gtk_box_append(GTK_BOX(ep), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

	GtkWidget *ep_scr = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ep_scr),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(ep_scr, TRUE);
	ca->event_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_margin_top(ca->event_list, 8);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ep_scr), ca->event_list);
	gtk_box_append(GTK_BOX(ep), ep_scr);

	GtkWidget *add_ep = gtk_button_new_with_label("+ Add Event");
	gtk_widget_add_css_class(add_ep, "add-event-btn");
	g_signal_connect(add_ep,"clicked",G_CALLBACK(on_add_today),NULL);
	gtk_box_append(GTK_BOX(ep), add_ep);

	gtk_box_append(GTK_BOX(main_row), ep);

	rebuild_calendar(ca);
	gtk_widget_set_visible(ca->window, TRUE);
}

int main(int argc, char **argv) {
	GtkApplication *app = gtk_application_new(
						  "org.mrrobotos.mrcalendar", G_APPLICATION_NON_UNIQUE);
	g_signal_connect(app,"activate",G_CALLBACK(activate),NULL);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}

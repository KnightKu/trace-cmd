/*
 * Copyright (C) 2009, Steven Rostedt <srostedt@redhat.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <getopt.h>

#include "trace-compat.h"
#include "trace-cmd.h"
#include "kernel-shark.h"
#include "version.h"

#define DEBUG_LEVEL	0
#if DEBUG_LEVEL > 0
# define dprintf(l, x...)			\
	do {					\
		if (l <= DEBUG_LEVEL)		\
			printf(x);		\
	} while (0)
#else
# define dprintf(l, x...)	do { if (0) printf(x); } while (0)
#endif

#define TRACE_WIDTH	800
#define TRACE_HEIGHT	600

#define default_input_file "trace.dat"
static char *input_file;

void usage(char *prog)
{
	printf("Usage: %s\n", prog);
	printf("  -h	Display this help message\n");
	printf("  -v	Display version and exit\n");
	printf("  -i	input_file, default is %s\n", default_input_file);
}

/* graph callbacks */

/* convert_nano() and print_time() are copied from trace-graph.c for debugging
   purposes, and should be deleted when this is complete (or merged with
   trace-graph.c */

static void convert_nano(unsigned long long time, unsigned long *sec,
			 unsigned long *usec)
{
	*sec = time / 1000000000ULL;
	*usec = (time / 1000) % 1000000;
}

static void print_time(unsigned long long time)
{
	unsigned long sec, usec;

	if (!DEBUG_LEVEL)
		return;

	convert_nano(time, &sec, &usec);
	printf("%lu.%06lu", sec, usec);
}

static void ks_graph_select(struct graph_info *ginfo, guint64 cursor)
{
	struct graph_callbacks *cbs;
	struct shark_info *info;

	dprintf(1, "Cursor: ");
	print_time(cursor);
	dprintf(1, " selected\n");

	cbs = trace_graph_get_callbacks(ginfo);
	info = container_of(cbs, struct shark_info, graph_cbs);

	trace_view_select(info->treeview, cursor);
}

static void ks_graph_filter(struct graph_info *ginfo,
			    struct filter_task *task_filter,
			    struct filter_task *hide_tasks)
{
	struct graph_callbacks *cbs;
	struct shark_info *info;

	cbs = trace_graph_get_callbacks(ginfo);
	info = container_of(cbs, struct shark_info, graph_cbs);

	if (info->list_filter_enabled)
		trace_view_update_filters(info->treeview,
					  task_filter, hide_tasks);
}

static void free_info(struct shark_info *info)
{
	tracecmd_close(info->handle);
	free(info->ginfo);
	free(info);
}

/* Callback for the clicked signal of the Load button */
static void
load_clicked (gpointer data)
{
	struct shark_info *info = data;
	struct tracecmd_input *handle;
	GtkWidget *dialog;
	gchar *filename;

	dialog = gtk_file_chooser_dialog_new("Load File",
					     NULL,
					     GTK_FILE_CHOOSER_ACTION_OPEN,
					     GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
					     GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
					     NULL);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		handle = tracecmd_open(filename);
		if (handle) {
			tracecmd_close(info->handle);
			info->handle = handle;
			trace_graph_load_handle(info->ginfo, handle);
			trace_view_reload(info->treeview, handle, info->spin);
		}
		g_free(filename);
	}
	gtk_widget_destroy(dialog);
}

/* Callback for the clicked signal of the Exit button */
static void
exit_clicked (gpointer data)
{
	struct shark_info *info = data;

	gtk_widget_destroy (info->window); /* the user data points to the main window */
	free_info(info);
	gtk_main_quit ();
}

/* Callback for the delete_event signal of the main application window */
static gint
delete_event (GtkWidget *widget, GdkEvent *event, gpointer data)
{
	struct shark_info *info = data;

	gtk_widget_destroy (widget); /* destroy the main window */
	free_info(info);
	gtk_main_quit ();
	return TRUE;
}

/* Callback for the clicked signal of the Events filter button */
static void
list_events_clicked (gpointer data)
{
	struct shark_info *info = data;
	GtkTreeView *trace_tree = GTK_TREE_VIEW(info->treeview);
	GtkTreeModel *model;
	TraceViewStore *store;
	gboolean all_events;
	gchar **systems;
	gint *events;

	model = gtk_tree_view_get_model(trace_tree);
	if (!model)
		return;

	store = TRACE_VIEW_STORE(model);

	all_events = trace_view_store_get_all_events_enabled(store);
	systems = trace_view_store_get_systems_enabled(store);
	events = trace_view_store_get_events_enabled(store);

	trace_filter_event_dialog(store->handle, all_events,
				  systems, events,
				  trace_view_event_filter_callback, info->treeview);
}

static void
graph_events_clicked (gpointer data)
{
	struct shark_info *info = data;
	struct graph_info *ginfo = info->ginfo;
	gboolean all_events;
	gchar **systems;
	gint *events;

	all_events = ginfo->all_events;
	systems = ginfo->systems;
	events = ginfo->event_ids;

	trace_filter_event_dialog(info->handle, all_events,
				  systems, events,
				  trace_graph_event_filter_callback, ginfo);
}

static void
sync_graph_events_to_list_clicked (gpointer data)
{
	struct shark_info *info = data;
	GtkTreeView *trace_tree = GTK_TREE_VIEW(info->treeview);
	GtkTreeModel *model;
	TraceViewStore *store;
	gboolean all_events;
	gchar **systems;
	gint *events;

	model = gtk_tree_view_get_model(trace_tree);
	if (!model)
		return;

	store = TRACE_VIEW_STORE(model);

	all_events = trace_view_store_get_all_events_enabled(store);
	systems = trace_view_store_get_systems_enabled(store);
	events = trace_view_store_get_events_enabled(store);

	trace_graph_event_filter_callback(TRUE, all_events, systems,
					  events, info->ginfo);
}


static void
sync_list_events_to_graph_clicked (gpointer data)
{
	struct shark_info *info = data;
	struct graph_info *ginfo = info->ginfo;
	gboolean all_events;
	gchar **systems;
	gint *events;

	all_events = ginfo->all_events;
	systems = ginfo->systems;
	events = ginfo->event_ids;

	trace_view_event_filter_callback(TRUE, all_events, systems,
					 events, info->treeview);
}

/* Callback for the clicked signal of the CPUs filter button */
static void
cpus_clicked (gpointer data)
{
	struct shark_info *info = data;
	GtkTreeView *trace_tree = GTK_TREE_VIEW(info->treeview);
	TraceViewStore *store;
	gboolean all_cpus;
	guint64 *cpu_mask;

	store = TRACE_VIEW_STORE(gtk_tree_view_get_model(trace_tree));

	all_cpus = trace_view_store_get_all_cpus(store);
	cpu_mask = trace_view_store_get_cpu_mask(store);

	trace_filter_cpu_dialog(all_cpus, cpu_mask,
				trace_view_store_get_cpus(store),
				trace_view_cpu_filter_callback, trace_tree);
}

static void row_double_clicked(GtkTreeView        *treeview,
			       GtkTreePath        *path,
			       GtkTreeViewColumn  *col,
			       gpointer            data)
{
	struct shark_info *info = data;
	TraceViewRecord *rec;
	GtkTreeModel *model;
	gchar *spath;
	guint64 time;
	gint row;

	model = gtk_tree_view_get_model(treeview);
	/* This can be called when we NULL out the model */
	if (!model)
		return;

	spath = gtk_tree_path_to_string(path);
	row = atoi(spath);
	g_free(spath);

	rec = trace_view_store_get_row(TRACE_VIEW_STORE(model), row);
	time = rec->timestamp;
	trace_graph_select_by_time(info->ginfo, time);
}

static void
filter_graph_enable_clicked (gpointer data)
{
	struct shark_info *info = data;

	trace_graph_filter_toggle(info->ginfo);
}

static void
filter_list_enable_clicked (gpointer data)
{
	struct shark_info *info = data;

	info->list_filter_enabled ^= 1;

	if (info->list_filter_enabled)
		trace_view_update_filters(info->treeview,
					  info->ginfo->task_filter,
					  info->ginfo->hide_tasks);
	else
		trace_view_update_filters(info->treeview, NULL, NULL);
}

static void
filter_add_task_clicked (gpointer data)
{
	struct shark_info *info = data;

	trace_graph_filter_add_remove_task(info->ginfo, info->selected_task);

	if (!filter_task_count(info->ginfo->task_filter))
		info->list_filter_enabled = 0;
}

static void
filter_hide_task_clicked (gpointer data)
{
	struct shark_info *info = data;

	trace_graph_filter_hide_show_task(info->ginfo, info->selected_task);

	if (!filter_task_count(info->ginfo->task_filter) &&
	    !filter_task_count(info->ginfo->hide_tasks))
		info->list_filter_enabled = 0;
}

static void
filter_clear_tasks_clicked (gpointer data)
{
	struct shark_info *info = data;

	trace_graph_clear_tasks(info->ginfo);

	info->list_filter_enabled = 0;
}

static gboolean
do_tree_popup(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	struct shark_info *info = data;
	struct graph_info *ginfo = info->ginfo;
	static GtkWidget *menu;
	static GtkWidget *menu_filter_graph_enable;
	static GtkWidget *menu_filter_list_enable;
	static GtkWidget *menu_filter_add_task;
	static GtkWidget *menu_filter_hide_task;
	static GtkWidget *menu_filter_clear_tasks;
	struct record *record;
	TraceViewRecord *vrec;
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreePath *path;
	const char *comm;
	gchar *text;
	gint pid;
	gint len;
	GList *glist;
	gchar *spath;
	guint64 offset;
	gint row;
	gint cpu;

	if (!menu) {
		menu = gtk_menu_new();
		menu_filter_graph_enable = gtk_menu_item_new_with_label("Enable Graph Filter");
		gtk_widget_show(menu_filter_graph_enable);
		gtk_menu_shell_append(GTK_MENU_SHELL (menu), menu_filter_graph_enable);

		g_signal_connect_swapped (G_OBJECT (menu_filter_graph_enable), "activate",
					  G_CALLBACK (filter_graph_enable_clicked),
					  data);

		menu_filter_list_enable = gtk_menu_item_new_with_label("Enable List Filter");
		gtk_widget_show(menu_filter_list_enable);
		gtk_menu_shell_append(GTK_MENU_SHELL (menu), menu_filter_list_enable);

		g_signal_connect_swapped (G_OBJECT (menu_filter_list_enable), "activate",
					  G_CALLBACK (filter_list_enable_clicked),
					  data);

		menu_filter_add_task = gtk_menu_item_new_with_label("Add Task");
		gtk_widget_show(menu_filter_add_task);
		gtk_menu_shell_append(GTK_MENU_SHELL (menu), menu_filter_add_task);

		g_signal_connect_swapped (G_OBJECT (menu_filter_add_task), "activate",
					  G_CALLBACK (filter_add_task_clicked),
					  data);

		menu_filter_hide_task = gtk_menu_item_new_with_label("Hide Task");
		gtk_widget_show(menu_filter_hide_task);
		gtk_menu_shell_append(GTK_MENU_SHELL (menu), menu_filter_hide_task);

		g_signal_connect_swapped (G_OBJECT (menu_filter_hide_task), "activate",
					  G_CALLBACK (filter_hide_task_clicked),
					  data);

		menu_filter_clear_tasks = gtk_menu_item_new_with_label("Clear Task Filter");
		gtk_widget_show(menu_filter_clear_tasks);
		gtk_menu_shell_append(GTK_MENU_SHELL (menu), menu_filter_clear_tasks);

		g_signal_connect_swapped (G_OBJECT (menu_filter_clear_tasks), "activate",
					  G_CALLBACK (filter_clear_tasks_clicked),
					  data);

	}

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(info->treeview));
	glist = gtk_tree_selection_get_selected_rows(GTK_TREE_SELECTION(selection), NULL);
	if (glist) {
		path = glist->data;
		g_list_free(glist);
		spath = gtk_tree_path_to_string(path);
		gtk_tree_path_free(path);
		row = atoi(spath);
		g_free(spath);

		model = gtk_tree_view_get_model(GTK_TREE_VIEW(info->treeview));
		vrec = trace_view_store_get_row(TRACE_VIEW_STORE(model), row);
		offset = vrec->offset;

		record = tracecmd_read_at(info->handle, offset, &cpu);

		if (record) {
			pid = pevent_data_pid(ginfo->pevent, record);
			comm = pevent_data_comm_from_pid(ginfo->pevent, pid);

			len = strlen(comm) + 50;

			text = g_malloc(len);
			g_assert(text);

			if (trace_graph_filter_task_find_pid(ginfo, pid))
				snprintf(text, len, "Remove %s-%d to filter", comm, pid);
			else
				snprintf(text, len, "Add %s-%d to filter", comm, pid);

			ginfo->filter_task_selected = pid;

			gtk_menu_item_set_label(GTK_MENU_ITEM(menu_filter_add_task),
						text);

			if (trace_graph_hide_task_find_pid(ginfo, pid))
				snprintf(text, len, "Show %s-%d", comm, pid);
			else
				snprintf(text, len, "Hide %s-%d", comm, pid);

			gtk_menu_item_set_label(GTK_MENU_ITEM(menu_filter_hide_task),
						text);

			g_free(text);

			info->selected_task = pid;

			gtk_widget_show(menu_filter_add_task);
			gtk_widget_show(menu_filter_hide_task);
			free_record(record);
		}
	} else {
		gtk_widget_hide(menu_filter_add_task);
		gtk_widget_hide(menu_filter_hide_task);
	}

	if (ginfo->filter_enabled)
		gtk_menu_item_set_label(GTK_MENU_ITEM(menu_filter_graph_enable),
					"Disable Graph Filter");
	else
		gtk_menu_item_set_label(GTK_MENU_ITEM(menu_filter_graph_enable),
					"Enable Graph Filter");

	if (info->list_filter_enabled)
		gtk_menu_item_set_label(GTK_MENU_ITEM(menu_filter_list_enable),
					"Disable List Filter");
	else
		gtk_menu_item_set_label(GTK_MENU_ITEM(menu_filter_list_enable),
					"Enable List Filter");

	if (ginfo->filter_available) {
		gtk_widget_set_sensitive(menu_filter_graph_enable, TRUE);
		gtk_widget_set_sensitive(menu_filter_list_enable, TRUE);
	} else {
		gtk_widget_set_sensitive(menu_filter_graph_enable, FALSE);
		gtk_widget_set_sensitive(menu_filter_list_enable, FALSE);
	}

	if (filter_task_count(ginfo->task_filter) ||
	    filter_task_count(ginfo->hide_tasks))
		gtk_widget_set_sensitive(menu_filter_clear_tasks, TRUE);
	else
		gtk_widget_set_sensitive(menu_filter_clear_tasks, FALSE);
		
	gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 3,
		       gtk_get_current_event_time());

	return TRUE;
}

static gboolean
button_press_event(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	if (event->button == 3)
		return do_tree_popup(widget, event, data);

	return FALSE;
}

void kernel_shark(int argc, char **argv)
{
	struct tracecmd_input *handle;
	struct shark_info *info;
	struct stat st;
	GtkWidget *window;
	GtkWidget *vbox;
	GtkWidget *vbox2;
	GtkWidget *vpaned;
	GtkWidget *hbox;
	GtkWidget *menu_bar;
	GtkWidget *menu;
	GtkWidget *menu_item;
	GtkWidget *sub_item;
	GtkWidget *scrollwin;
	GtkWidget *widget;
	GtkWidget *label;
	GtkWidget *spin;
	int ret;
	int c;

	while ((c = getopt(argc, argv, "hvi:")) != -1) {
		switch(c) {
		case 'h':
			usage(basename(argv[0]));
			return;
		case 'v':
			printf("%s - %s\n",
			       basename(argv[0]),
			       VERSION_STRING);
			return;
		case 'i':
			input_file = optarg;
			break;
		default:
			/* assume the other options are for gtk */
			break;
		}
	}

	info = g_new0(typeof(*info), 1);
	if (!info)
		die("Unable to allocate info");

	if (!input_file) {
		ret = stat(default_input_file, &st);
		if (ret >= 0)
			input_file = default_input_file;
	}
	if (handle)
		handle = tracecmd_open(input_file);

	info->handle = handle;

	gtk_init(&argc, &argv);

	/* --- Main window --- */

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	info->window = window;

	/* --- Top Level Vbox --- */

	vbox = gtk_vbox_new(FALSE, 0);
	gtk_container_add(GTK_CONTAINER (window), vbox);
	gtk_widget_show(vbox);

	/* --- Menu Bar --- */

	menu_bar = gtk_menu_bar_new();
	gtk_box_pack_start(GTK_BOX (vbox), menu_bar, FALSE, FALSE, 0);
	gtk_widget_show(menu_bar);

	/* --- File Option --- */

	menu_item = gtk_menu_item_new_with_label("File");
	gtk_widget_show(menu_item);

	gtk_menu_bar_append(GTK_MENU_BAR (menu_bar), menu_item);

	menu = gtk_menu_new();    /* Don't need to show menus */


	/* --- File - Load Option --- */

	sub_item = gtk_menu_item_new_with_label("Load info");

	/* Add them to the menu */
	gtk_menu_shell_append(GTK_MENU_SHELL (menu), sub_item);

	/* We can attach the Quit menu item to our exit function */
	g_signal_connect_swapped (G_OBJECT (sub_item), "activate",
				  G_CALLBACK (load_clicked),
				  (gpointer) info);

	/* We do need to show menu items */
	gtk_widget_show(sub_item);


	/* --- File - Quit Option --- */

	sub_item = gtk_menu_item_new_with_label("Quit");

	/* Add them to the menu */
	gtk_menu_shell_append(GTK_MENU_SHELL (menu), sub_item);

	/* We can attach the Quit menu item to our exit function */
	g_signal_connect_swapped (G_OBJECT (sub_item), "activate",
				  G_CALLBACK (exit_clicked),
				  (gpointer) info);

	/* We do need to show menu items */
	gtk_widget_show(sub_item);

	gtk_menu_item_set_submenu(GTK_MENU_ITEM (menu_item), menu);

	/* --- end File options --- */

	/* --- Filter Option --- */

	menu_item = gtk_menu_item_new_with_label("Filter");
	gtk_widget_show(menu_item);

	gtk_menu_bar_append(GTK_MENU_BAR (menu_bar), menu_item);

	menu = gtk_menu_new();    /* Don't need to show menus */


	/* --- Filter - List Events Option --- */

	sub_item = gtk_menu_item_new_with_label("list events");

	/* Add them to the menu */
	gtk_menu_shell_append(GTK_MENU_SHELL (menu), sub_item);

	/* We can attach the Quit menu item to our exit function */
	g_signal_connect_swapped (G_OBJECT (sub_item), "activate",
				  G_CALLBACK (list_events_clicked),
				  (gpointer) info);

	/* We do need to show menu items */
	gtk_widget_show(sub_item);


	/* --- Filter - Events Option --- */

	sub_item = gtk_menu_item_new_with_label("graph events");

	/* Add them to the menu */
	gtk_menu_shell_append(GTK_MENU_SHELL (menu), sub_item);

	/* We can attach the Quit menu item to our exit function */
	g_signal_connect_swapped (G_OBJECT (sub_item), "activate",
				  G_CALLBACK (graph_events_clicked),
				  (gpointer) info);

	/* We do need to show menu items */
	gtk_widget_show(sub_item);


	/* --- Filter - Events Option --- */

	sub_item = gtk_menu_item_new_with_label("sync graph events to list");

	/* Add them to the menu */
	gtk_menu_shell_append(GTK_MENU_SHELL (menu), sub_item);

	/* We can attach the Quit menu item to our exit function */
	g_signal_connect_swapped (G_OBJECT (sub_item), "activate",
				  G_CALLBACK (sync_graph_events_to_list_clicked),
				  (gpointer) info);

	/* We do need to show menu items */
	gtk_widget_show(sub_item);


	/* --- Filter - Events Option --- */

	sub_item = gtk_menu_item_new_with_label("sync list events to graph");

	/* Add them to the menu */
	gtk_menu_shell_append(GTK_MENU_SHELL (menu), sub_item);

	/* We can attach the Quit menu item to our exit function */
	g_signal_connect_swapped (G_OBJECT (sub_item), "activate",
				  G_CALLBACK (sync_list_events_to_graph_clicked),
				  (gpointer) info);

	/* We do need to show menu items */
	gtk_widget_show(sub_item);


	/* --- Filter - CPUs Option --- */

	sub_item = gtk_menu_item_new_with_label("CPUs");

	/* Add them to the menu */
	gtk_menu_shell_append(GTK_MENU_SHELL (menu), sub_item);

	/* We can attach the Quit menu item to our exit function */
	g_signal_connect_swapped (G_OBJECT (sub_item), "activate",
				  G_CALLBACK (cpus_clicked),
				  (gpointer) info);

	/* We do need to show menu items */
	gtk_widget_show(sub_item);


	/* --- End Filter Options --- */
	gtk_menu_item_set_submenu(GTK_MENU_ITEM (menu_item), menu);

	/* --- Top Level Vpaned --- */

	vpaned = gtk_vpaned_new();
	gtk_box_pack_start(GTK_BOX(vbox), vpaned, TRUE, TRUE, 0);
	gtk_widget_show(vpaned);
	gtk_paned_set_position(GTK_PANED(vpaned), TRACE_HEIGHT / 2);


	/* --- Set up Graph --- */

	info->graph_cbs.select = ks_graph_select;
	info->graph_cbs.filter = ks_graph_filter;

	info->ginfo = trace_graph_create_with_callbacks(handle, &info->graph_cbs);
	widget = trace_graph_get_window(info->ginfo);
	gtk_paned_add1(GTK_PANED(vpaned), widget);
	gtk_widget_show(widget);


	/* --- Tree View Vbox --- */

	vbox2 = gtk_vbox_new(FALSE, 0);
	gtk_paned_add2(GTK_PANED(vpaned), vbox2);
	gtk_widget_show(vbox2);

	/* --- Paging Hbox --- */

	hbox = gtk_hbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox2), hbox, FALSE, FALSE, 0);
	gtk_widget_show(hbox);

	/* --- Page Spin Button --- */

	label = gtk_label_new("Page");
	gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
	gtk_widget_show(label);

	spin = gtk_spin_button_new(NULL, 1.0, 0);
	gtk_spin_button_set_range(GTK_SPIN_BUTTON(spin), 1, 1);
	gtk_box_pack_start(GTK_BOX(hbox), spin, FALSE, FALSE, 0);
	gtk_widget_show(spin);

	info->spin = spin;

	/* --- Search --- */

	/* The tree needs its columns loaded now */
	info->treeview = gtk_tree_view_new();
	trace_view_load(info->treeview, handle, spin);

	label = gtk_label_new("      Search: ");
	gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
	gtk_widget_show(label);

	trace_view_search_setup(GTK_BOX(hbox), GTK_TREE_VIEW(info->treeview));

	/* --- Top Level Trace View Paging Hbox --- */

	hbox = gtk_hbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox2), hbox, TRUE, TRUE, 0);
	gtk_widget_show(hbox);

	/* --- Scroll Window --- */
	scrollwin = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrollwin),
				       GTK_POLICY_AUTOMATIC,
				       GTK_POLICY_AUTOMATIC);
	gtk_box_pack_start(GTK_BOX (hbox), scrollwin, TRUE, TRUE, 0);
	gtk_widget_show(scrollwin);

	/* --- Set up Trace Tree --- */

	g_signal_connect(info->treeview, "row-activated",
			 (GCallback)row_double_clicked, info);

	gtk_container_add(GTK_CONTAINER(scrollwin), info->treeview);

	gtk_signal_connect(GTK_OBJECT(info->treeview), "button_press_event",
			   (GtkSignalFunc) button_press_event, info);

	gtk_widget_show(info->treeview);


	/**********************************************
	 *   Main Window
	 **********************************************/

	/* Connect to the delete_event signal and Run the application */

	gtk_signal_connect (GTK_OBJECT (window), "delete_event",
			    (GtkSignalFunc) delete_event,
			    (gpointer) info);

	gtk_widget_set_size_request(window, TRACE_WIDTH, TRACE_HEIGHT);

	gtk_widget_show (window);
	gtk_main ();
}

int main(int argc, char **argv)
{
	kernel_shark(argc, argv);
	return 0;
}
